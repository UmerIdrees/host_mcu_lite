/*
 * scene_roce.c -- T1a end-to-end over a real loopback socket.
 *
 * The transmitter is the shipping hml_roce_gen helper (host/roce_gen), i.e. the
 * exact code the hml_gen_roce CLI runs: it opens the real hml_rocev2 transport
 * from src/transports/rocev2 and pushes frames through hml_stream from
 * src/core. Nothing here crafts a packet, so BTH/RETH/ICRC construction and the
 * 20-byte end-of-frame metadata record stay in the transport.
 *
 * The receiver is the shipping hml_roce_demux (host/roce_demux): a real UDP
 * socket, an IP/UDP/BTH/RETH parse, ICRC verification, a simulated remote
 * memory window, and a frame delivered to the callback only when the metadata
 * record's CRC-32 over that window matches.
 *
 * Counters asserted, phase A (3 frames x 9000 B) -- the known-good pattern from
 * docs/testing.md section 5.5:
 *
 *   pkts=24  writes=21  bytes=27000  eof_frames=3
 *   icrc_err=0  psn_gaps=0  proto_err=0  meta_drops=0
 *
 * 9000 B at HML_MAX_FRAG_PAYLOAD (1428) is exactly 7 fragments -- 6 x 1428 plus
 * a 432 B tail -- so 7 data writes plus 1 end-of-frame metadata write is 8
 * datagrams per frame: 24 pkts and 21 writes over 3 frames. The identity
 *
 *   pkts == writes + eof_frames + meta_drops + icrc_err + proto_err
 *
 * is asserted afterwards, which makes the loss phase an accounting check rather
 * than a smoke test: every datagram the receiver saw must land in exactly one
 * bucket. (The demux has exactly one unclassified path, the duplicate-frame_seq
 * suppression in rx_meta(), and it cannot trigger here because no frame is ever
 * transmitted twice.)
 *
 * Then 60 frames with 1 datagram in 12 withheld by the transport's own fault
 * hook, and finally one clean frame that must still be delivered, proving one
 * damaged datagram costs at most one frame and the receiver recovers.
 *
 * NOT COVERED: no NIC, no verbs, no libibverbs, no capture, no real MTU. Both
 * ends of this test are this repository's own code, so their agreement proves
 * self-consistency, not interoperability. docs/testing.md sections 7.5 and 8
 * list what would have to come first.
 */

#include "e2e_cpu.h"

#include "hml/hml_crc.h"
#include "hml/hml_port.h"
#include "hml/hml_stream.h"
#include "hml/hml_transport.h"
#include "hml_roce_demux.h"
#include "hml_roce_gen.h"
#include "hml_roce_priv.h"
#include "hml_synth_priv.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_SIZE 9000u
#define FRAMES 3u
#define LOSSY_FRAMES 60u
#define FIRST_LOSSY_FRAME FRAMES
#define FIRST_RECOVERY_FRAME (FIRST_LOSSY_FRAME + LOSSY_FRAMES)
#define TOTAL_FRAMES (FIRST_RECOVERY_FRAME + 1u)
#define WIN_BASE 0x100000ull
#define WIN_LEN 65536u
#define WIN_PAGES 16u
#define RKEY 0xDEADBEEFu
#define LOOPBACK 0x7F000001u
#define SEED 1u
/* The transport chunks a frame exactly like hml_roce_gen_frame_send() does, so
 * the receiver can recover the frame index from the metadata frame_seq, which
 * the transport sets to the seq of the last (HML_FLAG_END) fragment. */
#define FRAGS_PER_FRAME ((FRAME_SIZE + HML_MAX_FRAG_PAYLOAD - 1u) / HML_MAX_FRAG_PAYLOAD)
/* Datagrams the sender may put on the wire beyond what the demux has drained. */
#define INFLIGHT 48u
/* Drop every Nth datagram during the loss phase. */
#define LOSS_EVERY 12u

typedef struct rx {
  hml_roce_rx_t *rx;
  pthread_t th;
  atomic_int stop;
  atomic_ullong delivered;
  atomic_ullong bytes;
  atomic_ullong bad_seq;
  atomic_ullong bad_len;
  atomic_ullong bad_crc;
  atomic_ullong bad_content;
  atomic_ullong drained; /* datagrams consumed by the demux */
  e2e_harness *h;
} rx;

/*
 * The delivered frame must be byte-for-byte the frame the sender built, and the
 * only thing that identifies which one it is, without any side channel, is the
 * frame_seq the transport put in the metadata record. The content pattern is a
 * function of the absolute pattern offset frame_index * FRAME_SIZE, so the
 * receiver rebuilds the same bytes and compares. bad_seq counts a frame_seq
 * whose low bits are not the expected last-fragment seq, which would mean the
 * FRAGS_PER_FRAME assumption stopped holding.
 */
static void on_eof(void *vctx, uint32_t frame_seq, const uint8_t *frame, size_t n, uint32_t crc32) {
  rx *r = (rx *)vctx;
  (void)atomic_fetch_add(&r->delivered, 1ull);
  (void)atomic_fetch_add(&r->bytes, (unsigned long long)n);
  if (n != (size_t)FRAME_SIZE) {
    (void)atomic_fetch_add(&r->bad_len, 1ull);
    return;
  }
  if (frame_seq % FRAGS_PER_FRAME != FRAGS_PER_FRAME - 1u) {
    (void)atomic_fetch_add(&r->bad_seq, 1ull);
    return;
  }
  if (crc32 != hml_roce_crc32_done(hml_roce_crc32_update(HML_ROCE_CRC32_INIT, frame, n))) {
    (void)atomic_fetch_add(&r->bad_crc, 1ull);
    return;
  }
  const uint64_t base = (uint64_t)(frame_seq / FRAGS_PER_FRAME) * FRAME_SIZE;
  for (size_t i = 0; i < n; i++) {
    if (frame[i] != hml_synth_pattern_byte(base + i, SEED)) {
      (void)atomic_fetch_add(&r->bad_content, 1ull);
      return;
    }
  }
}

static void rx_pump(rx *r, int timeout_ms) {
  hml_roce_stats st;
  (void)hml_roce_rx_poll(r->rx, timeout_ms);
  hml_roce_rx_stats(r->rx, &st); /* the rx thread is the only stats writer */
  atomic_store(&r->drained, (unsigned long long)st.pkts);
}

static void *rx_main(void *arg) {
  rx *r = (rx *)arg;
  while (!atomic_load(&r->stop)) rx_pump(r, 50);
  for (int i = 0; i < 60; i++) rx_pump(r, 10); /* final drain */
  return NULL;
}

static void rx_start(rx *r) {
  atomic_store(&r->stop, 0);
  if (pthread_create(&r->th, NULL, rx_main, r) != 0)
    e2e_emit(r->h, "receiver_thread", false, "pthread_create failed");
}

static void rx_stop(rx *r) {
  atomic_store(&r->stop, 1);
  (void)pthread_join(r->th, NULL);
}

/* ---- fault hook: 1-in-N datagram loss ------------------------------------ */

typedef struct dropstate {
  unsigned every;
  unsigned seen;
  unsigned dropped;
  unsigned emitted;
} dropstate;

static dropstate g_drop;

static bool drop_hook(void *ctx, unsigned idx, uint8_t *dg, size_t *len) {
  dropstate *d = (dropstate *)ctx;
  (void)idx;
  (void)dg;
  (void)len;
  d->emitted++;
  if (d->every == 0u) return false;
  d->seen++;
  if ((d->seen % d->every) != 0u) return false;
  d->dropped++;
  return true;
}

static rx *g_rx;

static unsigned emitted_on_wire(void) { return g_drop.emitted - g_drop.dropped; }

static void flow(void) {
  int budget = e2e_budget_ms();
  uint64_t give_up = e2e_now_us() + (uint64_t)(budget > 0 ? budget : 5000) * 1000ull;
  while (atomic_load(&g_rx->drained) + INFLIGHT < emitted_on_wire()) {
    if (e2e_budget_ms() == -1 || e2e_now_us() >= give_up) return;
    e2e_sleep_us(50);
  }
}

static hml_status tx_frame(hml_stream_t *s, uint8_t *buf, uint64_t frame_index) {
  hml_roce_gen_frame_fill(buf, (size_t)FRAME_SIZE, frame_index, SEED);
  hml_status st = hml_roce_gen_frame_send(s, buf, (size_t)FRAME_SIZE, NULL);
  flow();
  return st;
}

static bool wait_delivered(rx *r, unsigned long long target, int ms) {
  uint64_t deadline = e2e_now_us() + (uint64_t)(unsigned)ms * 1000ull;
  while (atomic_load(&r->delivered) < target) {
    if (e2e_budget_ms() == -1) return false;
    if (e2e_now_us() >= deadline) return false;
    e2e_sleep_us(1000);
  }
  return true;
}

static void wait_quiet(rx *r, int quiet_ms, int max_ms) {
  unsigned long long last = atomic_load(&r->delivered);
  uint64_t quiet_until = e2e_now_us() + (uint64_t)(unsigned)quiet_ms * 1000ull;
  const uint64_t hard = e2e_now_us() + (uint64_t)(unsigned)max_ms * 1000ull;
  while (e2e_now_us() < hard) {
    e2e_sleep_us(2000);
    if (e2e_budget_ms() == -1) return;
    unsigned long long now = atomic_load(&r->delivered);
    if (now != last) {
      last = now;
      quiet_until = e2e_now_us() + (uint64_t)(unsigned)quiet_ms * 1000ull;
    } else if (e2e_now_us() >= quiet_until) {
      return;
    }
  }
}

/* ---- scene --------------------------------------------------------------- */

int e2e_scene_roce(e2e_harness *h) {
  uint16_t port = e2e_port_pick();
  if (port == 0u) {
    e2e_emit(h, "port_available", false, "no free port in the private loopback range");
    return 1;
  }
  e2e_eq_u(h, "port_bound", port, port);

  rx r;
  memset(&r, 0, sizeof(r));
  r.h = h;
  uint8_t *frame = (uint8_t *)malloc(FRAME_SIZE);
  if (frame == NULL) {
    e2e_emit(h, "alloc", false, "out of memory");
    return 1;
  }

  hml_roce_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.udp_port = port;
  cfg.window_base = WIN_BASE;
  cfg.window_len_bytes = WIN_LEN;
  cfg.frame_size = FRAME_SIZE;
  cfg.rkey = RKEY;
  cfg.verify_icrc = true;

  hml_status st = hml_roce_rx_open(&r.rx, &cfg, on_eof, &r);
  if (st != HML_OK || r.rx == NULL) {
    char d[96];
    (void)snprintf(d, sizeof(d), "hml_roce_rx_open status=%d port=%u", (int)st, (unsigned)port);
    e2e_emit(h, "receiver_open", false, d);
    free(frame);
    return 1;
  }
  e2e_case(h, "receiver_open", true);

  hml_roce_gen_xport g;
  memset(&g, 0, sizeof(g));

  hml_dest_cfg dest;
  memset(&dest, 0, sizeof(dest));
  dest.kind = HML_XPORT_ROCEV2;
  dest.ipv4 = LOOPBACK;
  dest.udp_port = port;
  dest.qp = 1u;
  dest.psn = 0x00200001u;
  dest.rkey = RKEY;
  dest.remote_addr = WIN_BASE;
  dest.frame_size = FRAME_SIZE;
  dest.window_len = WIN_PAGES;
  dest.hop_limit = 64u;

  /* allow_fallback = false: a degraded T0 transport must be a failure here,
   * because a T1a receiver would reject T0 datagrams anyway and a green run
   * that quietly sent T0 would prove nothing. */
  st = hml_roce_gen_xport_open(&g, &dest, 1u, 0u, false);
  e2e_eq_i(h, "transmitter_open", (long)st, (long)HML_OK);
  e2e_case(h, "transmitter_is_rocev2", g.ops == hml_rocev2_ops());
  e2e_case(h, "transmitter_did_not_fall_back", !g.fallback);
  if (st != HML_OK || g.ops == NULL) {
    hml_roce_gen_xport_close(&g);
    hml_roce_rx_close(r.rx);
    free(frame);
    return 1;
  }

  hml_stream_t *s = NULL;
  hml_stream_cfg sc;
  memset(&sc, 0, sizeof(sc));
  sc.stream_id = 1u;
  sc.max_frame_bytes = FRAME_SIZE;
  sc.seq_start = 0;
  sc.mtu_effective = 1500u;
  st = hml_stream_open(&s, &sc, &g.xport);
  e2e_eq_i(h, "stream_open", (long)st, (long)HML_OK);

  memset(&g_drop, 0, sizeof(g_drop));
  hml_roce_set_fault_hook(drop_hook, &g_drop); /* installed for the whole run */
  g_rx = &r;
  rx_start(&r);

  /* ---- phase A: the known-good 3 x 9000 pattern ---- */
  int tx_rc = 0;
  for (uint64_t i = 0; i < FRAMES; i++) {
    if (e2e_budget_ms() == -1) {
      e2e_emit(h, "clean_tx", false, "deadline hit before the clean phase finished");
      tx_rc = 1;
      break;
    }
    if (tx_frame(s, frame, i) != HML_OK) {
      char d[96];
      (void)snprintf(d, sizeof(d), "tx_frame index=%llu failed", (unsigned long long)i);
      e2e_emit(h, "clean_tx", false, d);
      tx_rc = 1;
      break;
    }
  }
  e2e_eq_i(h, "clean_flush", (long)g.ops->flush(&g.xport), (long)HML_OK);
  e2e_case(h, "clean_delivered", wait_delivered(&r, FRAMES, 20000));
  rx_stop(&r);

  hml_roce_stats st1;
  hml_roce_rx_stats(r.rx, &st1);
  char d[192];
  (void)snprintf(d, sizeof(d),
                 "pkts=%llu writes=%llu bytes=%llu eof_frames=%llu icrc_err=%llu psn_gaps=%llu",
                 (unsigned long long)st1.pkts, (unsigned long long)st1.writes,
                 (unsigned long long)st1.bytes, (unsigned long long)st1.eof_frames,
                 (unsigned long long)st1.icrc_err, (unsigned long long)st1.psn_gaps);
  e2e_emit(h, "clean_counters_exact",
           st1.pkts == 24u && st1.writes == 21u && st1.bytes == 27000u &&
               st1.eof_frames == FRAMES && st1.icrc_err == 0u && st1.psn_gaps == 0u,
           d);
  e2e_eq_u(h, "clean_pkts", st1.pkts, 24u);
  e2e_eq_u(h, "clean_writes", st1.writes, 21u);
  e2e_eq_u(h, "clean_bytes", st1.bytes, 27000u);
  e2e_eq_u(h, "clean_eof_frames", st1.eof_frames, FRAMES);
  e2e_eq_u(h, "clean_icrc_err", st1.icrc_err, 0u);
  e2e_eq_u(h, "clean_psn_gaps", st1.psn_gaps, 0u);
  e2e_eq_u(h, "clean_proto_err", st1.proto_err, 0u);
  e2e_eq_u(h, "clean_meta_drops", st1.meta_drops, 0u);
  e2e_eq_u(h, "clean_callbacks", (uint64_t)atomic_load(&r.delivered), FRAMES);
  e2e_eq_u(h, "clean_callback_bytes", (uint64_t)atomic_load(&r.bytes),
           (uint64_t)FRAMES * FRAME_SIZE);
  e2e_eq_u(h, "clean_bad_seq", (uint64_t)atomic_load(&r.bad_seq), 0u);
  e2e_eq_u(h, "clean_bad_len", (uint64_t)atomic_load(&r.bad_len), 0u);
  e2e_eq_u(h, "clean_bad_crc", (uint64_t)atomic_load(&r.bad_crc), 0u);
  e2e_eq_u(h, "clean_bad_content", (uint64_t)atomic_load(&r.bad_content), 0u);
  e2e_eq_u(h, "clean_sender_datagrams", (uint64_t)g_drop.emitted,
           (uint64_t)FRAMES * (FRAGS_PER_FRAME + 1u));
  e2e_eq_u(h, "clean_partition", st1.pkts,
           st1.writes + st1.eof_frames + st1.meta_drops + st1.icrc_err + st1.proto_err);

  /* ---- phase B: 1 datagram in LOSS_EVERY withheld ---- */
  rx_start(&r);
  g_drop.every = LOSS_EVERY;
  const uint64_t base = atomic_load(&r.delivered);
  unsigned sent_frames = 0u;
  for (uint64_t i = 0; i < LOSSY_FRAMES; i++) {
    if (e2e_budget_ms() == -1) break;
    if (tx_frame(s, frame, FIRST_LOSSY_FRAME + i) != HML_OK) break;
    sent_frames++;
  }
  e2e_eq_i(h, "lossy_flush", (long)g.ops->flush(&g.xport), (long)HML_OK);
  wait_quiet(&r, 500, 8000);
  const uint64_t lossy = atomic_load(&r.delivered) - base;
  const uint64_t lost_frames = (uint64_t)sent_frames - lossy;
  g_drop.every = 0u;

  hml_roce_stats mid;
  hml_roce_rx_stats(r.rx, &mid);
  (void)snprintf(d, sizeof(d),
                 "withheld=%llu sent=%u delivered=%llu lost=%llu meta_drops=%llu psn_gaps=%llu",
                 (unsigned long long)g_drop.dropped, sent_frames, (unsigned long long)lossy,
                 (unsigned long long)lost_frames, (unsigned long long)(mid.meta_drops - st1.meta_drops),
                 (unsigned long long)(mid.psn_gaps - st1.psn_gaps));
  e2e_emit(h, "loss_accounting", g_drop.dropped > 0u && lost_frames > 0u &&
                                      lost_frames <= g_drop.dropped,
         d);
  e2e_case(h, "loss_something_survived", lossy > 0u);
  e2e_case(h, "loss_detected_as_meta_drops", mid.meta_drops > st1.meta_drops);
  e2e_case(h, "loss_detected_as_psn_gaps", mid.psn_gaps > st1.psn_gaps);
  e2e_eq_u(h, "loss_no_bad_crc", (uint64_t)atomic_load(&r.bad_crc), 0u);
  e2e_eq_u(h, "loss_no_bad_content", (uint64_t)atomic_load(&r.bad_content), 0u);
  e2e_eq_u(h, "loss_no_icrc_err", mid.icrc_err, st1.icrc_err);
  e2e_eq_u(h, "loss_no_proto_err", mid.proto_err, st1.proto_err);

  /* ---- recovery: the very next clean frame must be delivered ---- */
  const uint64_t target = base + lossy + 1u;
  e2e_eq_i(h, "recovery_tx", (long)tx_frame(s, frame, FIRST_RECOVERY_FRAME), (long)HML_OK);
  e2e_eq_i(h, "recovery_flush", (long)g.ops->flush(&g.xport), (long)HML_OK);
  e2e_case(h, "recovery_frame_delivered", wait_delivered(&r, target, 10000));
  rx_stop(&r);

  hml_roce_stats st2;
  hml_roce_rx_stats(r.rx, &st2);
  (void)snprintf(d, sizeof(d),
                 "pkts=%llu writes=%llu bytes=%llu eof_frames=%llu psn_gaps=%llu meta_drops=%llu "
                 "icrc_err=%llu proto_err=%llu",
                 (unsigned long long)st2.pkts, (unsigned long long)st2.writes,
                 (unsigned long long)st2.bytes, (unsigned long long)st2.eof_frames,
                 (unsigned long long)st2.psn_gaps, (unsigned long long)st2.meta_drops,
                 (unsigned long long)st2.icrc_err, (unsigned long long)st2.proto_err);
  e2e_emit(h, "final_partition",
           st2.pkts ==
               st2.writes + st2.eof_frames + st2.meta_drops + st2.icrc_err + st2.proto_err,
           d);
  e2e_eq_u(h, "final_icrc_err", st2.icrc_err, 0u);
  e2e_eq_u(h, "final_proto_err", st2.proto_err, 0u);
  e2e_eq_u(h, "final_eof_frames", st2.eof_frames, (uint64_t)atomic_load(&r.delivered));
  e2e_eq_u(h, "final_bad_seq", (uint64_t)atomic_load(&r.bad_seq), 0u);
  e2e_eq_u(h, "final_bad_len", (uint64_t)atomic_load(&r.bad_len), 0u);
  e2e_eq_u(h, "final_bad_crc", (uint64_t)atomic_load(&r.bad_crc), 0u);
  e2e_eq_u(h, "final_bad_content", (uint64_t)atomic_load(&r.bad_content), 0u);

  hml_roce_set_fault_hook(NULL, NULL);
  (void)hml_stream_close(s);
  hml_roce_gen_xport_close(&g);
  hml_roce_rx_close(r.rx);
  free(frame);
  g_rx = NULL;

  return e2e_finish(h) == 0 ? (tx_rc == 0 ? 0 : 1) : 1;
}
