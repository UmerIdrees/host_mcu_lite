/*
 * scene_mcu.c -- the mcu-profile end-to-end scene.
 *
 * WHAT THIS IS
 *   For each logical MCU target the scene derives the vendor's frame cap from a
 *   documented (assumed) memory budget -- renode/boards/ does not exist in this
 *   tree, so no board abstraction table can be authoritative -- and then:
 *     1. runs renode/firmware/freertos_demo's hml_mcu_demo_run() NATIVELY at
 *        that cap, i.e. the exact codec/CRC/reassembly C the firmware image
 *        compiles, on the build machine, and asserts every check passed;
 *     2. opens a real UDP receiver at that cap (hml_host), sends a cap-sized
 *        frame over loopback through hml_stream + the real hml_udp transport,
 *        and asserts it is delivered byte-exact;
 *     3. sends a cap+1 byte frame and asserts the receiver drops it on purpose
 *        and never presents it to the application.
 *
 * WHAT THIS IS NOT
 *   No Cortex-M instruction executes, no Renode model, no FreeRTOS, no lwIP, no
 *   peripheral, no silicon. The per-vendor numbers are assumptions labelled as
 *   such, printed per profile, and the heap contract asserted is the demo's own
 *   (two reasm buffers of the cap). docs/cpu_only_e2e.md carries the boundary.
 */

#include "e2e_cpu.h"

#include "hml_mcu_demo.h"

#include "hml/hml_discovery.h"
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

#define STREAM_ID 1u
#define SEED 7u
#define INFLIGHT 64u
#define MCU_PAT_FRAME_BASE 0ull
#define MCU_OVERCAP_EXTRA 1u

/* ---- documented-assumption vendor table ---------------------------------- */

typedef struct mcu_profile {
  const char *name;
  uint64_t sram;   /* assumed internal SRAM the example part family offers */
  uint64_t sdram;  /* assumed SDRAM, 0 when the profile has none */
  uint32_t frame_cap;
  const char *note;
} mcu_profile;

/* The heap contract (two cap-sized reassembly buffers) is asserted per profile;
 * a profile is only consistent when 2*cap fits the assumed memory. Every number
 * below is an assumption about a well-known part family -- there is no board
 * index in this tree to read it from -- so the scene prints it and the
 * documentation repeats it. */
static const mcu_profile k_profiles[] = {
    {"renode-cortexm4", 512u * 1024u, 4u * 1024u * 1024u, HML_MAX_FRAME_SIZE,
     "the .resc map; the default 1 MiB cap's 2 MiB heap lives in SDRAM"},
    {"stm32f4", 192u * 1024u, 0u, 64u * 1024u, "e.g. STM32F407: 128 KiB SRAM + 64 KiB CCM"},
    {"stm32f7", 320u * 1024u, 0u, 128u * 1024u, "e.g. STM32F746: 256 + 64 KiB"},
    {"stm32h7", 1024u * 1024u, 0u, 256u * 1024u, "e.g. STM32H743"},
    {"nxp-rt1062", 1024u * 1024u, 0u, 256u * 1024u, "i.MX RT1062: 1 MiB OCRAM total"},
    {"esp32-s3", 512u * 1024u, 0u, 128u * 1024u, "ESP32-S3 internal SRAM"},
    {"renesas-ra4m1", 96u * 1024u, 0u, 32u * 1024u, "RA4M1 SRAM"},
    {"microchip-same54", 192u * 1024u, 0u, 64u * 1024u, "SAME54 SRAM"},
};
static const unsigned k_profile_count = (unsigned)(sizeof(k_profiles) / sizeof(k_profiles[0]));

/* ---- demo output capture --------------------------------------------------- */

#define MCU_OUT_CAP 32768u

static char g_out[MCU_OUT_CAP];
static size_t g_out_n;
static bool g_out_overflow;

static void cap_puts(void *ctx, const char *line) {
  (void)ctx;
  size_t add = strlen(line);
  if (g_out_n + add + 1u > MCU_OUT_CAP) {
    g_out_overflow = true;
    return;
  }
  memcpy(g_out + g_out_n, line, add);
  g_out_n += add;
  g_out[g_out_n++] = '\n';
}

static const char *find_marker(const char *marker) {
  return strstr(g_out, marker);
}

/* ---- receiver (per profile) ------------------------------------------------ */

typedef struct mcurx {
  hml_host_t *host;
  atomic_int run;
  atomic_uint errors;
  atomic_ullong calls;
  atomic_ullong bad_hash;
  atomic_ullong bad_size;
  atomic_ullong zero_byte;
  uint32_t want_hash;
  size_t want_bytes;
  pthread_mutex_t lock;
  hml_reasm_stats rs;
  uint64_t frames;
  uint64_t eof;
} mcurx;

static void mcu_on_frame(void *ctx, uint16_t stream_id, uint32_t frame_seq, size_t frame_bytes,
                         uint32_t hash) {
  mcurx *r = (mcurx *)ctx;
  (void)stream_id;
  (void)frame_seq;
  (void)atomic_fetch_add(&r->calls, 1ull);
  if (frame_bytes == 0u) {
    (void)atomic_fetch_add(&r->zero_byte, 1ull);
    return;
  }
  if (hash != r->want_hash) (void)atomic_fetch_add(&r->bad_hash, 1ull);
  if (frame_bytes != r->want_bytes) (void)atomic_fetch_add(&r->bad_size, 1ull);
}

static void *mcu_rx_thread(void *arg) {
  mcurx *r = (mcurx *)arg;
  while (atomic_load(&r->run)) {
    hml_status st = hml_host_poll(r->host, 20);
    if (st != HML_OK && st != HML_ERR_TIMEOUT && st != HML_ERR_AGAIN)
      (void)atomic_fetch_add(&r->errors, 1u);
    pthread_mutex_lock(&r->lock);
    hml_host_stats(r->host, &r->rs, &r->frames, NULL, &r->eof, NULL);
    pthread_mutex_unlock(&r->lock);
  }
  return NULL;
}

static void mcu_snapshot(mcurx *r, hml_reasm_stats *rs, uint64_t *frames, uint64_t *eof) {
  pthread_mutex_lock(&r->lock);
  if (rs) *rs = r->rs;
  if (frames) *frames = r->frames;
  if (eof) *eof = r->eof;
  pthread_mutex_unlock(&r->lock);
}

static bool mcu_wait(mcurx *r, unsigned long long calls, uint32_t want_dropped, int ms) {
  uint64_t deadline = e2e_now_us() + (uint64_t)(ms < 0 ? 0 : (unsigned)ms) * 1000ull;
  for (;;) {
    if (atomic_load(&r->calls) >= calls) {
      hml_reasm_stats st;
      mcu_snapshot(r, &st, NULL, NULL);
      if (st.frames_dropped >= want_dropped) return true;
    }
    if (e2e_budget_ms() == -1) return false;
    if (e2e_now_us() >= deadline) return false;
    e2e_sleep_us(500);
  }
}

/* ---- sender ---------------------------------------------------------------- */

typedef struct mcutx {
  uint64_t sent;
  mcurx *rx;
} mcutx;

static void mcu_flow_credit(mcutx *t) {
  int budget = e2e_budget_ms();
  uint64_t give_up = e2e_now_us() + (uint64_t)(budget > 0 ? budget : 5000) * 1000ull;
  for (;;) {
    hml_reasm_stats st;
    mcu_snapshot(t->rx, &st, NULL, NULL);
    if ((uint64_t)st.frags_recv + INFLIGHT >= t->sent) break;
    if (e2e_budget_ms() == -1 || e2e_now_us() >= give_up) break;
    e2e_sleep_us(200);
  }
}

/* Send one frame of `size` bytes straight from `buf`, chunked exactly as the
 * real transport expects, with a sender-side credit so the kernel socket
 * buffer cannot overflow even for the 1 MiB cap profile. */
static int mcu_send_frame(hml_stream_t *s, mcutx *t, const uint8_t *buf, size_t size) {
  size_t off = 0;
  int first = 1;
  while (off < size) {
    size_t want = size - off;
    if (want > (size_t)HML_MAX_FRAG_PAYLOAD) want = (size_t)HML_MAX_FRAG_PAYLOAD;
    int last = (off + want == size) != 0;
    hml_status st = HML_ERR_AGAIN;
    for (int tries = 0; tries < 2000 && st == HML_ERR_AGAIN; tries++) {
      if (e2e_budget_ms() == -1) return -2;
      st = hml_stream_send_frag(s, buf + off, want, first != 0, last != 0, e2e_now_us());
      if (st == HML_ERR_AGAIN) e2e_sleep_us(200);
    }
    if (st != HML_OK) return -1;
    t->sent++;
    off += want;
    first = 0;
    mcu_flow_credit(t);
  }
  return 0;
}

/* ---- per-profile socket fail-closed check ---------------------------------- */

static int profile_loopback(e2e_harness *h, const mcu_profile *p, uint32_t cap, uint16_t port) {
  char tag[96], ds[80];

  mcurx rx;
  memset(&rx, 0, sizeof(rx));
  (void)pthread_mutex_init(&rx.lock, NULL);

  hml_status st = hml_host_open(&rx.host, port, (size_t)cap);
  if (st != HML_OK) {
    (void)snprintf(ds, sizeof(ds), "hml_host_open status=%d port=%u", (int)st, (unsigned)port);
    (void)snprintf(tag, sizeof(tag), "%s.receiver_open", p->name);
    e2e_emit(h, tag, false, ds);
    return 1;
  }

  uint8_t *pat = (uint8_t *)malloc((size_t)cap + MCU_OVERCAP_EXTRA);
  if (pat == NULL) {
    (void)snprintf(tag, sizeof(tag), "%s.alloc", p->name);
    e2e_emit(h, tag, false, "out of memory for the pattern buffer");
    hml_host_close(rx.host);
    return 1;
  }

  hml_synth_pattern_fill(pat, cap, MCU_PAT_FRAME_BASE, SEED);
  rx.want_hash = hml_fnv1a32(pat, cap);
  rx.want_bytes = cap;
  (void)hml_host_set_frame_cb(rx.host, mcu_on_frame, &rx);

  atomic_store(&rx.run, 1);
  pthread_t rxt;
  if (pthread_create(&rxt, NULL, mcu_rx_thread, &rx) != 0) {
    (void)snprintf(tag, sizeof(tag), "%s.receiver_thread", p->name);
    e2e_emit(h, tag, false, "pthread_create failed");
    free(pat);
    hml_host_close(rx.host);
    return 1;
  }
  e2e_sleep_us(20000);

  st = hml_udp_register();
  if (st != HML_OK) {
    (void)snprintf(tag, sizeof(tag), "%s.transport_register", p->name);
    char dr[64];
    (void)snprintf(dr, sizeof(dr), "hml_udp_register status=%d", (int)st);
    e2e_emit(h, tag, false, dr);
    atomic_store(&rx.run, 0);
    (void)pthread_join(rxt, NULL);
    hml_host_close(rx.host);
    free(pat);
    return 1;
  }

  hml_dest_cfg dest;
  memset(&dest, 0, sizeof(dest));
  dest.kind = HML_XPORT_UDP;
  dest.ipv4 = 0x7F000001u;
  dest.udp_port = port;
  dest.frame_size = cap;
  dest.hop_limit = 64u;

  hml_xport_t x;
  memset(&x, 0, sizeof(x));
  x.ops = hml_udp_ops();
  st = x.ops->open(&x, &dest, STREAM_ID);
  if (st != HML_OK) {
    (void)snprintf(tag, sizeof(tag), "%s.transport_open", p->name);
    char dr[64];
    (void)snprintf(dr, sizeof(dr), "status=%d", (int)st);
    e2e_emit(h, tag, false, dr);
    atomic_store(&rx.run, 0);
    (void)pthread_join(rxt, NULL);
    hml_host_close(rx.host);
    free(pat);
    return 1;
  }

  hml_stream_t *s = NULL;
  hml_stream_cfg sc;
  memset(&sc, 0, sizeof(sc));
  sc.stream_id = STREAM_ID;
  sc.max_frame_bytes = cap + (uint32_t)HML_MAX_FRAG_PAYLOAD;
  sc.seq_start = 0;
  sc.mtu_effective = 1500u;
  st = hml_stream_open(&s, &sc, &x);
  if (st != HML_OK) {
    (void)snprintf(tag, sizeof(tag), "%s.stream_open", p->name);
    char dr[64];
    (void)snprintf(dr, sizeof(dr), "status=%d", (int)st);
    e2e_emit(h, tag, false, dr);
    (void)x.ops->close(&x);
    atomic_store(&rx.run, 0);
    (void)pthread_join(rxt, NULL);
    hml_host_close(rx.host);
    free(pat);
    return 1;
  }

  mcutx tx;
  memset(&tx, 0, sizeof(tx));
  tx.rx = &rx;

  int rc = mcu_send_frame(s, &tx, pat, (size_t)cap);
  (void)snprintf(tag, sizeof(tag), "%s.cap_frame_sent", p->name);
  (void)snprintf(ds, sizeof(ds), "send failed or deadline hit");
  e2e_emit(h, tag, rc == 0, rc == 0 ? "" : ds);

  bool delivered = mcu_wait(&rx, 1ull, 0u, 10000);
  hml_reasm_stats s1;
  uint64_t frames1 = 0u, eof1 = 0u;
  mcu_snapshot(&rx, &s1, &frames1, &eof1);
  (void)snprintf(tag, sizeof(tag), "%s.cap_frame_delivered", p->name);
  e2e_emit(h, tag, delivered && frames1 == 1u && eof1 == 1u && s1.frames_ok == 1u, "");
  (void)snprintf(tag, sizeof(tag), "%s.cap_frame_payload", p->name);
  e2e_emit(h, tag, atomic_load(&rx.bad_hash) == 0u && atomic_load(&rx.bad_size) == 0u &&
                       hml_host_last_seq(rx.host) == 0u &&
                       hml_host_last_bytes(rx.host) == (size_t)cap,
           "");
  (void)snprintf(tag, sizeof(tag), "%s.cap_frame_no_gap", p->name);
  e2e_emit(h, tag, s1.frames_dropped == 0u && s1.gaps == 0u && s1.crc_fail == 0u &&
                       s1.proto_fail == 0u,
           "");

  rc = mcu_send_frame(s, &tx, pat, (size_t)cap + MCU_OVERCAP_EXTRA);
  (void)snprintf(tag, sizeof(tag), "%s.overcap_frame_sent", p->name);
  e2e_emit(h, tag, rc == 0, rc == 0 ? "" : ds);

  bool dropped_wait = mcu_wait(&rx, 1ull, 1u, 10000);
  hml_reasm_stats s2;
  uint64_t frames2 = 0u, eof2 = 0u;
  mcu_snapshot(&rx, &s2, &frames2, &eof2);
  (void)snprintf(tag, sizeof(tag), "%s.overcap_dropped", p->name);
  (void)snprintf(ds, sizeof(ds), "frames_dropped=%u", (unsigned)s2.frames_dropped);
  e2e_emit(h, tag, dropped_wait && s2.frames_dropped == 1u && s2.gaps == 0u,
           s2.frames_dropped == 1u ? "" : ds);
  (void)snprintf(tag, sizeof(tag), "%s.overcap_never_delivered", p->name);
  e2e_emit(h, tag, frames2 == 1u && eof2 == 1u && s2.frames_ok == 1u && s2.crc_fail == 0u &&
                       s2.proto_fail == 0u && atomic_load(&rx.calls) == 1ull,
           "");
  (void)snprintf(tag, sizeof(tag), "%s.no_rx_errors", p->name);
  e2e_emit(h, tag, atomic_load(&rx.errors) == 0u, "");

  atomic_store(&rx.run, 0);
  (void)pthread_join(rxt, NULL);
  (void)hml_stream_close(s);
  (void)x.ops->close(&x);
  hml_host_close(rx.host);
  free(pat);
  return 0;
}

/* ---- profile driver -------------------------------------------------------- */

static int run_profile(e2e_harness *h, const mcu_profile *p) {
  char d[200];
  char tag[96];

  /* 1. the consistency and memory-contract assertions. */
  uint64_t heap = 2ull * p->frame_cap;
  bool fits = heap <= p->sram || (p->sdram != 0u && heap <= p->sdram);
  (void)snprintf(d, sizeof(d),
                 "assumption sram=%llu sdram=%llu frame_cap=%u heap=%llu (%s)",
                 (unsigned long long)p->sram, (unsigned long long)p->sdram,
                 (unsigned)p->frame_cap, (unsigned long long)heap,
                 fits ? "fits the assumed memory" : "DOES NOT fit: configuration error");
  (void)snprintf(tag, sizeof(tag), "%s.memory_contract", p->name);
  e2e_emit(h, tag, fits, d);

  /* The wire test uses a cap that leaves room for an over-cap frame under the
   * 1 MiB stream clamp, which is otherwise the hard wire-format maximum. Keep
   * the profile's default cap for the demo; only the socket check is lowered. */
  uint32_t wire_cap = p->frame_cap;
  if ((uint64_t)wire_cap + MCU_OVERCAP_EXTRA + 2u >= (uint64_t)HML_MAX_FRAME_SIZE)
    wire_cap = (uint32_t)HML_MAX_FRAME_SIZE - 2u * (uint32_t)HML_MAX_FRAG_PAYLOAD;
  (void)snprintf(tag, sizeof(tag), "%s.cap_sane", p->name);
  bool cap_ok = wire_cap >= (uint32_t)HML_MAX_FRAG_PAYLOAD &&
                wire_cap <= p->frame_cap &&
                (uint64_t)wire_cap + MCU_OVERCAP_EXTRA + 1u < (uint64_t)HML_MAX_FRAME_SIZE;
  e2e_emit(h, tag, cap_ok,
           "cap must admit >= 1 full fragment and leave room for an over-cap frame "
           "below the 1 MiB wire max");
  if (wire_cap != p->frame_cap) {
    (void)snprintf(tag, sizeof(tag), "%s.wire_cap_lowered", p->name);
    (void)snprintf(d, sizeof(d),
                   "wire cap lowered to %u so the over-cap frame stays expressible "
                   "under the stream's 1 MiB clamp; demo still runs at %u",
                   (unsigned)wire_cap, (unsigned)p->frame_cap);
    e2e_emit(h, tag, true, d);
  }

  /* 2. the demo body at the profile's cap, natively. */
  hml_mcu_demo_cfg cfg;
  hml_mcu_demo_io io;
  hml_mcu_demo_result res;
  memset(&cfg, 0, sizeof(cfg));
  memset(&io, 0, sizeof(io));
  memset(&res, 0, sizeof(res));
  cfg.frame_bytes = p->frame_cap;
  cfg.device_uuid = 0x484d4c4du; /* 'HMLM' */
  cfg.device_caps = HML_CAP_MCU;
  g_out_n = 0u;
  g_out_overflow = false;
  io.ctx = NULL;
  io.puts = cap_puts;
  int rc = hml_mcu_demo_run(&cfg, &io, &res);

  (void)snprintf(tag, sizeof(tag), "%s.demo_run", p->name);
  e2e_emit(h, tag, rc == 0 && res.failures == 0u && res.checks >= 20u,
           "");
  (void)snprintf(tag, sizeof(tag), "%s.demo_checks", p->name);
  e2e_emit(h, tag, res.checks >= 20u && res.checks > 0u, "");
  (void)snprintf(tag, sizeof(tag), "%s.demo_frames", p->name);
  e2e_emit(h, tag, res.frames_delivered >= 7u,
           "");
  (void)snprintf(tag, sizeof(tag), "%s.demo_markers", p->name);
  e2e_emit(h, tag, !g_out_overflow && find_marker(HML_MCU_DEMO_MARK_READY) != NULL &&
                       find_marker(HML_MCU_DEMO_MARK_DONE) != NULL &&
                       find_marker(HML_MCU_DEMO_MARK_FAIL) == NULL,
           "");

  /* 3. cap-sized + over-cap frames over a real loopback socket. */
  uint16_t port = e2e_port_pick();
  if (port == 0u) {
    (void)snprintf(tag, sizeof(tag), "%s.port", p->name);
    e2e_emit(h, tag, false, "no free port in the private loopback range");
    return 1;
  }
  return profile_loopback(h, p, wire_cap, port);
}

/* ---- scene ---------------------------------------------------------------- */

int e2e_scene_mcu(e2e_harness *h) {
  if (e2e_budget_ms() == -1) {
    e2e_emit(h, "late_start", false, "run-wide deadline already expired");
    return 1;
  }
  e2e_emit(h, "mcu.warning",
           true, "every vendor number is a labelled assumption (no renode/boards table)");
  int rc = 0;
  for (unsigned i = 0; i < k_profile_count; i++) {
    if (e2e_budget_ms() == -1) {
      e2e_emit(h, "mcu.deadline", false, "run-wide deadline expired mid-profiles");
      rc = 1;
      break;
    }
    if (run_profile(h, &k_profiles[i]) != 0) rc = 1;
  }
  return e2e_finish(h) == 0 ? rc : 1;
}