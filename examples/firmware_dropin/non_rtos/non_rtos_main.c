/*
 * non_rtos_main.c -- a "generic firmware" that links hml as a drop-in.
 *
 * What this is: the smallest main() that a board firmware could have while
 * still doing real hml work -- encode a frame, hand it to a transport, receive
 * a frame, and report the counters. It links hml_core and hml_protocol and
 * NOTHING else. There is no hml_port_posix, no <sys/socket.h>, no <time.h> in
 * this file, and no POSIX header anywhere in the link line.
 *
 * What it deliberately does NOT do: open a socket. This demo's port stub
 * answers HML_ERR_NOTSUP for the hml_sock_* family, which is the honest answer
 * for a board whose network stack is not wired up yet, and it is *safe*: the
 * send path refuses to advance next_seq/frag_idx/frame_bytes on a non-HML_OK
 * return, so nothing is marked as transmitted that was not. See
 * docs/porting.md 2.2.
 *
 * The other demo, ../linux/, is the same code shape with a real loopback UDP
 * port and a staged install of the library.
 */

#include <hml/hml_crc.h>
#include <hml/hml_partition.h>
#include <hml/hml_protocol.h>
#include <hml/hml_reasm.h>
#include <hml/hml_stream.h>
#include <hml/hml_transport.h>

#include <stdio.h>
#include <string.h>

/* ---- a transport that records instead of sending ------------------------ */

#define LOOPBACK_CAP 64

typedef struct loopback {
  uint8_t dg[LOOPBACK_CAP][HML_MAX_FRAG_SIZE];
  size_t len[LOOPBACK_CAP];
  unsigned n;
} loopback;

static loopback g_lb;

static hml_status lb_open(hml_xport_t *x, const hml_dest_cfg *dest, uint16_t stream_id) {
  (void)dest;
  (void)stream_id;
  if (!x) return HML_ERR_ARG;
  x->impl = NULL;
  memset(&g_lb, 0, sizeof(g_lb));
  return HML_OK;
}

/* Byte-for-byte the shaping that src/transports/udp/hml_udp.c does, minus the
 * hml_sock_send() at the end. That is the whole of a T0 datagram. */
static hml_status lb_send_frag(hml_xport_t *x, const hml_frame_hdr *hdr,
                               const uint8_t *payload, size_t payload_len) {
  (void)x;
  if (!hdr) return HML_ERR_ARG;
  if (!payload && payload_len > 0) return HML_ERR_ARG;
  if (payload_len > (size_t)HML_MAX_FRAG_PAYLOAD) return HML_ERR_TOOLARGE;
  if (g_lb.n >= LOOPBACK_CAP) return HML_ERR_NOMEM;

  hml_frame_hdr h = *hdr;
  h.payload_len = (uint16_t)payload_len;
  hml_proto_frame_fill(&h, g_lb.dg[g_lb.n]);
  if (payload_len > 0) {
    memcpy(g_lb.dg[g_lb.n] + HML_OFF_PAYLOAD, payload, payload_len);
  }
  hml_put_be16(g_lb.dg[g_lb.n] + HML_OFF_PAYLOAD + payload_len, hml_crc16(payload, payload_len));
  g_lb.len[g_lb.n] = (size_t)HML_FRAME_HDR_SIZE + payload_len + (size_t)HML_TRAIL_CRC_SIZE;
  g_lb.n++;
  return HML_OK;
}

static hml_status lb_flush(hml_xport_t *x) {
  (void)x;
  return HML_OK;
}

static void lb_close(hml_xport_t *x) {
  if (x) x->impl = NULL;
}

static const hml_xport_ops g_lb_ops = {"loopback", lb_open, lb_send_frag, lb_flush, lb_close};

/* ---- reassembly callback ------------------------------------------------ */

typedef struct rxstat {
  unsigned callbacks;    /* every EOF callback, data frames and flushes alike */
  unsigned data_frames; /* callbacks that carried a payload */
  unsigned eof_flushes;  /* zero-payload HML_FLAG_END: EOF without data */
  size_t bytes;
  uint16_t stream_id;
  uint32_t seq;
  uint16_t crc;
} rxstat;

static void on_frame(void *ctx, uint16_t stream_id, uint32_t frame_seq, const uint8_t *frame,
                     size_t frame_bytes, const hml_frame_hdr *final_hdr) {
  rxstat *r = (rxstat *)ctx;
  (void)final_hdr;
  r->callbacks++;
  if (frame_bytes == 0u) { /* EOF-only flush: counted, never a data frame */
    r->eof_flushes++;
    return;
  }
  r->data_frames++;
  r->bytes += frame_bytes;
  r->stream_id = stream_id;
  r->seq = frame_seq;
  r->crc = hml_crc16(frame, frame_bytes);
}

int main(void) {
  int rc = 0;

  printf("hml %s, linkage=%s, posix_port_available=%d\n", HML_VERSION_STRING, HML_LINKAGE_NAME,
         HML_HAS_POSIX_PORT);
  printf("port contract symbols required: %s\n", HML_LIBC_REQUIREMENTS);

  /* 1. CRC check vectors. Wrong values here mean the wire is wrong. */
  {
    uint16_t c16 = hml_crc16("123456789", 9);
    uint32_t c32 = hml_crc32_ieee("123456789", 9);
    printf("[crc]   crc16(\"123456789\")=0x%04x crc32=0x%08x\n", c16, c32);
    if (c16 != 0x29b1u || c32 != 0xcbf43926u) {
      printf("[crc]   FAIL expected crc16=0x29b1 crc32=0xcbf43926\n");
      rc = 1;
    }
  }

  /* 2. Build a frame through the framer and hand it to a transport. */
  static const uint8_t frame[] = "hml drop-in demo: 2 frames, no POSIX in the link";
  hml_xport_t xport;
  hml_stream_t *s = NULL;
  hml_xport_ops ops = g_lb_ops;

  if (hml_transport_register(&ops) != HML_OK) {
    printf("[xport] FAIL register\n");
    return 1;
  }
  xport.ops = &ops;
  xport.impl = NULL;
  if (ops.open(&xport, NULL, 1u) != HML_OK) {
    printf("[xport] FAIL open\n");
    return 1;
  }

  hml_stream_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.stream_id = 7u;
  cfg.max_frame_bytes = sizeof(frame);
  cfg.seq_start = 100u;
  cfg.mtu_effective = (uint32_t)HML_MAX_FRAG_PAYLOAD;

  if (hml_stream_open(&s, &cfg, &xport) != HML_OK) {
    printf("[stream] FAIL open\n");
    return 1;
  }

  uint64_t ts0 = hml_clock_us();
  hml_status st = hml_stream_send_frag(s, frame, sizeof(frame) - 1u, true, true, ts0);
  if (st != HML_OK) {
    printf("[stream] FAIL send_frag (%d)\n", (int)st);
    rc = 1;
  } else {
    printf("[stream] one frame, %u bytes, %u datagram(s), next_seq=%u\n",
           (unsigned)(sizeof(frame) - 1u), g_lb.n, hml_stream_next_seq(s));
  }
  /* A zero-payload END: the EOF-only flush the protocol defines. */
  if (hml_stream_flush_eof(s, hml_clock_us()) == HML_OK) {
    printf("[stream] EOF-only flush emitted, next_seq=%u\n", hml_stream_next_seq(s));
  }
  hml_stream_close(s);

  /* 3. Reassemble what the transport produced, with a small explicit cap.
   *    The cap is a promise that frames never exceed it, not a buffer knob:
   *    a larger frame is poisoned and dropped whole. See docs/limitations.md 3.1. */
  {
    rxstat rx;
    memset(&rx, 0, sizeof(rx));
    hml_reasm_ctx_t *r = NULL;
    if (hml_reasm_open(&r, sizeof(frame)) != HML_OK) {
      printf("[reasm] FAIL open\n");
      return 1;
    }
    hml_reasm_set_eof_cb(r, on_frame, &rx);
    for (unsigned i = 0; i < g_lb.n; i++) {
      if (hml_reasm_feed_datagram(r, g_lb.dg[i], g_lb.len[i]) != HML_OK) {
        printf("[reasm] FAIL feed %u\n", i);
        rc = 1;
      }
    }
    hml_reasm_stats rs;
    hml_reasm_get_stats(r, &rs);
    printf("[reasm]  frames_ok=%u dropped=%u frags=%u gaps=%u crc_fail=%u proto_fail=%u\n",
           rs.frames_ok, rs.frames_dropped, rs.frags_recv, rs.gaps, rs.crc_fail, rs.proto_fail);
    printf("[reasm]  delivered data_frames=%u bytes=%zu stream_id=%u seq=%u crc16=0x%04x, "
           "callbacks=%u (eof_flushes=%u)\n",
           rx.data_frames, rx.bytes, rx.stream_id, rx.seq, rx.crc, rx.callbacks, rx.eof_flushes);
    if (rx.data_frames != 1u || rx.bytes != sizeof(frame) - 1u || rx.callbacks != 2u ||
        rx.eof_flushes != 1u) {
      printf("[reasm] FAIL expected 1 data frame of %u bytes, 1 EOF-only flush, 2 callbacks\n",
             (unsigned)(sizeof(frame) - 1u));
      rc = 1;
    }
    hml_reasm_close(r);
  }

  /* 4. The parser fails closed. A flipped magic must be rejected, not
   *    partially accepted. */
  {
    uint8_t dg[HML_MAX_FRAG_SIZE];
    memcpy(dg, g_lb.dg[0], g_lb.len[0]);
    dg[HML_OFF_MAGIC + 0] ^= 0xffu;
    hml_frame_hdr hdr;
    const uint8_t *pay = NULL;
    if (hml_proto_frame_parse(dg, g_lb.len[0], &hdr, &pay) == HML_OK) {
      printf("[parse] FAIL a corrupted magic was accepted\n");
      rc = 1;
    } else {
      printf("[parse] corrupted magic rejected (fail-closed)\n");
    }
  }

  printf("%s: non-RTOS drop-in demo %s\n", rc == 0 ? "PASS" : "FAIL", rc == 0 ? "4/4" : "incomplete");
  return rc;
}
