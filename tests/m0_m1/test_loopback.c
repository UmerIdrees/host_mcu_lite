/* M1: real UDP loopback on 127.0.0.1 - hml_gen-style sender vs. hml_host receiver.
 *
 * argv[1] optionally overrides the UDP port (default 45511) so parallel CI runs
 * do not collide.
 */
#include "hml/hml_protocol.h"
#include "hml/hml_sock.h"
#include "hml/hml_stream.h"
#include "hml/hml_transport.h"
#include "hml_host.h"
#include "hml_synth_priv.h"
#include "hml_test_util.h"
#include "hml_udp_priv.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRAME_BYTES 9000u
#define FRAGS_PER_FRAME ((FRAME_BYTES + HML_MAX_FRAG_PAYLOAD - 1u) / HML_MAX_FRAG_PAYLOAD)
#define PHASE1 1000u
#define PHASE2 100u
#define PHASE3 3u
#define N_FRAMES (PHASE1 + PHASE2 + PHASE3)
#define SEED 1u
#define STREAM_ID 1u
/* Flow control for the test sender: never run more than WINDOW frames ahead of the
 * receiver, so the kernel socket buffers cannot overrun even if the receiver thread is
 * descheduled. The lossless phase must then complete with zero drops. */
#define WINDOW 8u
#define PACE_US 500ull
#define WINDOW_WAIT_US 5000000ull

/* p99 EOF-latency budget for the clean (phase 1) loopback path. Chosen from the
 * measured data on this (VirtualBox) machine: thirteen clean 9000-byte /
 * 7-fragment / 127.0.0.1 runs at the default pacing gave p50 = 200-250us and
 * p99 between 9.8 and 32.7ms (p99 == max, because the top-bucket rule reports the
 * exact tail when the rank lands past the 3150us histogram). 40000us is the
 * smallest round number above the worst measured p99; it is a LOOPBACK/VM number,
 * not a real-hardware target (see docs/latency.md). */
#define L_BUDGET_US 40000ull

static hml_host_t *g_host;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_rx_run = 1;
static int g_rx_error = 0;

static struct {
  uint64_t frames, bytes, eof, last_hash;
  hml_reasm_stats rs;
  hml_latency lat;
} g_snap;

static uint32_t g_expected[N_FRAMES];
static uint8_t *g_seen;
static uint64_t g_seen_count;
static uint32_t g_bad_hash, g_bad_size, g_dup, g_out_of_range;
static uint32_t g_seq_start;

/* ---- receiver thread ---- */

static void on_frame(void *ctx, uint16_t stream_id, uint32_t frame_seq, size_t frame_bytes,
                     uint32_t hash) {
  (void)ctx;
  (void)stream_id;
  uint64_t i = (uint64_t)(frame_seq - g_seq_start) / FRAGS_PER_FRAME;
  if (frame_bytes != (size_t)FRAME_BYTES) {
    g_bad_size++;
    return;
  }
  if (i >= (uint64_t)N_FRAMES) {
    g_out_of_range++;
    return;
  }
  if (g_seen[i]) {
    g_dup++;
    return;
  }
  g_seen[i] = 1;
  __atomic_add_fetch(&g_seen_count, 1, __ATOMIC_RELAXED);
  if (hash != g_expected[i]) g_bad_hash++;
}

static void *rx_thread(void *arg) {
  (void)arg;
  while (__atomic_load_n(&g_rx_run, __ATOMIC_ACQUIRE)) {
    hml_status st = hml_host_poll(g_host, 20);
    if (st != HML_OK && st != HML_ERR_TIMEOUT) {
      g_rx_error = 1;
      break;
    }
    pthread_mutex_lock(&g_mtx);
    hml_host_stats(g_host, &g_snap.rs, &g_snap.frames, &g_snap.bytes, &g_snap.eof,
                   &g_snap.last_hash);
    (void)hml_host_latency(g_host, &g_snap.lat);
    pthread_mutex_unlock(&g_mtx);
  }
  return NULL;
}

static void snap(uint64_t *frames, uint64_t *bytes, uint64_t *eof, hml_reasm_stats *rs) {
  pthread_mutex_lock(&g_mtx);
  if (frames) *frames = g_snap.frames;
  if (bytes) *bytes = g_snap.bytes;
  if (eof) *eof = g_snap.eof;
  if (rs) *rs = g_snap.rs;
  pthread_mutex_unlock(&g_mtx);
}

static void snap_lat(hml_latency *lat) {
  pthread_mutex_lock(&g_mtx);
  if (lat) *lat = g_snap.lat;
  pthread_mutex_unlock(&g_mtx);
}

static int wait_for(uint64_t target_frames, uint64_t timeout_ms) {
  uint64_t t0 = hml_clock_us();
  for (;;) {
    uint64_t f = 0;
    snap(&f, NULL, NULL, NULL);
    if (f >= target_frames) return 0;
    if (hml_clock_us() - t0 > timeout_ms * 1000ull) return -1;
    struct timespec ts = {0, 1000000L};
    (void)nanosleep(&ts, NULL);
  }
}

static void sleep_us(uint64_t us) {
  struct timespec ts;
  ts.tv_sec = (time_t)(us / 1000000ull);
  ts.tv_nsec = (long)((us % 1000000ull) * 1000ull);
  (void)nanosleep(&ts, NULL);
}

/* ---- lossy wrapper transport: drops every Nth datagram ---- */

typedef struct {
  hml_xport_t inner;
  uint64_t sent;
  uint64_t dropped;
  uint32_t every;
} lossy;

static hml_status lossy_open(hml_xport_t *x, const hml_dest_cfg *dest, uint16_t stream_id) {
  lossy *l = (lossy *)x->impl;
  if (!l) return HML_ERR_STATE;
  l->sent = 0;
  l->dropped = 0;
  return hml_udp_ops()->open(&l->inner, dest, stream_id);
}

static hml_status lossy_send(hml_xport_t *x, const hml_frame_hdr *hdr, const uint8_t *payload,
                             size_t payload_len) {
  lossy *l = (lossy *)x->impl;
  l->sent++;
  if (l->every && (l->sent % l->every) == 0) {
    l->dropped++;
    return HML_OK; /* silently discard */
  }
  return hml_udp_ops()->send_frag(&l->inner, hdr, payload, payload_len);
}

static hml_status lossy_flush(hml_xport_t *x) { return hml_udp_ops()->flush(&((lossy *)x->impl)->inner); }

static void lossy_close(hml_xport_t *x) {
  lossy *l = (lossy *)x->impl;
  if (!l) return;
  hml_udp_ops()->close(&l->inner);
  l->inner.impl = NULL;
}

static const hml_xport_ops g_lossy_ops = {"lossy-udp", lossy_open, lossy_send, lossy_flush,
                                          lossy_close};

/* ---- sender ---- */

static uint32_t g_again; /* datagrams that hit EAGAIN and were retried */

static hml_status send_frame(hml_stream_t *s, hml_sensor_t *sensor, uint8_t *fbuf) {
  size_t off = 0;
  int first = 1;
  while (off < FRAME_BYTES) {
    size_t want = FRAME_BYTES - off;
    if (want > (size_t)HML_MAX_FRAG_PAYLOAD) want = (size_t)HML_MAX_FRAG_PAYLOAD;
    size_t got = 0;
    hml_status st = sensor->ops->acquire(sensor, fbuf + off, want, &got);
    if (st != HML_OK || got == 0) return st == HML_OK ? HML_ERR_STATE : st;
    int last = (off + got == FRAME_BYTES);
    for (;;) {
      st = hml_stream_send_frag(s, fbuf + off, got, first != 0, last != 0, hml_clock_us());
      if (st != HML_ERR_AGAIN) break;
      g_again++;
      sleep_us(200);
    }
    if (st != HML_OK) return st;
    off += got;
    first = 0;
  }
  return sensor->ops->release(sensor);
}

typedef struct {
  hml_status rc;
  uint64_t datagrams;
} send_result;

static uint64_t g_sent_frames; /* frames handed to the transport by any phase */

static send_result send_batch(hml_sensor_t *sensor, hml_stream_t *stream, uint8_t *fbuf,
                              unsigned n, unsigned window, uint64_t pace_us) {
  send_result r;
  r.rc = HML_OK;
  r.datagrams = 0;
  for (unsigned i = 0; i < n; i++) {
    if (window) {
      /* wait for receiver credits */
      uint64_t deadline = hml_clock_us() + WINDOW_WAIT_US;
      while (g_sent_frames - __atomic_load_n(&g_seen_count, __ATOMIC_RELAXED) >= window) {
        if (hml_clock_us() > deadline) {
          r.rc = HML_ERR_TIMEOUT;
          return r;
        }
        sleep_us(200);
      }
    }
    hml_status st = send_frame(stream, sensor, fbuf);
    if (st != HML_OK) {
      r.rc = st;
      return r;
    }
    g_sent_frames++;
    r.datagrams += FRAGS_PER_FRAME;
    if (pace_us) sleep_us(pace_us);
  }
  return r;
}

int main(int argc, char **argv) {
  uint16_t port = 45511u;
  if (argc > 1) {
    long v = strtol(argv[1], NULL, 0);
    if (v > 0 && v < 65536) port = (uint16_t)v;
  }

  g_seen = (uint8_t *)calloc(N_FRAMES, 1);
  uint8_t *fbuf = (uint8_t *)malloc(FRAME_BYTES);
  if (!g_seen || !fbuf) {
    printf("FAIL out of memory\n");
    return 1;
  }

  /* expected hash of every frame the generator will produce */
  for (unsigned i = 0; i < N_FRAMES; i++) {
    hml_synth_pattern_fill(fbuf, FRAME_BYTES, (uint64_t)i * FRAME_BYTES, SEED);
    g_expected[i] = hml_fnv1a32(fbuf, FRAME_BYTES);
  }

  /* receiver */
  hml_status st = hml_host_open(&g_host, port, 65536u);
  if (st != HML_OK) {
    printf("FAIL hml_host_open(%u) status %d\n", (unsigned)port, (int)st);
    return 1;
  }
  (void)hml_host_set_frame_cb(g_host, on_frame, NULL);

  pthread_t rx;
  if (pthread_create(&rx, NULL, rx_thread, NULL) != 0) {
    printf("FAIL pthread_create\n");
    return 1;
  }
  sleep_us(20000);

  /* sender */
  CHECK_STATUS(hml_udp_register(), HML_OK);
  CHECK(hml_transport_lookup("udp") == hml_udp_ops());
  CHECK_STATUS(hml_udp_register(), HML_OK); /* idempotent */

  hml_synth_cfg scfg;
  scfg.seed = SEED;
  scfg.frame_bytes = (size_t)HML_MAX_FRAG_PAYLOAD;
  hml_sensor_t sensor;
  memset(&sensor, 0, sizeof(sensor));
  sensor.ops = hml_synth_ops();
  CHECK_STATUS(sensor.ops->open(&sensor, &scfg), HML_OK);

  hml_dest_cfg dest;
  memset(&dest, 0, sizeof(dest));
  dest.kind = HML_XPORT_UDP;
  dest.ipv4 = 0x7F000001u; /* 127.0.0.1, network byte order */
  dest.udp_port = port;
  dest.frame_size = FRAME_BYTES;
  dest.hop_limit = 64;

  g_seq_start = 0;
  lossy l1;
  hml_xport_t x1, x2;
  hml_stream_t *s1 = NULL, *s2 = NULL, *s3 = NULL;
  uint32_t seq = g_seq_start;
  uint64_t t_start = hml_clock_us();

  /* ---- phase 1: 1000 frames x 9000 bytes, clean path ---- */
  memset(&x1, 0, sizeof(x1));
  x1.ops = hml_udp_ops();
  CHECK_STATUS(x1.ops->open(&x1, &dest, STREAM_ID), HML_OK);
  CHECK_STATUS(x1.ops->open(&x1, &dest, STREAM_ID), HML_ERR_STATE); /* double open */
  {
    hml_stream_cfg c;
    c.stream_id = STREAM_ID;
    c.max_frame_bytes = FRAME_BYTES;
    c.seq_start = seq;
    c.mtu_effective = 1500;
    CHECK_STATUS(hml_stream_open(&s1, &c, &x1), HML_OK);
  }
  send_result r1 = send_batch(&sensor, s1, fbuf, PHASE1, WINDOW, PACE_US);
  CHECK_STATUS(r1.rc, HML_OK);
  seq = hml_stream_next_seq(s1);
  hml_stream_close(s1); /* also closes the transport */
  if (wait_for(PHASE1, 15000) != 0) printf("FAIL phase 1 stalled at %llu frames\n",
                                           (unsigned long long)g_seen_count);

  uint64_t f1 = 0, b1 = 0, e1 = 0;
  hml_reasm_stats rs1;
  snap(&f1, &b1, &e1, &rs1);
  uint64_t ms1 = (hml_clock_us() - t_start) / 1000ull;
  printf("  phase1: frames=%llu bytes=%llu eof=%llu seen=%llu in %llu ms (eagain=%u)\n",
         (unsigned long long)f1, (unsigned long long)b1, (unsigned long long)e1,
         (unsigned long long)g_seen_count, (unsigned long long)ms1, (unsigned)g_again);

  CHECK_EQ_U32(f1, PHASE1);
  CHECK_EQ_U32(b1, (uint64_t)PHASE1 * FRAME_BYTES);
  CHECK_EQ_U32(e1, PHASE1);
  CHECK_EQ_U32(g_seen_count, PHASE1);
  CHECK_EQ_U32(g_bad_hash, 0u);
  CHECK_EQ_U32(g_bad_size, 0u);
  CHECK_EQ_U32(g_dup, 0u);
  CHECK_EQ_U32(g_out_of_range, 0u);
  CHECK_EQ_U32(rs1.frames_dropped, 0u);
  CHECK_EQ_U32(rs1.gaps, 0u);
  CHECK_EQ_U32(rs1.crc_fail, 0u);
  CHECK_EQ_U32(rs1.proto_fail, 0u);

  /* ---- latency phase: clean-path EOF latency histogram ---- */
  {
    hml_latency lat;
    snap_lat(&lat);
    CHECK_EQ_U32(lat.n, PHASE1);        /* one sample per frame delivered */
    CHECK_EQ_U32(lat.skew_samples, 0u); /* loopback shares CLOCK_MONOTONIC */
    uint64_t p50 = 0, p95 = 0, p99 = 0;
    CHECK_STATUS(hml_latency_percentile(&lat, 50, &p50), HML_OK);
    CHECK_STATUS(hml_latency_percentile(&lat, 95, &p95), HML_OK);
    CHECK_STATUS(hml_latency_percentile(&lat, 99, &p99), HML_OK);
    printf("  latency p50=%lluus p95=%lluus p99=%lluus max=%lluus n=%llu bucket=%uus\n",
           (unsigned long long)p50, (unsigned long long)p95, (unsigned long long)p99,
           (unsigned long long)lat.max_us, (unsigned long long)lat.n, (unsigned)lat.bucket_us);
    CHECK(p99 <= L_BUDGET_US);
  }

  /* ---- phase 2: 100 frames with 1% of the datagrams dropped ---- */
  uint64_t drop_every = 100u; /* ~1% of datagrams */
  memset(&l1, 0, sizeof(l1));
  l1.every = (uint32_t)drop_every;
  memset(&x1, 0, sizeof(x1));
  x1.ops = &g_lossy_ops;
  x1.impl = &l1;
  l1.inner.ops = hml_udp_ops();
  CHECK_STATUS(g_lossy_ops.open(&x1, &dest, STREAM_ID), HML_OK);
  {
    hml_stream_cfg c;
    c.stream_id = STREAM_ID;
    c.max_frame_bytes = FRAME_BYTES;
    c.seq_start = seq;
    c.mtu_effective = 1500;
    CHECK_STATUS(hml_stream_open(&s2, &c, &x1), HML_OK);
  }
  uint64_t before2 = g_seen_count;
  uint32_t bad2_0 = g_bad_hash;
  /* the injected drops consume credits that will never be returned, so widen the window
   * by the number of datagrams the lossy transport is going to swallow */
  unsigned expect_dropped = (PHASE2 * FRAGS_PER_FRAME) / (unsigned)drop_every;
  send_result r2 = send_batch(&sensor, s2, fbuf, PHASE2, WINDOW + expect_dropped, PACE_US);
  CHECK_STATUS(r2.rc, HML_OK);
  seq = hml_stream_next_seq(s2);
  hml_stream_close(s2);
  /* the sender must not hang and the receiver must drain without help: every dropped
   * datagram costs at most one frame, so the rest must arrive on its own */
  uint64_t expect_drops = r2.datagrams / drop_every;
  CHECK_EQ_U32(expect_drops, expect_dropped);
  if (wait_for(before2 + (PHASE2 - expect_drops), 5000) != 0)
    printf("FAIL phase 2 stalled (loss phase must not hang)\n");
  sleep_us(200000);

  uint64_t f2 = 0, e2 = 0;
  hml_reasm_stats rs2;
  snap(&f2, NULL, &e2, &rs2);
  uint64_t got2 = g_seen_count - before2;
  printf("  phase2: sent=%u dropped=%llu delivered=%llu frames_dropped=%u gaps=%u\n", PHASE2,
         (unsigned long long)l1.dropped, (unsigned long long)got2, (unsigned)rs2.frames_dropped,
         (unsigned)rs2.gaps);
  CHECK(l1.dropped > 0u);                     /* loss really was injected */
  CHECK_EQ_U32(l1.dropped, (uint32_t)expect_drops);
  CHECK(got2 <= PHASE2);                      /* cannot deliver more than was sent */
  CHECK(got2 >= PHASE2 - expect_drops);       /* at most one frame lost per drop */
  CHECK(g_bad_hash - bad2_0 == 0u);           /* every delivered frame is bit-exact */
  CHECK_EQ_U32(g_bad_size, 0u);
  CHECK_EQ_U32(g_dup, 0u);
  CHECK(rs2.frames_dropped > rs1.frames_dropped); /* losses are accounted for */
  CHECK_EQ_U32(e2, f2);

  /* ---- phase 3: the stream recovers on the next good frame ---- */
  memset(&x2, 0, sizeof(x2));
  x2.ops = hml_udp_ops();
  CHECK_STATUS(x2.ops->open(&x2, &dest, STREAM_ID), HML_OK);
  {
    hml_stream_cfg c;
    c.stream_id = STREAM_ID;
    c.max_frame_bytes = FRAME_BYTES;
    c.seq_start = seq;
    c.mtu_effective = 1500;
    CHECK_STATUS(hml_stream_open(&s3, &c, &x2), HML_OK);
  }
  uint64_t before3 = g_seen_count;
  uint32_t bad3_0 = g_bad_hash;
  send_result r3 = send_batch(&sensor, s3, fbuf, PHASE3, 0, 0);
  CHECK_STATUS(r3.rc, HML_OK);
  hml_stream_close(s3);
  if (wait_for(before3 + PHASE3, 5000) != 0) printf("FAIL phase 3 stalled: no recovery\n");

  uint64_t f3 = 0, b3 = 0, e3 = 0;
  snap(&f3, &b3, &e3, NULL);
  uint64_t got3 = g_seen_count - before3;
  printf("  phase3: delivered=%llu (recovery), total frames=%llu\n", (unsigned long long)got3,
         (unsigned long long)f3);
  CHECK_EQ_U32(got3, PHASE3);
  { /* the first good frames after the loss, not the first frames of the phase */
    unsigned first = PHASE1;
    for (; first + PHASE3 <= PHASE1 + PHASE2; first++) {
      unsigned run = 0;
      while (run < PHASE3 && g_seen[first + run] == 1u) run++;
      if (run == PHASE3) break;
    }
    CHECK(first + PHASE3 <= PHASE1 + PHASE2); /* the receiver recovered */
  }
  CHECK(g_bad_hash - bad3_0 == 0u);
  CHECK_EQ_U32(g_bad_size, 0u);
  CHECK_EQ_U32(g_dup, 0u);
  CHECK_EQ_U32(f3, PHASE1 + got2 + PHASE3);
  CHECK_EQ_U32(b3, f3 * FRAME_BYTES);
  CHECK_EQ_U32(e3, f3);
  CHECK_EQ_U32(g_rx_error, 0u);

  __atomic_store_n(&g_rx_run, 0, __ATOMIC_RELEASE);
  pthread_join(rx, NULL);
  hml_host_close(g_host);
  sensor.ops->close(&sensor);
  free(fbuf);
  free(g_seen);

  uint64_t total_ms = (hml_clock_us() - t_start) / 1000ull;
  printf("  total %llu ms, %u frames, %llu bytes\n", (unsigned long long)total_ms,
         (unsigned)N_FRAMES, (unsigned long long)((uint64_t)N_FRAMES * FRAME_BYTES));

  TEST_DONE();
}
