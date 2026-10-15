/*
 * scene_udp.c -- T0 end-to-end over a real loopback socket.
 *
 * The receiver is the shipping hml_host (host/hml_host): it opens a real UDP
 * socket, parses every datagram with hml_proto_frame_parse, reassembles with
 * hml_reasm and hashes each delivered frame with hml_fnv1a32. The sender is
 * the shipping T0 path: hml_synth (adapters/synthetic) feeds hml_stream
 * (src/core) through the real hml_udp transport (src/transports/udp), which
 * re-serialises the HML header and the trailing payload CRC itself.
 *
 * Three phases, each with its own exact counter assertion:
 *
 *   clean     N frames x 9000 B in emission order. Every frame must arrive
 *             bit-exact (FNV-1a from the receiver == FNV-1a computed by the
 *             sender), and gaps/crc_fail/proto_fail/frames_dropped must all
 *             stay at 0 on a lossless loopback.
 *   reordered The same frame, but the transport shim below holds every
 *             datagram of a frame and emits them on the real socket in
 *             REVERSE order. Reordering on the wire is the point: the frame
 *             must still assemble byte-exact, which is the index-order gather
 *             rather than an append-in-arrival-order. The shim serialises with
 *             hml_proto_frame_fill + hml_crc16 and calls hml_sock_send, so the
 *             bytes are the same ones src/transports/udp produces.
 *   eof-only  A zero-payload HML_FLAG_END datagram must fire exactly one
 *             zero-byte callback and must not increment frames_ok.
 *
 * NOT COVERED: anything that needs a real link. No loss, no reordering from a
 * real NIC, no MTU discovery. Loopback is a reliable, in-order, loopback-
 * buffered path, so "gaps == 0" here says the clean path is clean, not that
 * loss is handled.
 */

#include "e2e_cpu.h"

#include "hml/hml_latency.h"
#include "hml/hml_sock.h"
#include "hml/hml_stream.h"
#include "hml/hml_transport.h"
#include "hml_host.h"
#include "hml_synth_priv.h"
#include "hml_udp_priv.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_BYTES 9000u
#define FRAGS_PER_FRAME ((FRAME_BYTES + HML_MAX_FRAG_PAYLOAD - 1u) / HML_MAX_FRAG_PAYLOAD)
#define STREAM_ID 1u
#define SEED 1u
/* Credits: never run more than this many frames ahead of the receiver, so the
 * kernel socket buffer cannot overrun and a "clean" phase really is lossless. */
#define WINDOW 8u
/* Datagrams the sender may put on the wire beyond what the receiver has
 * already pulled off the socket and fed into reassembly. */
#define INFLIGHT 64u

/* ---- receiver ------------------------------------------------------------ */

typedef struct rxstate {
  hml_host_t *host;
  atomic_int run;
  atomic_uint errors;
  atomic_ullong calls;
  atomic_ullong bad_hash;
  atomic_ullong bad_size;
  atomic_ullong dup;
  atomic_ullong zero_byte;
  atomic_ullong bytes;
  uint32_t *want_hash;
  unsigned want_n;
  pthread_mutex_t lock;
  hml_reasm_stats rs;
  hml_latency lat;
  uint64_t frames;
  uint64_t eof;
} rxstate;

static void on_frame(void *ctx, uint16_t stream_id, uint32_t frame_seq, size_t frame_bytes,
                     uint32_t hash) {
  rxstate *r = (rxstate *)ctx;
  (void)stream_id;
  (void)atomic_fetch_add(&r->calls, 1ull);
  if (frame_bytes == 0u) {
    (void)atomic_fetch_add(&r->zero_byte, 1ull);
    return;
  }
  (void)atomic_fetch_add(&r->bytes, (unsigned long long)frame_bytes);
  /* frame_seq is per-fragment, so the frame index is seq / FRAGS_PER_FRAME. */
  unsigned idx = (unsigned)(frame_seq / FRAGS_PER_FRAME);
  if (frame_bytes != (size_t)FRAME_BYTES) {
    (void)atomic_fetch_add(&r->bad_size, 1ull);
    return;
  }
  if (idx >= r->want_n) {
    (void)atomic_fetch_add(&r->dup, 1ull);
    return;
  }
  if (hash != r->want_hash[idx]) (void)atomic_fetch_add(&r->bad_hash, 1ull);
}

static void *rx_thread(void *arg) {
  rxstate *r = (rxstate *)arg;
  while (atomic_load(&r->run)) {
    hml_status st = hml_host_poll(r->host, 20);
    if (st != HML_OK && st != HML_ERR_TIMEOUT && st != HML_ERR_AGAIN)
      (void)atomic_fetch_add(&r->errors, 1u);
    pthread_mutex_lock(&r->lock);
    hml_host_stats(r->host, &r->rs, &r->frames, NULL, &r->eof, NULL);
    (void)hml_host_latency(r->host, &r->lat);
    pthread_mutex_unlock(&r->lock);
  }
  return NULL;
}

static void snapshot(rxstate *r, hml_reasm_stats *rs, hml_latency *lat, uint64_t *frames,
                     uint64_t *eof) {
  pthread_mutex_lock(&r->lock);
  if (rs) *rs = r->rs;
  if (lat) *lat = r->lat;
  if (frames) *frames = r->frames;
  if (eof) *eof = r->eof;
  pthread_mutex_unlock(&r->lock);
}

static bool wait_calls(rxstate *r, unsigned long long target, int ms) {
  uint64_t deadline = e2e_now_us() + (uint64_t)(ms < 0 ? 0 : (unsigned)ms) * 1000ull;
  while (atomic_load(&r->calls) < target) {
    if (e2e_budget_ms() == -1) return false;
    if (e2e_now_us() >= deadline) return false;
    e2e_sleep_us(500);
  }
  return true;
}

/* ---- sender -------------------------------------------------------------- */

static hml_sensor_t g_sensor;
static uint8_t g_fbuf[FRAME_BYTES];
static rxstate *g_rx;
static uint64_t g_sent_frags;
/* True while the reorder shim is the active transport: its datagrams only reach
 * the wire at flush(), so flow_credit must not run while a frame is staged. */
static bool g_buffering;

/* Wait until the receiver has drained all but INFLIGHT of the datagrams we
 * have put on the wire. The reassembler's frags_recv is exactly the point at
 * which a datagram has left the kernel receive queue, which is what bounds the
 * socket buffer. */
static void flow_credit(void) {
  if (g_buffering) return;
  int budget = e2e_budget_ms();
  uint64_t give_up = e2e_now_us() + (uint64_t)(budget > 0 ? budget : 5000) * 1000ull;
  while (atomic_load(&g_rx->rs.frags_recv) + INFLIGHT < g_sent_frags) {
    if (e2e_budget_ms() == -1 || e2e_now_us() >= give_up) return;
    e2e_sleep_us(200);
  }
}

static hml_status send_frame(hml_stream_t *s, uint64_t frame_index) {
  hml_synth_pattern_fill(g_fbuf, FRAME_BYTES, frame_index * FRAME_BYTES, SEED);
  size_t off = 0;
  int first = 1;
  while (off < FRAME_BYTES) {
    size_t want = FRAME_BYTES - off;
    if (want > (size_t)HML_MAX_FRAG_PAYLOAD) want = (size_t)HML_MAX_FRAG_PAYLOAD;
    size_t got = 0;
    hml_status st = g_sensor.ops->acquire(&g_sensor, g_fbuf + off, want, &got);
    if (st != HML_OK || got == 0u) return st == HML_OK ? HML_ERR_STATE : st;
    int last = (off + got == FRAME_BYTES) != 0;
    for (int tries = 0; tries < 2000; tries++) {
      st = hml_stream_send_frag(s, g_fbuf + off, got, first != 0, last != 0, e2e_now_us());
      if (st != HML_ERR_AGAIN) break;
      e2e_sleep_us(200);
    }
    if (st != HML_OK) return st;
    g_sent_frags++;
    off += got;
    first = 0;
    flow_credit();
  }
  return g_sensor.ops->release(&g_sensor);
}

/* ---- reordering transport shim ------------------------------------------- */

/*
 * Wraps the real hml_udp ops but buffers one whole frame and flushes it in
 * reverse index order over the real socket. Serialisation is the same
 * hml_proto_frame_fill + trailing hml_crc16 the udp transport performs, so the
 * bytes on the wire are byte-identical to a clean run; only their order
 * differs. Reversed arrival is legal (hml_reasm holds a frame pending until a
 * later datagram proves the stragglers will not arrive), so a correct receiver
 * must still deliver the frame byte-exact.
 */
#define REORDER_SLOTS 16u

typedef struct reorder {
  hml_sock_t *sock;
  uint8_t dg[REORDER_SLOTS][HML_MAX_FRAG_SIZE];
  size_t len[REORDER_SLOTS];
  unsigned n;
  uint64_t flushed;
  uint64_t frames;
} reorder;

static hml_status reorder_open(hml_xport_t *x, const hml_dest_cfg *dest, uint16_t stream_id) {
  reorder *r = (reorder *)x->impl;
  if (r == NULL || dest == NULL) return HML_ERR_ARG;
  (void)stream_id;
  r->n = 0u;
  hml_status st = hml_sock_udp_create(&r->sock, 0u, 0u, false);
  if (st == HML_OK)
    st = hml_sock_set_dest(r->sock, hml_ipv4_ne_to_host(dest->ipv4), dest->udp_port);
  if (st != HML_OK) {
    hml_sock_close(r->sock);
    r->sock = NULL;
    return st;
  }
  return HML_OK;
}

static hml_status reorder_send(hml_xport_t *x, const hml_frame_hdr *hdr, const uint8_t *payload,
                               size_t payload_len) {
  reorder *r = (reorder *)x->impl;
  if (payload_len > (size_t)HML_MAX_FRAG_PAYLOAD) return HML_ERR_TOOLARGE;
  if (r->n >= REORDER_SLOTS) return HML_ERR_TOOLARGE;
  hml_frame_hdr h = *hdr;
  h.payload_len = (uint16_t)payload_len;
  r->len[r->n] = e2e_build_dg(r->dg[r->n], (size_t)HML_MAX_FRAG_SIZE, h.stream_id, h.seq,
                              h.frag_idx, h.frag_cnt, h.flags, h.ts_us, payload,
                              (uint16_t)payload_len);
  if (r->len[r->n] == 0u) return HML_ERR_ARG;
  r->n++;
  return HML_OK;
}

static hml_status reorder_flush(hml_xport_t *x) {
  reorder *r = (reorder *)x->impl;
  for (unsigned i = r->n; i > 0u; i--) {
    hml_status st = hml_sock_send(r->sock, r->dg[i - 1u], r->len[i - 1u]);
    if (st == HML_OK) r->flushed++;
    if (st == HML_ERR_AGAIN) e2e_sleep_us(200);
  }
  r->n = 0u;
  r->frames++;
  return HML_OK;
}

static void reorder_close(hml_xport_t *x) {
  reorder *r = (reorder *)x == NULL || x->impl == NULL ? NULL : (reorder *)x->impl;
  if (r == NULL) return;
  if (r->sock) hml_sock_close(r->sock);
  r->sock = NULL;
}

static const hml_xport_ops g_reorder_ops = {"reorder-udp", reorder_open, reorder_send,
                                            reorder_flush, reorder_close};

/* ---- scene --------------------------------------------------------------- */

int e2e_scene_udp(e2e_harness *h) {
  uint16_t port = e2e_port_pick();
  if (port == 0u) {
    e2e_emit(h, "port_available", false, "no free port in the private loopback range");
    return 1;
  }
  e2e_eq_u(h, "port_bound", port, port);

  unsigned n = g_e2e_frames;
  unsigned reorder_frames = 4u;

  rxstate rx;
  memset(&rx, 0, sizeof(rx));
  (void)pthread_mutex_init(&rx.lock, NULL);
  rx.want_n = n + reorder_frames + 1u;
  rx.want_hash = (uint32_t *)calloc(rx.want_n, sizeof(uint32_t));
  if (rx.want_hash == NULL) {
    e2e_emit(h, "alloc", false, "out of memory");
    return 1;
  }

  hml_status st = hml_host_open(&rx.host, port, (size_t)FRAME_BYTES + (size_t)HML_MAX_FRAG_PAYLOAD);
  if (st != HML_OK) {
    char d[96];
    (void)snprintf(d, sizeof(d), "hml_host_open status=%d port=%u", (int)st, (unsigned)port);
    e2e_emit(h, "receiver_open", false, d);
    free(rx.want_hash);
    return 1;
  }
  e2e_case(h, "receiver_open", true);
  (void)hml_host_set_frame_cb(rx.host, on_frame, &rx);

  g_rx = &rx;
  atomic_store(&rx.run, 1);
  pthread_t rxt;
  if (pthread_create(&rxt, NULL, rx_thread, &rx) != 0) {
    e2e_emit(h, "receiver_thread", false, "pthread_create failed");
    hml_host_close(rx.host);
    free(rx.want_hash);
    return 1;
  }
  e2e_case(h, "receiver_thread", true);
  e2e_sleep_us(20000);

  st = hml_udp_register();
  e2e_eq_i(h, "udp_register", (long)st, (long)HML_OK);
  e2e_case(h, "udp_registry_lookup", hml_transport_lookup("udp") == hml_udp_ops());

  hml_synth_cfg scfg;
  memset(&scfg, 0, sizeof(scfg));
  scfg.seed = SEED;
  scfg.frame_bytes = (size_t)HML_MAX_FRAG_PAYLOAD;
  memset(&g_sensor, 0, sizeof(g_sensor));
  g_sensor.ops = hml_synth_ops();
  st = g_sensor.ops->open(&g_sensor, &scfg);
  e2e_eq_i(h, "sensor_open", (long)st, (long)HML_OK);

  hml_dest_cfg dest;
  memset(&dest, 0, sizeof(dest));
  dest.kind = HML_XPORT_UDP;
  dest.ipv4 = 0x7F000001u; /* 127.0.0.1, network byte order */
  dest.udp_port = port;
  dest.frame_size = FRAME_BYTES;
  dest.hop_limit = 64u;

  for (unsigned i = 0; i < n; i++) {
    hml_synth_pattern_fill(g_fbuf, FRAME_BYTES, (uint64_t)i * FRAME_BYTES, SEED);
    rx.want_hash[i] = hml_fnv1a32(g_fbuf, FRAME_BYTES);
  }

  /* ---- phase clean ---- */
  g_sent_frags = 0;
  hml_xport_t x;
  memset(&x, 0, sizeof(x));
  x.ops = hml_udp_ops();
  st = x.ops->open(&x, &dest, STREAM_ID);
  e2e_eq_i(h, "clean_transport_open", (long)st, (long)HML_OK);
  st = x.ops->open(&x, &dest, STREAM_ID);
  e2e_eq_i(h, "clean_double_open_refused", (long)st, (long)HML_ERR_STATE);

  hml_stream_t *s = NULL;
  hml_stream_cfg sc;
  memset(&sc, 0, sizeof(sc));
  sc.stream_id = STREAM_ID;
  sc.max_frame_bytes = FRAME_BYTES;
  sc.seq_start = 0;
  sc.mtu_effective = 1500u;
  st = hml_stream_open(&s, &sc, &x);
  e2e_eq_i(h, "clean_stream_open", (long)st, (long)HML_OK);

  int send_rc = 0;
  for (unsigned i = 0; i < n; i++) {
    if (g_e2e_deadline != 0u && e2e_budget_ms() == -1) {
      e2e_emit(h, "clean_send_bounded", false, "deadline hit before all frames were sent");
      send_rc = 1;
      break;
    }
    if (send_frame(s, (uint64_t)i) != HML_OK) {
      char d[96];
      (void)snprintf(d, sizeof(d), "send_frame index=%u failed", i);
      e2e_emit(h, "clean_send", false, d);
      send_rc = 1;
      break;
    }
    if (g_sent_frags - atomic_load(&rx.calls) * FRAGS_PER_FRAME >= WINDOW * FRAGS_PER_FRAME) {
      uint64_t give_up = e2e_now_us() + 5000000ull;
      while (g_sent_frags / FRAGS_PER_FRAME >= atomic_load(&rx.calls) + WINDOW) {
        if (e2e_now_us() >= give_up || e2e_budget_ms() == -1) break;
        e2e_sleep_us(200);
      }
    }
  }
  e2e_eq_i(h, "clean_stream_next_seq", (long)hml_stream_next_seq(s),
           (long)((uint64_t)n * FRAGS_PER_FRAME));
  e2e_eq_u(h, "clean_datagrams_on_wire", g_sent_frags, (uint64_t)n * FRAGS_PER_FRAME);
  (void)hml_stream_close(s);

  if (!wait_calls(&rx, n, 20000)) {
    char d[128];
    (void)snprintf(d, sizeof(d), "stalled at %llu of %u frames after 20 s",
                   atomic_load(&rx.calls), n);
    e2e_emit(h, "clean_delivered", false, d);
  } else {
    e2e_case(h, "clean_delivered", true);
  }
  e2e_sleep_us(150000);

  hml_reasm_stats rs;
  hml_latency lat;
  uint64_t frames = 0, eof = 0;
  snapshot(&rx, &rs, &lat, &frames, &eof);
  e2e_eq_u(h, "clean_frames", frames, (uint64_t)n);
  e2e_eq_u(h, "clean_bytes", (uint64_t)atomic_load(&rx.bytes), (uint64_t)n * FRAME_BYTES);
  e2e_eq_u(h, "clean_eof", eof, (uint64_t)n);
  e2e_eq_u(h, "clean_frags_recv", rs.frags_recv, (uint64_t)n * FRAGS_PER_FRAME);
  e2e_eq_u(h, "clean_frames_ok", rs.frames_ok, (uint64_t)n);
  e2e_eq_u(h, "clean_frames_dropped", rs.frames_dropped, 0u);
  e2e_eq_u(h, "clean_gaps", rs.gaps, 0u);
  e2e_eq_u(h, "clean_crc_fail", rs.crc_fail, 0u);
  e2e_eq_u(h, "clean_proto_fail", rs.proto_fail, 0u);
  e2e_eq_u(h, "clean_bad_hash", (uint64_t)atomic_load(&rx.bad_hash), 0u);
  e2e_eq_u(h, "clean_bad_size", (uint64_t)atomic_load(&rx.bad_size), 0u);
  e2e_eq_u(h, "clean_rx_errors", (uint64_t)atomic_load(&rx.errors), 0u);
  e2e_eq_u(h, "clean_latency_samples", lat.n, (uint64_t)n);
  e2e_eq_u(h, "clean_latency_skew", lat.skew_samples, 0u);

  /* ---- phase reordered ---- */
  hml_reasm_stats rs_pre;
  uint64_t calls_pre = atomic_load(&rx.calls);
  snapshot(&rx, &rs_pre, NULL, NULL, NULL);

  reorder ro;
  memset(&ro, 0, sizeof(ro));
  memset(&x, 0, sizeof(x));
  x.ops = &g_reorder_ops;
  x.impl = &ro;
  st = g_reorder_ops.open(&x, &dest, STREAM_ID);
  e2e_eq_i(h, "reorder_transport_open", (long)st, (long)HML_OK);
  g_buffering = true;

  memset(&sc, 0, sizeof(sc));
  sc.stream_id = STREAM_ID;
  sc.max_frame_bytes = FRAME_BYTES;
  sc.seq_start = (uint32_t)(n * FRAGS_PER_FRAME);
  sc.mtu_effective = 1500u;
  s = NULL;
  st = hml_stream_open(&s, &sc, &x);
  e2e_eq_i(h, "reorder_stream_open", (long)st, (long)HML_OK);

  for (unsigned i = 0; i < reorder_frames; i++) {
    uint64_t fi = (uint64_t)n + i;
    hml_synth_pattern_fill(g_fbuf, FRAME_BYTES, fi * FRAME_BYTES, SEED);
    rx.want_hash[n + i] = hml_fnv1a32(g_fbuf, FRAME_BYTES);
    if (send_frame(s, fi) != HML_OK) {
      char d[96];
      (void)snprintf(d, sizeof(d), "reordered send_frame index=%u failed", i);
      e2e_emit(h, "reorder_send", false, d);
      send_rc = 1;
      break;
    }
    /* No flush_eof here: send_frame already placed the HML_FLAG_END fragment,
     * and the whole frame is still staged in the shim. A second END would be
     * an EOF-only frame, not part of this one. */
    /* each frame needs its reversed datagrams on the wire before the next
     * frame's fragments are staged, so the reversal is per frame */
    st = g_reorder_ops.flush(&x);
    if (st != HML_OK) {
      e2e_emit(h, "reorder_flush", false, "transport flush failed");
      send_rc = 1;
      break;
    }
    flow_credit();
  }
  g_buffering = false;
  e2e_eq_u(h, "reorder_frames_flushed", ro.frames, (uint64_t)reorder_frames);
  e2e_eq_u(h, "reorder_datagrams_flushed", ro.flushed,
           (uint64_t)reorder_frames * FRAGS_PER_FRAME);
  (void)hml_stream_close(s);

  if (!wait_calls(&rx, calls_pre + reorder_frames, 20000)) {
    char d[128];
    (void)snprintf(d, sizeof(d), "only %llu of %u reversed frames arrived",
                   atomic_load(&rx.calls) - calls_pre, reorder_frames);
    e2e_emit(h, "reorder_delivered", false, d);
  } else {
    e2e_case(h, "reorder_delivered", true);
  }
  e2e_sleep_us(150000);

  snapshot(&rx, &rs, NULL, NULL, NULL);
  e2e_eq_u(h, "reorder_frames_ok", rs.frames_ok - rs_pre.frames_ok, (uint64_t)reorder_frames);
  e2e_eq_u(h, "reorder_frames_dropped", rs.frames_dropped - rs_pre.frames_dropped, 0u);
  e2e_eq_u(h, "reorder_gaps", rs.gaps - rs_pre.gaps, 0u);
  e2e_eq_u(h, "reorder_crc_fail", rs.crc_fail - rs_pre.crc_fail, 0u);
  e2e_eq_u(h, "reorder_proto_fail", rs.proto_fail - rs_pre.proto_fail, 0u);
  e2e_eq_u(h, "reorder_bad_hash", (uint64_t)atomic_load(&rx.bad_hash), 0u);
  e2e_eq_u(h, "reorder_bad_size", (uint64_t)atomic_load(&rx.bad_size), 0u);
  e2e_eq_u(h, "reorder_out_of_range", (uint64_t)atomic_load(&rx.dup), 0u);

  /* ---- phase eof-only flush ---- */
  hml_reasm_stats rs_eof;
  snapshot(&rx, &rs_eof, NULL, NULL, NULL);
  uint64_t ok_pre = rs_eof.frames_ok;
  uint64_t calls_pre2 = atomic_load(&rx.calls);
  uint64_t zero_pre = atomic_load(&rx.zero_byte);

  memset(&x, 0, sizeof(x));
  x.ops = hml_udp_ops();
  st = x.ops->open(&x, &dest, STREAM_ID);
  e2e_eq_i(h, "eof_transport_open", (long)st, (long)HML_OK);
  memset(&sc, 0, sizeof(sc));
  sc.stream_id = STREAM_ID;
  sc.max_frame_bytes = FRAME_BYTES;
  sc.seq_start = (uint32_t)((n + reorder_frames) * FRAGS_PER_FRAME);
  sc.mtu_effective = 1500u;
  s = NULL;
  st = hml_stream_open(&s, &sc, &x);
  e2e_eq_i(h, "eof_stream_open", (long)st, (long)HML_OK);
  st = hml_stream_flush_eof(s, e2e_now_us());
  e2e_eq_i(h, "eof_only_sent", (long)st, (long)HML_OK);
  (void)hml_stream_close(s);

  if (wait_calls(&rx, calls_pre2 + 1u, 10000)) {
    e2e_case(h, "eof_only_zero_byte_callback", true);
  } else {
    e2e_emit(h, "eof_only_zero_byte_callback", false, "no callback after the EOF-only flush");
  }
  e2e_eq_u(h, "eof_only_zero_byte_count", (uint64_t)atomic_load(&rx.zero_byte) - zero_pre, 1u);
  snapshot(&rx, &rs, NULL, NULL, NULL);
  e2e_eq_u(h, "eof_only_frames_ok_unchanged", rs.frames_ok - ok_pre, 0u);
  e2e_eq_u(h, "eof_only_frames_dropped", rs.frames_dropped - rs_eof.frames_dropped, 0u);
  e2e_eq_u(h, "eof_only_gaps", rs.gaps - rs_eof.gaps, 0u);
  e2e_eq_u(h, "eof_only_crc_fail", rs.crc_fail - rs_eof.crc_fail, 0u);
  e2e_eq_u(h, "eof_only_bad_hash", (uint64_t)atomic_load(&rx.bad_hash), 0u);

  /* ---- teardown ---- */
  atomic_store(&rx.run, 0);
  (void)pthread_join(rxt, NULL);
  hml_host_close(rx.host);
  g_sensor.ops->close(&g_sensor);
  (void)pthread_mutex_destroy(&rx.lock);
  free(rx.want_hash);
  g_rx = NULL;

  return e2e_finish(h) == 0 ? (send_rc == 0 ? 0 : 1) : 1;
}
