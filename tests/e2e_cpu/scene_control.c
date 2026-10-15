/*
 * scene_control.c -- the control-beacon end-to-end scene.
 *
 * WHAT IS EXERCISED
 *   Phase 1  the discovery beacon golden wire vector: hml_beacon_fill with a
 *            fixed field set must produce the exact documented 28 bytes, and
 *            hml_beacon_parse must accept it and reject every documented
 *            corruption (short, over-long, bad magic, bad CRC, flipped field).
 *   Phase 2  the control codec and the hml_dest_cfg codec: round trips plus
 *            the full reject list (magic, version, length, trailing byte,
 *            over-long payload_len, CRC; cfg short, unknown kind, pad0, pad1,
 *            reserved byte).
 *   Phase 3  a real device-side hml_control server thread over loopback, a
 *            real hml_client, and a real socket feeding the client's discovery
 *            socket: discover (the server's own beacon fields), ping echo,
 *            set/get dest, caps, start/stop/reset lifecycle, unknown-op answer,
 *            and fail-closed malformed-datagram accounting.
 *
 * BOUNDARIES (kept identical to the other scenes)
 *   127.0.0.1 only, ports from the e2e private range, no privileges, governed
 *   by g_e2e_deadline. This is a host-CPU run of the shipping control code; it
 *   is not evidence about an MCU, FreeRTOS, lwIP or silicon.
 */

#include "e2e_cpu.h"

#include "hml/hml_ctrl.h"
#include "hml/hml_discovery.h"
#include "hml/hml_protocol.h"
#include "hml/hml_sock.h"
#include "hml_client.h"
#include "hml_control.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The one capability set that makes sense for a host that speaks UDP and
 * RoCEv2: HML_CAP_UDP | HML_CAP_ROCEV2. The MCU profile scene uses HML_CAP_MCU
 * instead; each place states its own profile. */
#define BEACON_UUID 0x0000002au /* 42 */
#define BEACON_CAPS (HML_CAP_UDP | HML_CAP_ROCEV2)
#define BEACON_FW   0x00010000u
#define BEACON_TS   0x000001000002ull

/*
 * The deterministic beacon that hml_beacon_fill must emit for the field set
 * above (version 1, flags 0, ttl 1, rsvd 0). Layout is
 * [magic u32 BE][ver u8][flags u8][ttl u8][rsvd u8][uuid u32 BE][caps u32 BE]
 * [fw_ver u32 BE][ts_us u64 BE][crc16 BE], 28 bytes, CRC over bytes [0, 26).
 * Computed with the library's own hml_crc16 (poly 0x1021, init 0xffff, no xor).
 */
static const uint8_t k_beacon_golden[HML_DISC_SIZE] = {
    0x48, 0x4d, 0x4c, 0x42, /* 'HMLB' */
    0x01,                   /* version */
    0x00,                   /* flags */
    0x01,                   /* ttl */
    0x00,                   /* rsvd */
    0x00, 0x00, 0x00, 0x2a, /* uuid 42 */
    0x00, 0x00, 0x00, 0x03, /* caps UDP|ROCEV2 */
    0x00, 0x01, 0x00, 0x00, /* fw_ver 0x00010000 */
    0x00, 0x00, 0x01, 0x00, 0x00, 0x02, /* ts_us 0x000001000002 (48 bits) */
    0xab, 0x47,                         /* crc16 */
};

/* ---- shared server-thread state ------------------------------------------ */

typedef struct srvstate {
  hml_control_t *c;
  atomic_int done;
  atomic_uint set_dest;
  atomic_uint start;
  atomic_uint stop;
} srvstate;

static void on_set_dest(void *ctx, const hml_dest_cfg *cfg) {
  (void)cfg;
  srvstate *s = (srvstate *)ctx;
  (void)atomic_fetch_add(&s->set_dest, 1u);
}

static void on_start(void *ctx) {
  srvstate *s = (srvstate *)ctx;
  (void)atomic_fetch_add(&s->start, 1u);
}

static void on_stop(void *ctx) {
  srvstate *s = (srvstate *)ctx;
  (void)atomic_fetch_add(&s->stop, 1u);
}

static void *srv_thread(void *arg) {
  srvstate *s = (srvstate *)arg;
  while (!atomic_load(&s->done)) {
    hml_status st = hml_control_poll(s->c, 40);
    if (st != HML_OK && st != HML_ERR_TIMEOUT && st != HML_ERR_AGAIN) break;
    if (e2e_budget_ms() == -1) break;
  }
  return NULL;
}

/* ---- helpers ------------------------------------------------------------- */

/* Loopback send of a prebuilt datagram; used for the beacon and for the two
 * malformed control datagrams the server must account for. */
static int raw_send(uint16_t to_port, const uint8_t *buf, size_t n) {
  hml_sock_t *s = NULL;
  if (hml_sock_udp_create(&s, HML_IP_LOOPBACK, 0, false) != HML_OK) return -1;
  hml_status st = hml_sock_set_dest(s, HML_IP_LOOPBACK, to_port);
  if (st == HML_OK) st = hml_sock_send(s, buf, n);
  hml_sock_close(s);
  return st == HML_OK ? 0 : -1;
}

/* Wait (bounded by the run-wide deadline) until the given 32-bit source
 * counter reaches the target. Used for things the server thread counts. */
static bool wait_u32(atomic_uint *v, unsigned target, int ms) {
  uint64_t deadline = e2e_now_us() + (uint64_t)(ms < 0 ? 0 : (unsigned)ms) * 1000ull;
  while (atomic_load(v) < target) {
    if (e2e_budget_ms() == -1) return false;
    if (e2e_now_us() >= deadline) return false;
    e2e_sleep_us(500);
  }
  return true;
}

static void wait_server_rx(const srvstate *s, uint32_t target, int ms) {
  uint64_t deadline = e2e_now_us() + (uint64_t)(ms < 0 ? 0 : (unsigned)ms) * 1000ull;
  while (hml_control_rx_count(s->c) < target) {
    if (e2e_budget_ms() == -1) return;
    if (e2e_now_us() >= deadline) return;
    e2e_sleep_us(500);
  }
}

/* ---- phase 1: beacon golden vector ---------------------------------------- */

static void phase_beacon_golden(e2e_harness *h) {
  hml_beacon b;
  hml_beacon out;
  uint8_t wire[HML_DISC_SIZE];

  memset(&b, 0, sizeof(b));
  b.version = (uint8_t)HML_DISC_VERSION;
  b.flags = 0u;
  b.ttl = 1u;
  b.uuid = BEACON_UUID;
  b.caps = BEACON_CAPS;
  b.fw_ver = BEACON_FW;
  b.ts_us = BEACON_TS;
  hml_beacon_fill(&b, wire);

  E2E_OK(h, "beacon.golden_vector",
         memcmp(wire, k_beacon_golden, HML_DISC_SIZE) == 0);
  E2E_OK(h, "beacon.parse_roundtrip", hml_beacon_parse(wire, HML_DISC_SIZE, &out) == HML_OK);
  E2E_EQ_U(h, "beacon.uuid", out.uuid, BEACON_UUID);
  E2E_EQ_U(h, "beacon.caps", out.caps, BEACON_CAPS);
  E2E_EQ_U(h, "beacon.fw_ver", out.fw_ver, BEACON_FW);
  E2E_EQ_U(h, "beacon.ts_us", out.ts_us, BEACON_TS);
  E2E_EQ_U(h, "beacon.ttl", out.ttl, 1u);

  uint8_t bad[HML_DISC_SIZE];
  E2E_OK(h, "beacon.reject.short",
         hml_beacon_parse(k_beacon_golden, HML_DISC_SIZE - 1u, &out) == HML_ERR_PROTO);
  E2E_OK(h, "beacon.reject.overlong",
         hml_beacon_parse(k_beacon_golden, HML_DISC_SIZE + 1u, &out) == HML_ERR_PROTO);

  memcpy(bad, k_beacon_golden, HML_DISC_SIZE);
  bad[0] ^= 0xffu;
  E2E_OK(h, "beacon.reject.bad_magic",
         hml_beacon_parse(bad, HML_DISC_SIZE, &out) == HML_ERR_PROTO);

  memcpy(bad, k_beacon_golden, HML_DISC_SIZE);
  bad[8] ^= 0x01u; /* first uuid byte: the CRC must catch it */
  E2E_OK(h, "beacon.reject.flipped_field",
         hml_beacon_parse(bad, HML_DISC_SIZE, &out) == HML_ERR_CRC);

  memcpy(bad, k_beacon_golden, HML_DISC_SIZE);
  bad[HML_DISC_SIZE - 1u] ^= 0x01u; /* last CRC byte */
  E2E_OK(h, "beacon.reject.bad_crc",
         hml_beacon_parse(bad, HML_DISC_SIZE, &out) == HML_ERR_CRC);
}

/* ---- phase 2: control + cfg codecs ---------------------------------------- */

static void phase_control_codec(e2e_harness *h) {
  hml_ctrl_msg m;
  hml_ctrl_msg out;
  const uint8_t *payload = NULL;
  hml_dest_cfg cfg;
  hml_dest_cfg cfg_out;
  uint16_t n = 0;
  uint8_t msg[sizeof(hml_ctrl_msg) + HML_CTRL_MAX_PAYLOAD + 16u];
  uint8_t buf[HML_CFG_SIZE];

  memset(&m, 0, sizeof(m));
  m.op = HML_OP_PING;
  m.flags = (uint16_t)(HML_CTRL_FLAG_REQ | HML_CTRL_FLAG_ACK);
  hml_ctrl_fill(&m, msg, (uint16_t)(HML_CTRL_HDR_SIZE + 2u), &n);
  /* fixed 12-byte header + zero payload + 2-byte trailing crc = 14 bytes */
  E2E_EQ_U(h, "ctrl.ping_wire_len", n, HML_CTRL_HDR_SIZE + 2u);
  E2E_OK(h, "ctrl.ping_roundtrip",
         hml_ctrl_parse(msg, n, &out, &payload) == HML_OK);
  E2E_EQ_U(h, "ctrl.ping_op", out.op, HML_OP_PING);
  E2E_EQ_U(h, "ctrl.ping_flags", out.flags, (uint16_t)(HML_CTRL_FLAG_REQ | HML_CTRL_FLAG_ACK));
  E2E_EQ_U(h, "ctrl.ping_payload_len", out.payload_len, 0u);

  memset(&cfg, 0, sizeof(cfg));
  cfg.kind = HML_XPORT_UDP;
  cfg.ipv4 = 0x7f000001u;
  cfg.udp_port = 45701u;
  cfg.qp = 0x11223344u;
  cfg.psn = 0xaabbccddu;
  cfg.rkey = 0x55667788u;
  cfg.remote_addr = 0x00007f0000000000ull;
  cfg.frame_size = 4096u;
  cfg.window_len = 64u;
  cfg.hop_limit = 64u;
  hml_proto_cfg_fill(&cfg, buf);

  memset(&m, 0, sizeof(m));
  m.op = HML_OP_SET_DEST;
  m.flags = (uint16_t)HML_CTRL_FLAG_REQ;
  m.payload = buf;
  m.payload_len = (uint16_t)HML_CFG_SIZE;
  hml_ctrl_fill(&m, msg, (uint16_t)sizeof(msg), &n);
  E2E_EQ_U(h, "ctrl.set_dest_wire_len", n, (uint16_t)(HML_CFG_SIZE + (HML_CTRL_HDR_SIZE + 2u)));
  E2E_OK(h, "ctrl.set_dest_roundtrip",
         hml_ctrl_parse(msg, n, &out, &payload) == HML_OK);
  E2E_EQ_U(h, "ctrl.set_dest_op", out.op, HML_OP_SET_DEST);
  E2E_EQ_U(h, "ctrl.set_dest_payload_len", out.payload_len, (uint16_t)HML_CFG_SIZE);
  E2E_OK(h, "ctrl.cfg_roundtrip",
         hml_proto_cfg_parse(payload, out.payload_len, &cfg_out) == HML_OK);
  E2E_OK(h, "ctrl.cfg_fields",
         cfg_out.ipv4 == cfg.ipv4 && cfg_out.udp_port == cfg.udp_port &&
             cfg_out.qp == cfg.qp && cfg_out.rkey == cfg.rkey &&
             cfg_out.remote_addr == cfg.remote_addr && cfg_out.frame_size == cfg.frame_size &&
             cfg_out.window_len == cfg.window_len && cfg_out.hop_limit == cfg.hop_limit);

  {
    uint8_t bad[sizeof(msg)];
    memcpy(bad, msg, n);
    bad[0] ^= 0xffu;
    E2E_OK(h, "ctrl.reject.bad_magic",
           hml_ctrl_parse(bad, n, &out, &payload) == HML_ERR_PROTO);
  }
  {
    uint8_t bad[sizeof(msg)];
    memcpy(bad, msg, n);
    bad[4] = 2u; /* version */
    E2E_OK(h, "ctrl.reject.bad_version",
           hml_ctrl_parse(bad, n, &out, &payload) == HML_ERR_PROTO);
  }
  E2E_OK(h, "ctrl.reject.truncated",
         hml_ctrl_parse(msg, (size_t)n - 1u, &out, &payload) == HML_ERR_PROTO);
  E2E_OK(h, "ctrl.reject.trailing_byte",
         hml_ctrl_parse(msg, (size_t)n + 1u, &out, &payload) == HML_ERR_PROTO);
  {
    uint8_t bad[sizeof(msg)];
    memcpy(bad, msg, n);
    bad[n - 1u] ^= 0x01u; /* last CRC byte */
    E2E_OK(h, "ctrl.reject.bad_crc",
           hml_ctrl_parse(bad, n, &out, &payload) == HML_ERR_CRC);
  }
  {
    uint8_t bad[sizeof(msg)];
    memcpy(bad, msg, n);
    hml_put_be16(bad + 8u, HML_CTRL_MAX_PAYLOAD + 1u); /* over-long payload_len */
    E2E_OK(h, "ctrl.reject.overlong_payload",
           hml_ctrl_parse(bad, n, &out, &payload) == HML_ERR_PROTO);
  }

  E2E_OK(h, "cfg.reject.short",
         hml_proto_cfg_parse(buf, HML_CFG_SIZE - 1u, &cfg_out) == HML_ERR_PROTO);
  {
    uint8_t bad[HML_CFG_SIZE];
    memcpy(bad, buf, sizeof(bad));
    bad[0] = 7u; /* neither UDP nor RoCEv2 */
    E2E_OK(h, "cfg.reject.bad_kind",
           hml_proto_cfg_parse(bad, sizeof(bad), &cfg_out) == HML_ERR_PROTO);
  }
  {
    uint8_t bad[HML_CFG_SIZE];
    memcpy(bad, buf, sizeof(bad));
    bad[1] = 1u; /* pad0 */
    E2E_OK(h, "cfg.reject.pad0",
           hml_proto_cfg_parse(bad, sizeof(bad), &cfg_out) == HML_ERR_PROTO);
  }
  {
    uint8_t bad[HML_CFG_SIZE];
    memcpy(bad, buf, sizeof(bad));
    bad[35] = 1u; /* pad1 */
    E2E_OK(h, "cfg.reject.pad1",
           hml_proto_cfg_parse(bad, sizeof(bad), &cfg_out) == HML_ERR_PROTO);
  }
  {
    uint8_t bad[HML_CFG_SIZE];
    memcpy(bad, buf, sizeof(bad));
    bad[36] = 1u; /* rsvd */
    E2E_OK(h, "cfg.reject.rsvd",
           hml_proto_cfg_parse(bad, sizeof(bad), &cfg_out) == HML_ERR_PROTO);
  }
}

/* ---- phase 3: real hml_control server + real hml_client over loopback ------- */

static int server_lifecycle(e2e_harness *h) {
  uint16_t base = 0u;
  for (unsigned i = 0; i + 3u <= E2E_PORT_COUNT; i++) {
    uint16_t cand = (uint16_t)(E2E_PORT_BASE + i);
    if (e2e_port_free(cand) && e2e_port_free((uint16_t)(cand + 1u)) &&
        e2e_port_free((uint16_t)(cand + 2u))) {
      base = cand;
      break;
    }
  }
  if (base == 0u) {
    e2e_emit(h, "ctrl.ports_available", false, "no 3-port window free in the private range");
    return 1;
  }
  uint16_t ctrl_port = base;
  uint16_t client_port = (uint16_t)(base + 1u);
  uint16_t disc_port = (uint16_t)(base + 2u);

  hml_control_cfg cc;
  memset(&cc, 0, sizeof(cc));
  cc.ctrl_port = ctrl_port;
  cc.disc_port = 0u; /* the beacon is delivered by the loopback sender below */
  cc.beacon_enable = false;
  cc.uuid = BEACON_UUID;
  cc.caps = BEACON_CAPS;
  cc.fw_ver = BEACON_FW;
  cc.reply_ip = HML_IP_LOOPBACK;
  cc.reply_port = client_port;

  hml_control_t *srv = NULL;
  hml_status st = hml_control_open(&srv, &cc);
  if (st != HML_OK) {
    char d[96];
    (void)snprintf(d, sizeof(d), "hml_control_open status=%d", (int)st);
    e2e_emit(h, "ctrl.server_open", false, d);
    return 1;
  }
  srvstate ss;
  memset(&ss, 0, sizeof(ss));
  ss.c = srv;
  st = hml_control_set_cbs(srv, on_set_dest, on_start, on_stop, NULL, &ss);
  if (st != HML_OK) {
    e2e_emit(h, "ctrl.cb_set", false, "hml_control_set_cbs failed");
    hml_control_close(srv);
    return 1;
  }

  pthread_t tid;
  if (pthread_create(&tid, NULL, srv_thread, &ss) != 0) {
    e2e_emit(h, "ctrl.thread_create", false, "pthread_create failed");
    hml_control_close(srv);
    return 1;
  }

  hml_client_t *c = NULL;
  st = hml_client_open(&c, client_port);
  if (st != HML_OK) {
    char d[96];
    (void)snprintf(d, sizeof(d), "hml_client_open status=%d", (int)st);
    e2e_emit(h, "ctrl.client_open", false, d);
    atomic_store(&ss.done, 1);
    (void)pthread_join(tid, NULL);
    hml_control_close(srv);
    return 1;
  }
  (void)hml_client_set_timeout(c, 2000u);
  st = hml_client_set_peer(c, HML_IP_LOOPBACK, ctrl_port, disc_port);
  if (st != HML_OK) {
    e2e_emit(h, "ctrl.client_peer", false, "hml_client_set_peer failed");
    hml_client_close(c);
    atomic_store(&ss.done, 1);
    (void)pthread_join(tid, NULL);
    hml_control_close(srv);
    return 1;
  }

  /* deliver the server's own beacon over a real loopback socket to the socket
   * hml_client_set_peer created for discovery. */
  {
    hml_beacon b;
    uint8_t wire[HML_DISC_SIZE];
    memset(&b, 0, sizeof(b));
    b.version = (uint8_t)HML_DISC_VERSION;
    b.ttl = 1u;
    b.uuid = BEACON_UUID;
    b.caps = BEACON_CAPS;
    b.fw_ver = BEACON_FW;
    b.ts_us = BEACON_TS;
    hml_beacon_fill(&b, wire);
    if (raw_send(disc_port, wire, HML_DISC_SIZE) != 0) {
      e2e_emit(h, "ctrl.beacon_send", false, "loopback beacon send failed");
      hml_client_close(c);
      atomic_store(&ss.done, 1);
      (void)pthread_join(tid, NULL);
      hml_control_close(srv);
      return 1;
    }
    uint32_t u = 0u, caps = 0u, fw = 0u;
    uint64_t ts = 0u;
    st = hml_client_discover(c, 2000, &u, &caps, &fw, &ts);
    E2E_OK(h, "ctrl.discover_ok", st == HML_OK && u == BEACON_UUID && caps == BEACON_CAPS &&
                                     fw == BEACON_FW && ts == BEACON_TS);
  }

  uint32_t tok = 0u;
  E2E_OK(h, "ctrl.ping_echo", hml_client_ping(c, 0x12345678u, &tok) == HML_OK && tok == 0x12345678u);

  hml_dest_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.kind = HML_XPORT_UDP;
  cfg.ipv4 = 0x7f000001u;
  cfg.udp_port = 45701u;
  cfg.qp = 0x11223344u;
  cfg.frame_size = 4096u;
  cfg.window_len = 64u;
  cfg.hop_limit = 64u;
  E2E_OK(h, "ctrl.set_dest", hml_client_set_dest(c, &cfg) == HML_OK);
  E2E_OK(h, "ctrl.set_dest_cb", wait_u32(&ss.set_dest, 1u, 2000));

  hml_dest_cfg got;
  memset(&got, 0, sizeof(got));
  hml_dest_cfg server_cfg;
  memset(&server_cfg, 0, sizeof(server_cfg));
  E2E_OK(h, "ctrl.get_dest", hml_client_get_dest(c, &got) == HML_OK &&
                                 hml_control_get_dest(srv, &server_cfg) &&
                                 got.kind == cfg.kind && got.ipv4 == cfg.ipv4 &&
                                 got.udp_port == cfg.udp_port && got.qp == cfg.qp);

  uint32_t cu = 0u, ccaps = 0u, cfw = 0u;
  E2E_OK(h, "ctrl.get_caps",
         hml_client_get_caps(c, &cu, &ccaps, &cfw) == HML_OK && cu == BEACON_UUID &&
             ccaps == BEACON_CAPS && cfw == BEACON_FW);

  bool streaming = true;
  E2E_OK(h, "ctrl.status_not_streaming", hml_client_status(c, &streaming) == HML_OK && !streaming);
  E2E_OK(h, "ctrl.start", hml_client_start(c) == HML_OK && hml_control_streaming(srv));
  E2E_OK(h, "ctrl.start_cb", wait_u32(&ss.start, 1u, 2000));
  E2E_OK(h, "ctrl.status_streaming", hml_client_status(c, &streaming) == HML_OK && streaming);
  E2E_OK(h, "ctrl.stop", hml_client_stop(c) == HML_OK && !hml_control_streaming(srv));
  E2E_OK(h, "ctrl.stop_cb", wait_u32(&ss.stop, 1u, 2000));

  /* reset clears the configured destination: the next get_dest must come back
   * as an HML_ERR_NOTSUP carrying the server's HML_ERR_STATE. */
  E2E_OK(h, "ctrl.reset", hml_client_reset(c) == HML_OK);
  hml_dest_cfg gone;
  memset(&gone, 0, sizeof(gone));
  E2E_OK(h, "ctrl.reset_cleared",
         hml_client_get_dest(c, &gone) == HML_ERR_NOTSUP &&
             hml_client_last_device_status(c) == HML_ERR_STATE &&
             !hml_control_get_dest(srv, &gone));

  /* Unknown opcode: the server must answer HML_OP_ERROR, not hang or crash.
   * The v1 client matches a response by op, and HML_OP_ERROR matches any
   * request (documented rule), so the stale error that answers the raw
   * unknown-op request is consumed by the client's NEXT call; the two PING
   * requests that call may also have had time to be served, so the scene
   * drains until a fresh request gets the fresh echo. */
  {
    uint32_t rx_before = hml_control_rx_count(srv);
    uint32_t bad_before = hml_control_bad_count(srv);
    hml_ctrl_msg q;
    uint8_t buf[HML_CTRL_HDR_SIZE + 2u];
    uint16_t qn = 0u;
    memset(&q, 0, sizeof(q));
    q.op = 0x33u;
    q.flags = (uint16_t)HML_CTRL_FLAG_REQ;
    hml_ctrl_fill(&q, buf, (uint16_t)sizeof(buf), &qn);
    bool sent = raw_send(ctrl_port, buf, qn) == 0;
    wait_server_rx(&ss, rx_before + 1u, 2000);
    E2E_OK(h, "ctrl.unknown_op_served",
           sent && hml_control_rx_count(srv) > rx_before &&
               hml_control_bad_count(srv) == bad_before);
    uint32_t stale = 0xffffffffu;
    E2E_OK(h, "ctrl.stale_error_consumed",
           hml_client_ping(c, 1u, &stale) == HML_ERR_NOTSUP &&
               hml_client_last_device_status(c) == HML_ERR_NOTSUP);
    /* v1 matching is by op, not by request token, so an echo left queued by an
     * earlier ping may answer a later one. The point of this case is not the
     * token value: it is that the device keeps answering after the error. */
    uint32_t tok2 = 0u;
    E2E_OK(h, "ctrl.recovers_after_error", hml_client_ping(c, 1u, &tok2) == HML_OK);
    uint8_t op_now = 0u;
    uint16_t flags_now = 0u, len_now = 0u;
    hml_control_last_req(srv, &op_now, &flags_now, &len_now);
    E2E_OK(h, "ctrl.server_last_req_is_ping",
           op_now == HML_OP_PING && (flags_now & HML_CTRL_FLAG_REQ) != 0u);
  }

  /* A malformed datagram is accounted, not answered: bad_count increments and
   * the client never sees anything it could mistake for a response. */
  {
    uint32_t bad_before = hml_control_bad_count(srv);
    uint8_t junk[9] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    bool sent = raw_send(ctrl_port, junk, sizeof(junk)) == 0;
    wait_server_rx(&ss, hml_control_rx_count(srv) + 1u, 2000);
    E2E_OK(h, "ctrl.garbage_accounted",
           sent && hml_control_bad_count(srv) == bad_before + 1u &&
               hml_control_rx_count(srv) >= bad_before + 1u);
    uint32_t tok3 = 0u;
    E2E_OK(h, "ctrl.garbage_no_poison", hml_client_ping(c, 2u, &tok3) == HML_OK);
  }

  hml_client_close(c);
  atomic_store(&ss.done, 1);
  (void)pthread_join(tid, NULL);
  hml_control_close(srv);
  return e2e_finish(h) == 0 ? 0 : 1;
}

/* ---- scene ---------------------------------------------------------------- */

int e2e_scene_control(e2e_harness *h) {
  if (e2e_budget_ms() == -1) {
    e2e_emit(h, "late_start", false, "run-wide deadline already expired");
    return 1;
  }
  phase_beacon_golden(h);
  phase_control_codec(h);
  return server_lifecycle(h);
}