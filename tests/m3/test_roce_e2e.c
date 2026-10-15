/*
 * M3 / T1a -- end-to-end: hml_rocev2 transmit path + hml_roce_demux receive
 * path in ONE process over loopback UDP/14791.
 *
 *   phase A  500 frames x 9000 bytes, clean            -> 500 delivered frames
 *   phase B  100 frames with 2% datagram loss          -> no hang, some frames lost,
 *                                                        the next clean frame recovers
 *   phase C  misconfigured emitter (bad rkey / bad VA) -> proto_err++, no delivery,
 *                                                        the next clean frame recovers
 *
 * Each 9000-byte frame is streamed as 3 logical pieces of 3000 bytes, each piece
 * as 3 wire fragments (1440 + 1440 + 120) -- 9 RDMA WRITEs plus 1 EOF metadata
 * write per frame.
 */

#include "hml_t_m3.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "hml/hml_crc.h"
#include "hml/hml_port.h"
#include "hml/hml_protocol.h"
#include "hml/hml_transport.h"
#include "hml_roce_demux.h"
#include "hml_roce_priv.h"

#define PORT HML_ROCE_TEST_PORT /* 14791 */
#define WIN_BASE 0x100000ull
#define WIN_LEN 65536u
#define FRAME_SIZE 9000u
#define RKEY 0xDEADBEEFu
#define LOOPBACK 0x7F000001u
#define PIECE 3000u
#define PIECES_PER_FRAME 3u
#define FRAGS_PER_FRAME 9u /* 3 pieces x 3 fragments */
#define DG_PER_FRAME 10u   /* +1 EOF metadata write */
#define N_FRAMES 500u
#define N_LOSSY 100u
#define EXPECT_BYTES ((uint64_t)N_FRAMES * FRAME_SIZE)

/* ---- fault injection ---------------------------------------------------- */

#define FAULT_OFF 0u
#define FAULT_DROP 1u
#define FAULT_BAD_HEADER 2u

typedef struct fault_state {
  unsigned mode;       /* FAULT_* */
  unsigned drop_every; /* FAULT_DROP: drop every Nth datagram (50 == 2%) */
  unsigned seen;       /* datagrams handled since the mode was armed */
  unsigned last_idx;   /* absolute index of the last datagram this hook saw */
  unsigned emitted;    /* datagrams offered to the socket layer */
  unsigned dropped;    /* datagrams withheld from the socket layer */
  unsigned t1, t2;     /* FAULT_BAD_HEADER: absolute indices to corrupt */
} fault_state;

static fault_state g_fault;

static void fault_arm(fault_state *f, unsigned mode) {
  f->mode = mode;
  f->seen = 0;
}

static bool fault_hook(void *ctx, unsigned idx, uint8_t *dg, size_t *len) {
  fault_state *f = (fault_state *)ctx;
  f->last_idx = idx;
  f->emitted++;
  if (f->mode == FAULT_OFF) return false;
  f->seen++;
  if (f->mode == FAULT_DROP) {
    if ((f->seen % f->drop_every) != 0) return false;
    f->dropped++;
    return true;
  }
  if (idx == f->t1) {
    hml_put_be32(dg + HML_ROCE_OFF_RETH + 8, 0x12345678u); /* wrong remote key */
  } else if (idx == f->t2) {
    hml_put_be64(dg + HML_ROCE_OFF_RETH, 0x0000DEAD00000000ull); /* VA outside the window */
  } else {
    return false;
  }
  /* The RETH lives inside the ICRC coverage, so refresh it: the receiver has to
   * notice a protocol error, not a link error. */
  uint32_t ic = hml_crc32_ieee(dg + HML_ROCE_OFF_BTH, *len - 4u - HML_ROCE_OFF_BTH);
  dg[*len - 4] = (uint8_t)ic;
  dg[*len - 3] = (uint8_t)(ic >> 8);
  dg[*len - 2] = (uint8_t)(ic >> 16);
  dg[*len - 1] = (uint8_t)(ic >> 24);
  return false;
}

/* usleep() is not in POSIX.1-2008; nanosleep() is. */
static void sleep_us(unsigned us) {
  struct timespec ts;
  ts.tv_sec = (time_t)(us / 1000000u);
  ts.tv_nsec = (long)(us % 1000000u) * 1000L;
  (void)nanosleep(&ts, NULL);
}

/* ---- receive thread ----------------------------------------------------- */

typedef struct rxrt {
  hml_roce_rx_t *rx;
  pthread_t th;
  atomic_int stop;
  atomic_uint_least64_t delivered;
  atomic_uint_least64_t bytes;
  atomic_uint_least64_t bad_len;
  atomic_uint_least64_t bad_crc;
  atomic_uint_least64_t bad_content;
  atomic_uint_least64_t drained; /* datagrams consumed by the demux */
  uint32_t frame_size;
} rxrt;

static void on_eof(void *vctx, uint32_t frame_seq, const uint8_t *frame, size_t n, uint32_t crc32) {
  rxrt *s = (rxrt *)vctx;
  atomic_fetch_add(&s->delivered, 1);
  atomic_fetch_add(&s->bytes, (uint64_t)n);
  if (n != s->frame_size) {
    atomic_fetch_add(&s->bad_len, 1);
    return;
  }
  if (crc32 != hml_roce_crc32_done(hml_roce_crc32_update(HML_ROCE_CRC32_INIT, frame, n))) {
    atomic_fetch_add(&s->bad_crc, 1);
    return;
  }
  for (size_t i = 0; i < n; i++) {
    if (frame[i] != (uint8_t)(frame_seq + i)) {
      atomic_fetch_add(&s->bad_content, 1);
      return;
    }
  }
}

static void rx_pump(rxrt *s, int timeout_ms) {
  hml_roce_stats st;
  (void)hml_roce_rx_poll(s->rx, timeout_ms);
  hml_roce_rx_stats(s->rx, &st); /* safe: this thread is the only stats writer */
  atomic_store(&s->drained, st.pkts);
}

static void *rx_main(void *arg) {
  rxrt *s = (rxrt *)arg;
  while (!atomic_load(&s->stop)) rx_pump(s, 50);
  for (int i = 0; i < 40; i++) rx_pump(s, 10); /* final drain */
  return NULL;
}

static void rx_start(rxrt *s) {
  atomic_store(&s->stop, 0);
  T_CHECK(pthread_create(&s->th, NULL, rx_main, s) == 0, "pthread_create failed");
}

static void rx_stop(rxrt *s) {
  atomic_store(&s->stop, 1);
  T_CHECK(pthread_join(s->th, NULL) == 0, "pthread_join failed");
}

static void wait_delivered(rxrt *s, uint64_t target, int ms) {
  uint64_t deadline = hml_clock_us() + (uint64_t)ms * 1000ull;
  while (atomic_load(&s->delivered) < target) {
    if (hml_clock_us() >= deadline) return;
    sleep_us(1000);
  }
}

static void wait_quiet(rxrt *s, int quiet_ms, int max_ms) {
  uint64_t last = atomic_load(&s->delivered);
  uint64_t quiet_until = hml_clock_us() + (uint64_t)quiet_ms * 1000ull;
  const uint64_t hard = hml_clock_us() + (uint64_t)max_ms * 1000ull;
  while (hml_clock_us() < hard) {
    sleep_us(2000);
    uint64_t now = atomic_load(&s->delivered);
    if (now != last) {
      last = now;
      quiet_until = hml_clock_us() + (uint64_t)quiet_ms * 1000ull;
    } else if (hml_clock_us() >= quiet_until) {
      return;
    }
  }
}

/* ---- transmit ----------------------------------------------------------- */

/*
 * Flow control. The kernel receive queue holds only ~90 of these datagrams and
 * the emitter is marginally slower per datagram than the receiver, so an
 * unpaced burst eventually overflows it and silently loses frames. The receiver
 * thread publishes the datagram count it has drained into an atomic -- it is the
 * only writer of the demux stats, so reading them there is race-free, and the
 * handoff to this thread is an atomic store/load. Keeping at most INFLIGHT
 * datagrams outstanding removes all timing heuristics from the test.
 */
#define INFLIGHT 48u

static rxrt *g_rx = NULL;

static unsigned on_wire(void) { return g_fault.emitted - g_fault.dropped; }

static void flow(void) {
  if (!g_rx) return;
  const uint64_t deadline = hml_clock_us() + 2000000ull;
  unsigned spins = 0;
  while (atomic_load(&g_rx->drained) + INFLIGHT < on_wire()) {
    if (hml_clock_us() >= deadline) return; /* never deadlock the test */
    if ((++spins & 0x3FFu) == 0) sleep_us(50);
  }
}

static hml_status tx_one(const hml_xport_ops *ops, hml_xport_t *x, const hml_frame_hdr *h,
                         const uint8_t *p, size_t n) {
  for (int i = 0; i < 2000; i++) {
    hml_status st = ops->send_frag(x, h, p, n);
    if (st != HML_ERR_AGAIN) return st;
    sleep_us(200);
  }
  return HML_ERR_TIMEOUT;
}

static hml_status tx_frame(const hml_xport_ops *ops, hml_xport_t *x, uint32_t seq,
                           uint8_t *frame_buf) {
  for (size_t i = 0; i < FRAME_SIZE; i++) frame_buf[i] = (uint8_t)(seq + i);

  size_t off = 0;
  uint16_t idx = 0;
  for (unsigned piece = 0; piece < PIECES_PER_FRAME; piece++) {
    size_t left = PIECE;
    while (left) {
      size_t take = left > HML_ROCE_MAX_PAYLOAD ? HML_ROCE_MAX_PAYLOAD : left;
      hml_frame_hdr h;
      memset(&h, 0, sizeof(h));
      h.stream_id = 1;
      h.seq = seq;
      h.frag_idx = idx;
      h.frag_cnt = FRAGS_PER_FRAME;
      h.ts_us = hml_clock_us();
      h.payload_len = (uint16_t)take;
      h.flags = 0;
      if (idx == 0) h.flags |= HML_FLAG_START;
      if (idx == FRAGS_PER_FRAME - 1) h.flags |= HML_FLAG_END;
      hml_status st = tx_one(ops, x, &h, frame_buf + off, take);
      if (st != HML_OK) return st;
      off += take;
      left -= take;
      idx++;
      flow();
    }
  }
  return HML_OK;
}

int main(void) {
  uint8_t frame_buf[FRAME_SIZE];

  rxrt s;
  memset(&s, 0, sizeof(s));
  s.frame_size = FRAME_SIZE;

  hml_roce_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.udp_port = PORT;
  cfg.window_base = WIN_BASE;
  cfg.window_len_bytes = WIN_LEN;
  cfg.frame_size = FRAME_SIZE;
  cfg.rkey = RKEY;
  cfg.verify_icrc = true;

  T_CHECK(hml_roce_rx_open(&s.rx, &cfg, on_eof, &s) == HML_OK, "hml_roce_rx_open failed");
  if (!s.rx) T_DONE("test_roce_e2e");

  hml_dest_cfg dest;
  memset(&dest, 0, sizeof(dest));
  dest.kind = HML_XPORT_ROCEV2;
  dest.ipv4 = LOOPBACK;
  dest.udp_port = PORT;
  dest.qp = 1;
  dest.psn = 0x00200001u;
  dest.rkey = RKEY;
  dest.remote_addr = WIN_BASE;
  dest.frame_size = FRAME_SIZE;
  dest.window_len = 16;
  dest.hop_limit = 64;

  const hml_xport_ops *ops = hml_rocev2_ops();
  hml_xport_t x;
  x.ops = ops;
  x.impl = NULL;
  T_CHECK(ops->open(&x, &dest, 1) == HML_OK, "rocev2 open failed");
  if (!x.impl) T_DONE("test_roce_e2e");

  memset(&g_fault, 0, sizeof(g_fault));
  g_fault.mode = FAULT_OFF;
  hml_roce_set_fault_hook(fault_hook, &g_fault); /* installed for the whole run */
  g_rx = &s;

  rx_start(&s);

  /* ---- phase A: 500 clean frames ---- */
  const uint64_t t0 = hml_clock_us();
  for (uint32_t i = 0; i < N_FRAMES; i++) {
    hml_status st = tx_frame(ops, &x, i, frame_buf);
    if (st != HML_OK) {
      T_CHECK(0, "tx_frame %u failed: %d", i, (int)st);
      break;
    }
  }
  T_CHECK(ops->flush(&x) == HML_OK, "flush failed");
  wait_delivered(&s, N_FRAMES, 20000);
  rx_stop(&s);
  double phase_a_s = (double)(hml_clock_us() - t0) / 1e6;

  hml_roce_stats st;
  hml_roce_rx_stats(s.rx, &st);
  printf("  phase A: %.2f s  eof_frames=%llu writes=%llu bytes=%llu icrc_err=%llu\n", phase_a_s,
         (unsigned long long)st.eof_frames, (unsigned long long)st.writes,
         (unsigned long long)st.bytes, (unsigned long long)st.icrc_err);
  T_CHECK(st.eof_frames == N_FRAMES, "phase A eof_frames = %llu, want %u",
          (unsigned long long)st.eof_frames, N_FRAMES);
  T_CHECK(st.bytes == EXPECT_BYTES, "phase A bytes = %llu, want %llu",
          (unsigned long long)st.bytes, (unsigned long long)EXPECT_BYTES);
  T_CHECK(st.icrc_err == 0, "phase A icrc_err = %llu, want 0", (unsigned long long)st.icrc_err);
  T_CHECK(st.writes == (uint64_t)N_FRAMES * FRAGS_PER_FRAME, "phase A writes = %llu, want %llu",
          (unsigned long long)st.writes,
          (unsigned long long)((uint64_t)N_FRAMES * FRAGS_PER_FRAME));
  T_CHECK(atomic_load(&s.delivered) == N_FRAMES, "phase A callback count = %llu, want %u",
          (unsigned long long)atomic_load(&s.delivered), N_FRAMES);
  T_CHECK(atomic_load(&s.bad_len) == 0, "phase A frames with wrong length: %llu",
          (unsigned long long)atomic_load(&s.bad_len));
  T_CHECK(atomic_load(&s.bad_crc) == 0, "phase A frames with bad CRC: %llu",
          (unsigned long long)atomic_load(&s.bad_crc));
  T_CHECK(atomic_load(&s.bad_content) == 0, "phase A frames with wrong content: %llu",
          (unsigned long long)atomic_load(&s.bad_content));

  rx_start(&s);

  /* ---- phase B: 2% datagram loss ---- */
  g_fault.drop_every = 50;
  fault_arm(&g_fault, FAULT_DROP);
  uint64_t base = atomic_load(&s.delivered);
  for (uint32_t i = 0; i < N_LOSSY; i++) {
    if (tx_frame(ops, &x, 1000 + i, frame_buf) != HML_OK) break;
  }
  ops->flush(&x);
  wait_quiet(&s, 500, 6000); /* must not hang */
  uint64_t lossy = atomic_load(&s.delivered) - base;
  fault_arm(&g_fault, FAULT_OFF);
  printf("  phase B: %llu of %u frames survived 2%% loss\n", (unsigned long long)lossy, N_LOSSY);
  T_CHECK(lossy < N_LOSSY, "phase B delivered %llu of %u frames: loss was not injected",
          (unsigned long long)lossy, N_LOSSY);
  T_CHECK(lossy > 0, "phase B delivered nothing at all");

  /* recovery: the very next clean frame must still be delivered */
  wait_delivered(&s, base + lossy + 1, 5000);
  T_CHECK(tx_frame(ops, &x, 5000, frame_buf) == HML_OK, "recovery frame tx failed");
  wait_delivered(&s, base + lossy + 1, 5000);
  T_CHECK(atomic_load(&s.delivered) == base + lossy + 1,
          "phase B recovery: delivered = %llu, want %llu",
          (unsigned long long)atomic_load(&s.delivered),
          (unsigned long long)(base + lossy + 1));

  /* ---- phase C: misconfigured emitter (bad rkey / VA outside the window) ---- */
  g_fault.t1 = g_fault.last_idx + 2; /* fragment 1 of the next frame */
  g_fault.t2 = g_fault.last_idx + 5; /* fragment 4 of the next frame */
  fault_arm(&g_fault, FAULT_BAD_HEADER);
  base = atomic_load(&s.delivered);
  hml_status mc = tx_frame(ops, &x, 6000, frame_buf);
  ops->flush(&x);
  fault_arm(&g_fault, FAULT_OFF);
  wait_quiet(&s, 500, 4000);
  T_CHECK(mc == HML_OK, "phase C tx failed: %d", (int)mc);
  T_CHECK(atomic_load(&s.delivered) == base,
          "phase C: misconfigured frame was delivered (must be dropped)");

  T_CHECK(tx_frame(ops, &x, 6001, frame_buf) == HML_OK, "phase C recovery frame tx failed");
  wait_delivered(&s, base + 1, 5000);
  T_CHECK(atomic_load(&s.delivered) == base + 1,
          "phase C: clean frame after misconfiguration was not delivered");

  rx_stop(&s);
  hml_roce_rx_stats(s.rx, &st);
  printf(
      "  phase C: proto_err=%llu psn_gaps=%llu meta_drops=%llu\n",
      (unsigned long long)st.proto_err, (unsigned long long)st.psn_gaps,
      (unsigned long long)st.meta_drops);
  T_CHECK(st.proto_err >= 2, "phase C proto_err = %llu, want >= 2",
          (unsigned long long)st.proto_err);
  T_CHECK(st.meta_drops >= 1, "phase C meta_drops = %llu, want >= 1",
          (unsigned long long)st.meta_drops);
  T_CHECK(st.icrc_err == 0, "icrc_err = %llu, want 0 over the whole run",
          (unsigned long long)st.icrc_err);
  T_CHECK(atomic_load(&s.bad_content) == 0, "%llu delivered frames had wrong content",
          (unsigned long long)atomic_load(&s.bad_content));
  T_CHECK(atomic_load(&s.bad_crc) == 0, "%llu delivered frames had a bad CRC",
          (unsigned long long)atomic_load(&s.bad_crc));
  T_CHECK(st.eof_frames == atomic_load(&s.delivered), "eof_frames %llu != callbacks %llu",
          (unsigned long long)st.eof_frames, (unsigned long long)atomic_load(&s.delivered));

  printf(
      "stats pkts=%llu icrc_err=%llu writes=%llu bytes=%llu eof_frames=%llu psn_gaps=%llu "
      "proto_err=%llu meta_drops=%llu\n",
      (unsigned long long)st.pkts, (unsigned long long)st.icrc_err, (unsigned long long)st.writes,
      (unsigned long long)st.bytes, (unsigned long long)st.eof_frames,
      (unsigned long long)st.psn_gaps, (unsigned long long)st.proto_err,
      (unsigned long long)st.meta_drops);
  printf("  delivered=%llu (phase A %u + phase B %llu + 2 clean)\n",
         (unsigned long long)atomic_load(&s.delivered), N_FRAMES, (unsigned long long)lossy);

  printf("  total %.2f s\n", (double)(hml_clock_us() - t0) / 1e6);

  hml_roce_set_fault_hook(NULL, NULL);
  ops->close(&x);
  hml_roce_rx_close(s.rx);
  T_DONE("test_roce_e2e");
}
