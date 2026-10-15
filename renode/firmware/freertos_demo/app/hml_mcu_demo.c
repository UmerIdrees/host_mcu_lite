/*
 * hml_mcu_demo.c -- the check body of the Renode MCU smoke demo.
 *
 * Every check below calls the shipping holoscan_mcu_lite library. Nothing here
 * re-implements, stubs or short-circuits a library code path: if a codec, a
 * CRC or the reassembler is wrong, the matching check fails and the image
 * prints HML-MCU-DEMO-FAIL, which makes the Renode robot test fail. That is
 * the entire value of this file.
 *
 * WHAT IS EXERCISED
 *   - hml_crc16 / hml_crc32_ieee against the published check vectors
 *   - the discovery beacon codec: round trip and every documented rejection
 *   - the control codec: round trip, and bad magic / version / length /
 *     trailing byte / CRC, plus the hml_dest_cfg codec's reserved-byte rules
 *   - the frame codec: round trip, bad magic, version, header CRC, payload CRC
 *   - the reassembler: a 3-fragment frame, a corrupt fragment in the middle,
 *     recovery on the next clean frame, an EOF-only flush, and a frame that
 *     overruns the configured cap
 *
 * WHAT IS NOT EXERCISED (and must never be claimed)
 *   - No socket, no UDP, no lwIP: not one datagram leaves this image. The
 *     platform file has no network peripheral at all.
 *   - No sensor HAL, no DMA, no cache maintenance, no MPU/PMP setup.
 *   - No FreeRTOS: the task/queue calls come from a cooperative stub, see
 *     ../stub_freertos/README.md.
 *   - No timing accuracy, no throughput, no power, no interrupt latency.
 *   - Emulated Cortex-M is not silicon. A green run is evidence about this C
 *     code and about the emulator, not about any real MCU.
 */

#include "hml_mcu_demo.h"

#include "hml/hml_crc.h"
#include "hml/hml_ctrl.h"
#include "hml/hml_discovery.h"
#include "hml/hml_protocol.h"
#include "hml/hml_reasm.h"
#include "hml/hml_types.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Static scratch. A Cortex-M stack is a few kilobytes, so nothing here may be
 * an automatic: a 1458-byte datagram buffer on a 2 KiB stack is an instant
 * overflow. All of it lands in .bss.                                       */
/* ------------------------------------------------------------------------- */
static uint8_t g_dg[HML_MAX_FRAG_SIZE];
static uint8_t g_msg[HML_CTRL_MAX_PAYLOAD + 32u];
static uint8_t g_beacon[HML_DISC_SIZE];
static uint8_t g_cfg[HML_CFG_SIZE];
static uint8_t g_payload[256];
static uint8_t g_expect[512];

/* Reassembly sink state. */
static uint32_t g_sink_calls;        /* callbacks since the last sink_reset() */
static uint32_t g_sink_total;        /* callbacks since reset_all()          */
static uint32_t g_sink_stream;
static uint32_t g_sink_seq;
static size_t g_sink_bytes;
static size_t g_sink_mismatch;
static uint32_t g_sink_zero_byte_calls;
static size_t g_expect_len;

/* Run context. Single-threaded by construction (see the stub RTOS), so a file
 * static is the honest choice; a real port would pass it through task-local
 * storage instead. */
typedef struct demo_ctx {
  const hml_mcu_demo_io *io;
  hml_mcu_demo_result *res;
} demo_ctx;

static demo_ctx g_ctx;

/* ------------------------------------------------------------------------- */
/* Line formatting: no stdio anywhere, so this links on a bare-metal target.   */
/* Every emitted line is NUL-terminated by emit(); nothing is ever read past   */
/* the bytes that were actually written.                                       */
/* ------------------------------------------------------------------------- */
typedef struct line {
  char *d;
  size_t cap;
  size_t n;
} line;

static line ln(char *buf, size_t cap) {
  line l;
  l.d = buf;
  l.cap = cap;
  l.n = 0u;
  return l;
}

static line put_str(line l, const char *s) {
  while (*s != '\0' && l.n + 1u < l.cap) l.d[l.n++] = *s++;
  return l;
}

static line put_u32(line l, uint32_t v) {
  char tmp[10];
  size_t k = 0u;
  if (v == 0u) tmp[k++] = '0';
  while (v != 0u && k < sizeof(tmp)) {
    tmp[k++] = (char)('0' + (v % 10u));
    v /= 10u;
  }
  while (k > 0u && l.n + 1u < l.cap) l.d[l.n++] = tmp[--k];
  return l;
}

static line put_hex(line l, uint32_t v, unsigned digits) {
  static const char k_hex[] = "0123456789abcdef";
  for (unsigned i = digits; i > 0u; i--) {
    if (l.n + 1u >= l.cap) break;
    l.d[l.n++] = k_hex[(v >> ((i - 1u) * 4u)) & 0xfu];
  }
  return l;
}

/* Emit "<prefix> <n bytes of text>", always NUL-terminated. Nothing is ever
 * read past the bytes that were actually written. */
static void emit_text(const char *prefix, const char *text, size_t n) {
  char out[HML_MCU_DEMO_LINE_MAX];
  line o = ln(out, sizeof(out));
  o = put_str(o, prefix);
  o = put_str(o, " ");
  for (size_t i = 0; i < n && o.n + 1u < o.cap; i++) o.d[o.n++] = text[i];
  o.d[o.n] = '\0';
  if (g_ctx.io != NULL && g_ctx.io->puts != NULL) g_ctx.io->puts(g_ctx.io->ctx, out);
}

static void emit(const char *prefix, const char *text) {
  emit_text(prefix, text, strlen(text));
}

static void emit_line(const char *prefix, line l) { emit_text(prefix, l.d, l.n); }

/* One check. `ok` is an int so callers can pass a comparison directly. */
static void check(int ok, const char *tag) {
  if (g_ctx.res == NULL) return;
  g_ctx.res->checks++;
  if (ok) return;
  g_ctx.res->failures++;
  emit(HML_MCU_DEMO_MARK_FAIL, tag);
}

static void note(const char *body) { emit(HML_MCU_DEMO_MARK_NOTE, body); }

static void phase(const char *name) { emit(HML_MCU_DEMO_MARK_PHASE, name); }

/* ------------------------------------------------------------------------- */
/* Datagram builder -- uses only the public library API, exactly as a sender  */
/* would, so the demo never gets to disagree with src/stream about framing.   */
/* ------------------------------------------------------------------------- */
static size_t build_frame(uint8_t *dg, uint16_t stream, uint32_t seq, uint16_t frag_idx,
                          uint16_t frag_cnt, uint8_t flags, uint64_t ts_us,
                          const uint8_t *payload, uint16_t len) {
  hml_frame_hdr h;
  memset(&h, 0, sizeof(h));
  h.stream_id = stream;
  h.seq = seq;
  h.frag_idx = frag_idx;
  h.frag_cnt = frag_cnt;
  h.ts_us = ts_us;
  h.flags = flags;
  h.payload_len = len;
  hml_proto_frame_fill(&h, dg);
  if (len > 0u && payload != NULL) memcpy(dg + HML_OFF_PAYLOAD, payload, len);
  hml_put_be16(dg + HML_OFF_PAYLOAD + len, hml_crc16(payload, len));
  return (size_t)HML_FRAME_HDR_SIZE + (size_t)len + HML_TRAIL_CRC_SIZE;
}

static void on_frame(void *ctx, uint16_t stream_id, uint32_t frame_seq, const uint8_t *frame,
                     size_t frame_bytes, const hml_frame_hdr *final_hdr) {
  (void)ctx;
  g_sink_calls++;
  g_sink_total++;
  g_sink_stream = stream_id;
  g_sink_seq = frame_seq;
  g_sink_bytes = frame_bytes;
  if (frame_bytes == 0u) {
    g_sink_zero_byte_calls++;
    return;
  }
  if (final_hdr == NULL || frame_bytes != g_expect_len ||
      memcmp(frame, g_expect, frame_bytes) != 0)
    g_sink_mismatch++;
}

static void sink_reset(void) {
  g_sink_calls = 0u;
  g_sink_stream = 0u;
  g_sink_seq = 0u;
  g_sink_bytes = 0u;
  g_sink_mismatch = 0u;
  g_sink_zero_byte_calls = 0u;
}

/* Tell the sink which bytes a correct delivery must contain. */
static void sink_expect(const uint8_t *bytes, size_t len) {
  g_expect_len = len;
  if (len > 0u && bytes != NULL) memcpy(g_expect, bytes, len);
}

/* ------------------------------------------------------------------------- */
/* Phase 1 -- CRC primitives                                                   */
/* ------------------------------------------------------------------------- */
static void phase_crc(void) {
  static const uint8_t k_check_vector[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  phase("crc");
  check(hml_crc16(k_check_vector, sizeof(k_check_vector)) == 0x29B1u,
        "hml_crc16 of \"123456789\" must be 0x29b1");
  check(hml_crc32_ieee(k_check_vector, sizeof(k_check_vector)) == 0xCBF43926u,
        "hml_crc32_ieee of \"123456789\" must be 0xcbf43926");
  /* init 0xffff, no final xor: the empty input is the documented 0xffff */
  check(hml_crc16(k_check_vector, 0u) == 0xFFFFu, "hml_crc16 of an empty buffer must be 0xffff");
  check(hml_crc16(k_check_vector, 8u) != hml_crc16(k_check_vector, 9u),
        "hml_crc16 must change when one byte is added");
  check(hml_crc32_ieee(k_check_vector, 8u) != hml_crc32_ieee(k_check_vector, 9u),
        "hml_crc32_ieee must change when one byte is added");
}

/* ------------------------------------------------------------------------- */
/* Phase 2 -- discovery beacon codec                                           */
/* ------------------------------------------------------------------------- */
static void phase_beacon(uint32_t uuid, uint32_t caps, uint32_t fw_ver) {
  hml_beacon in;
  hml_beacon out;
  phase("beacon");

  memset(&in, 0, sizeof(in));
  in.version = (uint8_t)HML_DISC_VERSION;
  in.flags = 0u;
  in.ttl = 1u;
  in.uuid = uuid;
  in.caps = caps;
  in.fw_ver = fw_ver;
  in.ts_us = 0x0123456789abull; /* 48-bit field on the wire */
  hml_beacon_fill(&in, g_beacon);

  check(hml_beacon_parse(g_beacon, HML_DISC_SIZE, &out) == HML_OK, "a beacon must parse back");
  check(out.uuid == uuid, "beacon uuid must survive the round trip");
  check(out.caps == caps, "beacon caps must survive the round trip");
  check(out.fw_ver == fw_ver, "beacon fw_ver must survive the round trip");
  check(out.ts_us == in.ts_us, "beacon ts_us must survive the 48-bit encode");
  check(out.ttl == 1u, "beacon ttl must survive the round trip");

  /* Fail closed: the length is exact, the magic is checked, the CRC covers
   * bytes [0, 26). */
  check(hml_beacon_parse(g_beacon, HML_DISC_SIZE - 1u, &out) == HML_ERR_PROTO,
        "a short beacon must be rejected");
  check(hml_beacon_parse(g_beacon, HML_DISC_SIZE + 1u, &out) == HML_ERR_PROTO,
        "an over-long beacon must be rejected");
  {
    uint8_t bad[HML_DISC_SIZE];
    memcpy(bad, g_beacon, sizeof(bad));
    bad[0] ^= 0xffu;
    check(hml_beacon_parse(bad, sizeof(bad), &out) == HML_ERR_PROTO,
          "a beacon with a bad magic must be rejected");
  }
  {
    uint8_t bad[HML_DISC_SIZE];
    memcpy(bad, g_beacon, sizeof(bad));
    bad[HML_DISC_SIZE - 1u] ^= 0x01u; /* last CRC byte */
    check(hml_beacon_parse(bad, sizeof(bad), &out) == HML_ERR_CRC,
          "a beacon with a bad CRC must be rejected as a CRC error");
  }
  {
    uint8_t bad[HML_DISC_SIZE];
    memcpy(bad, g_beacon, sizeof(bad));
    bad[8] ^= 0x01u; /* first uuid byte: the CRC must catch it */
    check(hml_beacon_parse(bad, sizeof(bad), &out) == HML_ERR_CRC,
          "a flipped beacon field must be caught by the CRC");
  }
}

/* ------------------------------------------------------------------------- */
/* Phase 3 -- control codec and the hml_dest_cfg codec                        */
/* ------------------------------------------------------------------------- */
static void phase_control(void) {
  hml_ctrl_msg m;
  hml_ctrl_msg out;
  const uint8_t *payload = NULL;
  hml_dest_cfg cfg;
  hml_dest_cfg cfg_out;
  uint16_t n = 0u;
  phase("control");

  /* PING, no payload. */
  memset(&m, 0, sizeof(m));
  m.op = HML_OP_PING;
  m.flags = (uint16_t)(HML_CTRL_FLAG_REQ | HML_CTRL_FLAG_ACK);
  hml_ctrl_fill(&m, g_msg, (uint16_t)sizeof(g_msg), &n);
  check(n == 14u, "a PING request must serialise to 14 bytes");
  check(hml_ctrl_parse(g_msg, n, &out, &payload) == HML_OK, "PING must parse back");
  check(out.op == HML_OP_PING, "the PING opcode must survive the round trip");
  check(out.flags == m.flags, "the PING flags must survive the round trip");
  check(out.payload_len == 0u, "PING must carry no payload");

  /* SET_DEST carrying a full hml_dest_cfg, then the cfg codec. */
  memset(&cfg, 0, sizeof(cfg));
  cfg.kind = HML_XPORT_UDP;
  cfg.ipv4 = 0x0a000205u; /* 10.0.2.5 */
  cfg.udp_port = (uint16_t)HML_DATA_PORT;
  cfg.qp = 0x11223344u;
  cfg.psn = 0xaabbccddu;
  cfg.rkey = 0x55667788u;
  cfg.remote_addr = 0x00007f0000000000ull;
  cfg.frame_size = 4096u;
  cfg.window_len = 64u;
  cfg.hop_limit = 64u;
  hml_proto_cfg_fill(&cfg, g_cfg);

  memset(&m, 0, sizeof(m));
  m.op = HML_OP_SET_DEST;
  m.flags = (uint16_t)HML_CTRL_FLAG_REQ;
  m.payload = g_cfg;
  m.payload_len = (uint16_t)HML_CFG_SIZE;
  hml_ctrl_fill(&m, g_msg, (uint16_t)sizeof(g_msg), &n);
  check(n == (uint16_t)(HML_CFG_SIZE + 14u), "SET_DEST must serialise to cfg + 14 bytes");
  check(hml_ctrl_parse(g_msg, n, &out, &payload) == HML_OK, "SET_DEST must parse back");
  check(out.op == HML_OP_SET_DEST, "the SET_DEST opcode must survive the round trip");
  check(out.payload_len == (uint16_t)HML_CFG_SIZE, "the SET_DEST payload length must be HML_CFG_SIZE");
  check(hml_proto_cfg_parse(payload, out.payload_len, &cfg_out) == HML_OK, "the cfg must parse");
  check(cfg_out.ipv4 == cfg.ipv4 && cfg_out.udp_port == cfg.udp_port,
        "cfg addr/port must survive the round trip");
  check(cfg_out.qp == cfg.qp && cfg_out.rkey == cfg.rkey, "cfg qp/rkey must survive");
  check(cfg_out.remote_addr == cfg.remote_addr, "cfg remote_addr must survive the round trip");
  check(cfg_out.frame_size == cfg.frame_size && cfg_out.window_len == cfg.window_len,
        "cfg window must survive the round trip");
  check(cfg_out.hop_limit == cfg.hop_limit, "cfg hop_limit must survive the round trip");

  /* Fail-closed control parsing. */
  {
    uint8_t bad[sizeof(g_msg)];
    memcpy(bad, g_msg, n);
    bad[0] ^= 0xffu;
    check(hml_ctrl_parse(bad, n, &out, &payload) == HML_ERR_PROTO,
          "a control message with a bad magic must be rejected");
  }
  {
    uint8_t bad[sizeof(g_msg)];
    memcpy(bad, g_msg, n);
    bad[4] = 2u; /* version */
    check(hml_ctrl_parse(bad, n, &out, &payload) == HML_ERR_PROTO,
          "a control message with the wrong version must be rejected");
  }
  check(hml_ctrl_parse(g_msg, (size_t)n - 1u, &out, &payload) == HML_ERR_PROTO,
        "a truncated control message must be rejected");
  check(hml_ctrl_parse(g_msg, (size_t)n + 1u, &out, &payload) == HML_ERR_PROTO,
        "a control message with a trailing byte must be rejected");
  {
    uint8_t bad[sizeof(g_msg)];
    memcpy(bad, g_msg, n);
    bad[n - 1u] ^= 0x01u; /* last CRC byte */
    check(hml_ctrl_parse(bad, n, &out, &payload) == HML_ERR_CRC,
          "a control message with a bad CRC must be rejected as a CRC error");
  }
  {
    uint8_t bad[sizeof(g_msg)];
    memcpy(bad, g_msg, n);
    hml_put_be16(bad + 8, (uint16_t)(HML_CTRL_MAX_PAYLOAD + 1u)); /* over-long payload_len */
    check(hml_ctrl_parse(bad, n, &out, &payload) == HML_ERR_PROTO,
          "an over-long payload_len must be rejected");
  }

  /* Fail-closed cfg parsing: the reserved bytes must be zero on the wire. */
  check(hml_proto_cfg_parse(g_cfg, HML_CFG_SIZE - 1u, &cfg_out) == HML_ERR_PROTO,
        "a short hml_dest_cfg must be rejected");
  {
    uint8_t bad[HML_CFG_SIZE];
    memcpy(bad, g_cfg, sizeof(bad));
    bad[0] = 7u; /* kind: neither UDP nor RoCEv2 */
    check(hml_proto_cfg_parse(bad, sizeof(bad), &cfg_out) == HML_ERR_PROTO,
          "an unknown cfg kind must be rejected");
  }
  {
    uint8_t bad[HML_CFG_SIZE];
    memcpy(bad, g_cfg, sizeof(bad));
    bad[1] = 1u; /* pad0 */
    check(hml_proto_cfg_parse(bad, sizeof(bad), &cfg_out) == HML_ERR_PROTO,
          "a non-zero cfg pad0 must be rejected");
  }
  {
    uint8_t bad[HML_CFG_SIZE];
    memcpy(bad, g_cfg, sizeof(bad));
    bad[35] = 1u; /* pad1 */
    check(hml_proto_cfg_parse(bad, sizeof(bad), &cfg_out) == HML_ERR_PROTO,
          "a non-zero cfg pad1 must be rejected");
  }
  {
    uint8_t bad[HML_CFG_SIZE];
    memcpy(bad, g_cfg, sizeof(bad));
    bad[36] = 1u; /* rsvd[1] */
    check(hml_proto_cfg_parse(bad, sizeof(bad), &cfg_out) == HML_ERR_PROTO,
          "a non-zero cfg reserved byte must be rejected");
  }
}

/* ------------------------------------------------------------------------- */
/* Phase 4 -- frame codec                                                      */
/* ------------------------------------------------------------------------- */
static void phase_frame_codec(void) {
  hml_frame_hdr out;
  const uint8_t *payload = NULL;
  size_t dg_len;
  phase("frame-codec");

  for (unsigned i = 0; i < sizeof(g_payload); i++) g_payload[i] = (uint8_t)(i * 7u + 3u);
  dg_len = build_frame(g_dg, 7u, 1000u, 0u, 0u, (uint8_t)(HML_FLAG_START | HML_FLAG_END),
                       1234567u, g_payload, (uint16_t)sizeof(g_payload));

  check(hml_proto_frame_parse(g_dg, dg_len, &out, &payload) == HML_OK, "a frame must parse back");
  check(out.stream_id == 7u && out.seq == 1000u, "the frame stream/seq must survive the round trip");
  check(out.ts_us == 1234567u, "the frame ts_us must survive the round trip");
  check(out.payload_len == (uint16_t)sizeof(g_payload), "the frame payload_len must survive");
  check(payload != NULL && memcmp(payload, g_payload, sizeof(g_payload)) == 0,
        "the frame payload bytes must survive the round trip");

  {
    uint8_t bad[HML_MAX_FRAG_SIZE];
    memcpy(bad, g_dg, dg_len);
    bad[HML_OFF_MAGIC] ^= 0xffu;
    check(hml_proto_frame_parse(bad, dg_len, &out, &payload) == HML_ERR_PROTO,
          "a frame with a bad magic must be rejected");
  }
  {
    uint8_t bad[HML_MAX_FRAG_SIZE];
    memcpy(bad, g_dg, dg_len);
    bad[HML_OFF_VERSION] = 2u;
    check(hml_proto_frame_parse(bad, dg_len, &out, &payload) == HML_ERR_PROTO,
          "a frame with the wrong version must be rejected");
  }
  {
    uint8_t bad[HML_MAX_FRAG_SIZE];
    memcpy(bad, g_dg, dg_len);
    bad[HML_OFF_HDR_CRC] ^= 0x01u;
    check(hml_proto_frame_parse(bad, dg_len, &out, &payload) == HML_ERR_CRC,
          "a frame with a bad header CRC must be rejected as a CRC error");
  }
  {
    uint8_t bad[HML_MAX_FRAG_SIZE];
    memcpy(bad, g_dg, dg_len);
    bad[HML_OFF_PAYLOAD + 3u] ^= 0x01u; /* payload byte, header CRC still valid */
    check(hml_proto_frame_parse(bad, dg_len, &out, &payload) == HML_ERR_CRC,
          "a flipped payload byte must be caught by the payload CRC");
  }
  check(hml_proto_frame_parse(g_dg, (size_t)HML_FRAME_HDR_SIZE, &out, &payload) == HML_ERR_PROTO,
        "a header-only frame must be rejected when a payload was announced");
}

/* ------------------------------------------------------------------------- */
/* Phase 5 -- reassembly                                                       */
/* ------------------------------------------------------------------------- */
static void phase_reasm(hml_reasm_ctx_t *r) {
  hml_reasm_stats st;
  size_t dg_len;
  uint32_t frames_ok_before;
  uint32_t frags_recv_before;
  phase("reasm");

  /* Every fragment below is built from a DIFFERENT slice of g_payload, so the
   * sink can tell a correctly gathered frame from one that was merely appended
   * in arrival order. This is the whole point of the reassembly check. */
  for (size_t i = 0u; i < sizeof(g_payload); i++) g_payload[i] = (uint8_t)((i * 7u) + 3u);

  /* Frame 1: three fragments of 64 bytes each, so 192 bytes assembled. The v1
   * layout rule requires every fragment to have the same length except the
   * last, which may be shorter: a longer final fragment is a layout error the
   * reassembler drops on purpose (frame 3 below). Three fragments with last > 0
   * is what forces the index-order gather, i.e. the path a multi-MTU frame
   * actually takes. */
  sink_expect(g_payload, 192u);
  sink_reset();

  dg_len = build_frame(g_dg, 7u, 1000u, 0u, 3u, HML_FLAG_START, 1000u, g_payload, 64u);
  check(hml_reasm_feed_datagram(r, g_dg, dg_len) == HML_OK, "reasm fragment 0 must be accepted");
  dg_len = build_frame(g_dg, 7u, 1001u, 1u, 3u, 0u, 1001u, g_payload + 64u, 64u);
  check(hml_reasm_feed_datagram(r, g_dg, dg_len) == HML_OK, "reasm fragment 1 must be accepted");
  dg_len = build_frame(g_dg, 7u, 1002u, 2u, 3u, HML_FLAG_END, 1002u, g_payload + 128u, 64u);
  check(hml_reasm_feed_datagram(r, g_dg, dg_len) == HML_OK, "reasm fragment 2 must be accepted");
  check(g_sink_calls == 1u, "a 3-fragment frame must produce exactly one EOF callback");
  check(g_sink_bytes == 192u, "the reassembled frame must be 192 bytes");
  check(g_sink_mismatch == 0u, "the reassembled frame must be byte-exact in index order");
  check(g_sink_stream == 7u && g_sink_seq == 1000u, "the EOF must report stream 7 seq 1000");
  hml_reasm_get_stats(r, &st);
  check(st.frames_ok == 1u, "frames_ok must be 1 after one clean frame");
  check(st.frags_recv == 3u, "frags_recv must be 3 after three fragments");
  check(st.crc_fail == 0u && st.proto_fail == 0u, "a clean frame must not raise any error counter");

  /* The same frame with the fragments arriving REVERSED. A stream that numbers
   * its fragments may reorder them, and the reassembled bytes must not care. */
  sink_expect(g_payload, 192u);
  sink_reset();
  /* Each datagram's seq is base + frag_idx, which is the v1 rule the
   * reassembler enforces (the last fragment of a frame carries base + cnt - 1). */
  dg_len = build_frame(g_dg, 11u, 1202u, 2u, 3u, HML_FLAG_END, 1202u, g_payload + 128u, 64u);
  check(hml_reasm_feed_datagram(r, g_dg, dg_len) == HML_OK, "a tail-first fragment must be accepted");
  dg_len = build_frame(g_dg, 11u, 1201u, 1u, 3u, 0u, 1201u, g_payload + 64u, 64u);
  check(hml_reasm_feed_datagram(r, g_dg, dg_len) == HML_OK, "a mid fragment must be accepted");
  dg_len = build_frame(g_dg, 11u, 1200u, 0u, 3u, HML_FLAG_START, 1200u, g_payload, 64u);
  check(hml_reasm_feed_datagram(r, g_dg, dg_len) == HML_OK, "the straggler head must be accepted");
  check(g_sink_calls == 1u && g_sink_bytes == 192u && g_sink_mismatch == 0u,
        "a reversed-order frame must assemble to the same 192 bytes");
  check(g_sink_stream == 11u && g_sink_seq == 1200u, "the EOF must report the base sequence, not the last one");

  /* Frame 2: a shorter final fragment is legal and must be honoured exactly:
   * 64 + 64 + 32 = 160 bytes. */
  sink_expect(g_payload, 160u);
  sink_reset();
  dg_len = build_frame(g_dg, 6u, 1100u, 0u, 3u, HML_FLAG_START, 1100u, g_payload, 64u);
  (void)hml_reasm_feed_datagram(r, g_dg, dg_len);
  dg_len = build_frame(g_dg, 6u, 1101u, 1u, 3u, 0u, 1101u, g_payload + 64u, 64u);
  (void)hml_reasm_feed_datagram(r, g_dg, dg_len);
  dg_len = build_frame(g_dg, 6u, 1102u, 2u, 3u, HML_FLAG_END, 1102u, g_payload + 128u, 32u);
  (void)hml_reasm_feed_datagram(r, g_dg, dg_len);
  check(g_sink_calls == 1u && g_sink_bytes == 160u,
        "a frame with a shorter final fragment must assemble to 160 bytes");
  check(g_sink_mismatch == 0u, "the short-tail frame must be byte-exact");

  /* Frame 3: a LONGER final fragment is not uniformly fragmentable, so the v1
   * reassembler drops it and counts the drop. Fail closed, never delivered. */
  sink_expect(g_payload, 200u);
  sink_reset();
  dg_len = build_frame(g_dg, 5u, 1300u, 0u, 3u, HML_FLAG_START, 1300u, g_payload, 64u);
  (void)hml_reasm_feed_datagram(r, g_dg, dg_len);
  dg_len = build_frame(g_dg, 5u, 1301u, 1u, 3u, 0u, 1301u, g_payload + 64u, 64u);
  (void)hml_reasm_feed_datagram(r, g_dg, dg_len);
  dg_len = build_frame(g_dg, 5u, 1302u, 2u, 3u, HML_FLAG_END, 1302u, g_payload + 128u, 72u);
  (void)hml_reasm_feed_datagram(r, g_dg, dg_len);
  hml_reasm_get_stats(r, &st);
  check(st.frames_dropped == 1u, "a longer final fragment must be dropped and counted");
  check(g_sink_calls == 0u, "a non-uniformly fragmentable frame must not be delivered");

  /* Frame 4: a corrupt final fragment. Counted, never delivered, and the
   * reassembler keeps working for the next frame. */
  sink_reset();
  dg_len = build_frame(g_dg, 8u, 2000u, 0u, 2u, HML_FLAG_START, 2000u, g_payload, 64u);
  (void)hml_reasm_feed_datagram(r, g_dg, dg_len);
  dg_len = build_frame(g_dg, 8u, 2001u, 1u, 2u, HML_FLAG_END, 2001u, g_payload + 64u, 64u);
  g_dg[HML_OFF_PAYLOAD + 5u] ^= 0x01u; /* flip a payload bit: the payload CRC must catch it */
  (void)hml_reasm_feed_datagram(r, g_dg, dg_len);
  hml_reasm_get_stats(r, &st);
  check(st.crc_fail == 1u, "a corrupted fragment must be counted in crc_fail");
  check(g_sink_calls == 0u, "the corrupt frame must not reach the EOF callback");

  /* Frame 5: the next clean frame still arrives -- one bad datagram is not
   * fatal, which is the property a lossy link needs. */
  sink_expect(g_payload, 32u);
  sink_reset();
  dg_len = build_frame(g_dg, 9u, 3000u, 0u, 0u, (uint8_t)(HML_FLAG_START | HML_FLAG_END), 3000u,
                       g_payload, 32u);
  check(hml_reasm_feed_datagram(r, g_dg, dg_len) == HML_OK, "the recovery frame must be accepted");
  check(g_sink_calls == 1u && g_sink_bytes == 32u, "the recovery frame must be delivered once");
  check(g_sink_mismatch == 0u, "the recovery frame must be byte-exact");
  hml_reasm_get_stats(r, &st);
  check(st.frames_ok == 4u, "frames_ok must count the 4 delivered frames so far");

  /* A duplicate fragment INSIDE an in-flight multi-fragment frame is
   * idempotent: it must not be staged twice, must not change the delivered
   * bytes, and must not inflate frames_ok. It IS counted in frags_recv, which
   * counts datagrams received, not frames delivered. */
  sink_expect(g_payload, 128u);
  sink_reset();
  hml_reasm_get_stats(r, &st);
  frags_recv_before = st.frags_recv;
  frames_ok_before = st.frames_ok;
  dg_len = build_frame(g_dg, 10u, 4000u, 0u, 2u, HML_FLAG_START, 4000u, g_payload, 64u);
  (void)hml_reasm_feed_datagram(r, g_dg, dg_len);
  (void)hml_reasm_feed_datagram(r, g_dg, dg_len); /* the duplicate */
  hml_reasm_get_stats(r, &st);
  check(st.frags_recv == frags_recv_before + 2u, "frags_recv counts datagrams, duplicates included");
  check(st.frames_ok == frames_ok_before, "a duplicate fragment must not deliver a frame");
  check(g_sink_calls == 0u, "an incomplete frame must not reach the EOF callback");
  dg_len = build_frame(g_dg, 10u, 4001u, 1u, 2u, HML_FLAG_END, 4001u, g_payload + 64u, 64u);
  (void)hml_reasm_feed_datagram(r, g_dg, dg_len);
  hml_reasm_get_stats(r, &st);
  check(g_sink_calls == 1u && g_sink_bytes == 128u,
        "a duplicate fragment must not change the assembled frame");
  check(g_sink_mismatch == 0u, "a duplicate fragment must not corrupt the payload");
  check(st.frames_ok == frames_ok_before + 1u, "the completed frame must count exactly one frame");

  /* A repeated COMPLETE single-fragment frame is a new frame, not a duplicate
   * to be swallowed: the datagram carries a fresh sequence number, so the
   * reassembler is right to deliver it again. */
  sink_reset();
  dg_len = build_frame(g_dg, 9u, 3002u, 0u, 0u, (uint8_t)(HML_FLAG_START | HML_FLAG_END), 3002u,
                       g_payload, 32u);
  (void)hml_reasm_feed_datagram(r, g_dg, dg_len);
  hml_reasm_get_stats(r, &st);
  check(g_sink_calls == 1u && g_sink_bytes == 32u,
        "a repeated complete single-fragment frame must be delivered again");
  check(st.frames_ok == 6u, "frames_ok must count every delivered frame, repeats included");

  /* EOF-only flush: a zero-payload END wakes the consumer with 0 bytes and does
   * NOT count as a delivered frame. */
  sink_reset();
  dg_len = build_frame(g_dg, 9u, 3003u, 0u, 0u, HML_FLAG_END, 3003u, NULL, 0u);
  hml_reasm_get_stats(r, &st);
  frames_ok_before = st.frames_ok;
  check(hml_reasm_feed_datagram(r, g_dg, dg_len) == HML_OK, "an EOF-only flush must be accepted");
  check(g_sink_calls == 1u && g_sink_zero_byte_calls == 1u,
        "an EOF-only flush must fire exactly one zero-byte callback");
  hml_reasm_get_stats(r, &st);
  check(st.frames_ok == frames_ok_before, "an EOF-only flush must not increment frames_ok");

  /* A datagram that is not a legal frame is rejected by the protocol parser,
   * before the reassembler's own logic sees it, and never delivered. */
  sink_reset();
  dg_len = build_frame(g_dg, 9u, 3004u, 0u, 0u, (uint8_t)(HML_FLAG_START | HML_FLAG_END), 3004u,
                       g_payload, 32u);
  g_dg[HML_OFF_VERSION] = 0x7fu; /* an unsupported protocol version */
  check(hml_reasm_feed_datagram(r, g_dg, dg_len) != HML_OK,
        "an unsupported protocol version must be rejected");
  dg_len = build_frame(g_dg, 9u, 3005u, 0u, 0u, (uint8_t)(HML_FLAG_START | HML_FLAG_END), 3005u,
                       g_payload, 32u);
  g_dg[HML_OFF_MAGIC] ^= 0xffu; /* not a datagram at all */
  check(hml_reasm_feed_datagram(r, g_dg, dg_len) != HML_OK, "a foreign magic must be rejected");
  hml_reasm_get_stats(r, &st);
  check(st.frags_recv == frags_recv_before + 3u + 2u,
        "a rejected datagram must not be counted as a received fragment");
  check(g_sink_calls == 0u, "a rejected datagram must not reach the EOF callback");
}

static void phase_reasm_cap(void) {
  hml_reasm_ctx_t *small = NULL;
  hml_reasm_stats st;
  size_t dg_len;
  phase("reasm-cap");

  check(hml_reasm_open(&small, 64u) == HML_OK, "a 64-byte reassembly cap must open");
  if (small == NULL) return;
  hml_reasm_set_eof_cb(small, on_frame, NULL);
  memset(g_payload, 0x77, sizeof(g_payload));
  sink_expect(g_payload, 128u); /* never delivered; the sink must see nothing */
  sink_reset();

  dg_len = build_frame(g_dg, 11u, 4000u, 0u, 2u, HML_FLAG_START, 4000u, g_payload, 64u);
  (void)hml_reasm_feed_datagram(small, g_dg, dg_len);
  dg_len = build_frame(g_dg, 11u, 4001u, 1u, 2u, HML_FLAG_END, 4001u, g_payload, 64u);
  (void)hml_reasm_feed_datagram(small, g_dg, dg_len);

  hml_reasm_get_stats(small, &st);
  check(st.frames_dropped == 1u, "a frame larger than the cap must be dropped and counted");
  check(st.frames_ok == 0u, "an over-cap frame must never count as ok");
  check(g_sink_calls == 0u, "an over-cap frame must not reach the EOF callback");
  hml_reasm_close(small);
}

/* ------------------------------------------------------------------------- */
/* Runner                                                                     */
/* ------------------------------------------------------------------------- */
int hml_mcu_demo_run(const hml_mcu_demo_cfg *cfg, const hml_mcu_demo_io *io,
                     hml_mcu_demo_result *res) {
  hml_mcu_demo_cfg use;
  hml_reasm_ctx_t *r = NULL;
  char buf[HML_MCU_DEMO_LINE_MAX];
  line l;
  uint32_t cap;
  hml_status st;

  if (cfg == NULL || io == NULL || io->puts == NULL || res == NULL) return -1;

  use = *cfg;
  if (use.device_uuid == 0u) use.device_uuid = 0x484d4c4du; /* 'HMLM' */
  /* HML_CAP_UDP is deliberately NOT claimed: there is no network stack in this
   * image, so advertising a UDP capability would be a lie a host could act on. */
  if (use.device_caps == 0u) use.device_caps = HML_CAP_MCU;
  cap = (use.frame_bytes == 0u) ? (uint32_t)HML_MAX_FRAME_SIZE : use.frame_bytes;

  memset(res, 0, sizeof(*res));
  memset(&g_expect, 0, sizeof(g_expect));
  g_expect_len = 0u;
  g_sink_total = 0u;
  g_ctx.io = io;
  g_ctx.res = res;

  l = ln(buf, sizeof(buf));
  l = put_str(l, "banner holoscan_mcu_lite version=");
  l = put_str(l, HML_VERSION_STRING);
  l = put_str(l, " image=hml_mcu_demo.elf rtos=cooperative-stub");
  emit_line("HML-MCU-DEMO", l);

  note("emulated Cortex-M smoke test: not a FreeRTOS port, not silicon");
  note("no lwip, no udp, no socket, no sensor, no dma: no datagram leaves this image");
  note("the rtos in this image is a single-task cooperative stub, not FreeRTOS");

  /* The memory contract, stated by the image itself. */
  l = ln(buf, sizeof(buf));
  l = put_str(l, "mem reasm-frame-bytes=");
  l = put_u32(l, cap);
  l = put_str(l, " reasm-buffers-bytes=");
  l = put_u32(l, cap * 2u);
  l = put_str(l, " (the two hml_reasm_open buffers; the heap must also hold "
                 "sizeof(hml_reasm_ctx)=9488, see board_config.h)");
  emit_line(HML_MCU_DEMO_MARK_NOTE, l);

  l = ln(buf, sizeof(buf));
  l = put_str(l, "device uuid=");
  l = put_hex(l, use.device_uuid, 8u);
  l = put_str(l, " caps=");
  l = put_hex(l, use.device_caps, 8u);
  l = put_str(l, " (HML_CAP_MCU only: no UDP capability is claimed)");
  emit_line(HML_MCU_DEMO_MARK_NOTE, l);

  /* READY is printed once the banner and the non-claims are out, so a consumer
   * can tell "booted and honest about its limits" from "started and died". */
  emit(HML_MCU_DEMO_MARK_READY, "banner complete, running the checks");

  if (use.pause != NULL) use.pause(use.pause_ctx, 20u);

  phase_crc();
  phase_beacon(use.device_uuid, use.device_caps, 0x00010200u);
  phase_control();
  phase_frame_codec();

  st = hml_reasm_open(&r, (size_t)use.frame_bytes);
  if (st != HML_OK) {
    /* The documented failure mode of the default cap on a part with little
     * SRAM: say it out loud instead of quietly degrading. */
    l = ln(buf, sizeof(buf));
    l = put_str(l, "hml_reasm_open failed status=");
    l = put_u32(l, (uint32_t)st);
    l = put_str(l, ": the reassembly buffers do not fit in this heap");
    l = put_str(l, " (see docs/renode.md section 3)");
    emit_line(HML_MCU_DEMO_MARK_WARN, l);
    check(0, "hml_reasm_open must succeed for the configured frame cap");
  } else {
    hml_reasm_set_eof_cb(r, on_frame, NULL);
    if (use.pause != NULL) use.pause(use.pause_ctx, 20u);
    phase_reasm(r);
    hml_reasm_close(r);
    r = NULL;
  }
  phase_reasm_cap();

  if (use.pause != NULL) use.pause(use.pause_ctx, 20u);

  res->frames_delivered = g_sink_total;
  l = ln(buf, sizeof(buf));
  l = put_str(l, "result=");
  l = put_str(l, (res->failures == 0u) ? "PASS" : "FAIL");
  l = put_str(l, " failures=");
  l = put_u32(l, res->failures);
  l = put_str(l, " checks=");
  l = put_u32(l, res->checks);
  l = put_str(l, " frames_delivered=");
  l = put_u32(l, g_sink_total);
  emit_line(HML_MCU_DEMO_MARK_DONE, l);
  return (res->failures == 0u) ? 0 : 1;
}
