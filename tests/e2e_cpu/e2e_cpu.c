/*
 * e2e_cpu.c -- CPU-only end-to-end runner for Host MCU Lite (hml).
 *
 * WHAT THIS IS
 *   A single host binary that runs the full vendor-matrix *logic* on the build
 *   machine's CPU. Each subcommand is one end-to-end scene: it opens real UDP
 *   sockets on 127.0.0.1, drives the shipping library, and asserts on counters
 *   the library itself keeps. There is no mock layer, no shim that fakes a
 *   frame, and no assertion that is not read off a real code path.
 *
 * WHAT THIS IS NOT
 *   It is not a microcontroller result. No Cortex-M instruction executes, no
 *   Renode model is instantiated, no FreeRTOS or lwIP runs, and no physical
 *   part is touched. The MCU scenes derive each vendor's memory constraints
 *   and then run the *same C* that the firmware image runs, natively. That is
 *   evidence about this repository's code, not about silicon. See
 *   docs/cpu_only_e2e.md, and docs/renode.md section 5 for the boundary the
 *   emulator harness itself declares.
 *
 * SCENES
 *   udp-loopback   T0: real hml_stream + UDP transport -> real hml_host
 *                  receiver, in-order and on-the-wire reordered, asserting
 *                  frames/bytes/eof/gaps/crc_fail/proto_fail end to end.
 *   roce-e2e       T1a: real hml_rocev2 emitter -> real hml_roce_demux over
 *                  loopback, asserting the known-good pattern of
 *                  24 pkts / 21 writes / 27000 B / 0 errors for 3 x 9000-byte
 *                  frames, then a loss phase and a recovery phase.
 *   reasm-edge     hml_reasm driven through a real socket with the
 *                  pathological sequences: gaps, duplicates, reversed
 *                  arrival, oversized, short tail, corrupt CRC, EOF-only
 *                  flush.
 *   control-beacon Real hml_control server + real hml_client over loopback,
 *                  plus the hml_beacon golden vector and its rejections.
 *   mcu-profile    Per logical MCU target: derive the vendor's frame cap and
 *                  heap budget, run renode/firmware/freertos_demo's
 *                  hml_mcu_demo_run() at that cap, then push a cap-sized
 *                  frame over a real socket and assert the over-cap frame is
 *                  dropped rather than delivered.
 *
 * EXIT STATUS
 *   0 every case passed, 1 any case failed or any step could not be executed.
 */

#include "e2e_cpu.h"

#include "hml/hml_crc.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ---- run-wide knobs ------------------------------------------------------ */

unsigned g_e2e_frames = 200u;
uint64_t g_e2e_deadline = 0u;

int e2e_budget_ms(void) {
  if (g_e2e_deadline == 0u) return 0;
  if (e2e_now_us() >= g_e2e_deadline) return -1;
  return (int)((g_e2e_deadline - e2e_now_us()) / 1000ull);
}

/* ---- harness ------------------------------------------------------------- */

void e2e_emit(e2e_harness *h, const char *name, bool ok, const char *detail) {
  h->checks++;
  if (!ok) h->fails++;
  printf("E2E scene=%s case=%s %s %s\n", h->scene, name, ok ? "PASS" : "FAIL",
         detail ? detail : "");
  fflush(stdout);
}

void e2e_skip(e2e_harness *h, const char *id, const char *reason) {
  h->skipped++;
  printf("E2E scene=%s skip=%s %s\n", h->scene, id, reason);
  fflush(stdout);
}

void e2e_case(e2e_harness *h, const char *name, bool ok) { e2e_emit(h, name, ok, ""); }

void e2e_eq_u(e2e_harness *h, const char *name, unsigned long long got,
              unsigned long long want) {
  char d[160];
  (void)snprintf(d, sizeof(d), "got=%llu want=%llu", got, want);
  e2e_emit(h, name, got == want, d);
}

void e2e_eq_i(e2e_harness *h, const char *name, long got, long want) {
  char d[160];
  (void)snprintf(d, sizeof(d), "got=%ld want=%ld", got, want);
  e2e_emit(h, name, got == want, d);
}

void e2e_eq_s(e2e_harness *h, const char *name, const char *got, const char *want) {
  char d[400];
  (void)snprintf(d, sizeof(d), "got=\"%s\" want=\"%s\"", got ? got : "(null)",
                 want ? want : "(null)");
  e2e_emit(h, name, got != NULL && want != NULL && strcmp(got, want) == 0, d);
}

int e2e_finish(e2e_harness *h) {
  uint64_t ms = (e2e_now_us() - h->t0_us) / 1000ull;
  printf("E2E scene=%s result=%s checks=%u fails=%u skipped=%u elapsed_ms=%llu\n", h->scene,
         h->fails == 0u ? "PASS" : "FAIL", h->checks, h->fails, h->skipped,
         (unsigned long long)ms);
  fflush(stdout);
  return h->fails == 0u ? 0 : 1;
}

/* ---- ports and time ------------------------------------------------------ */

void e2e_sleep_us(uint64_t us) {
  struct timespec ts;
  ts.tv_sec = (time_t)(us / 1000000ull);
  ts.tv_nsec = (long)((us % 1000000ull) * 1000ull);
  (void)nanosleep(&ts, NULL);
}

uint64_t e2e_now_us(void) { return hml_clock_us(); }

bool e2e_port_free(uint16_t port) {
  if (port == 0u) return false;
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(port);
  bool ok = bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
  (void)close(fd);
  return ok;
}

uint16_t e2e_port_pick(void) {
  for (unsigned i = 0; i < E2E_PORT_COUNT; i++) {
    uint16_t p = (uint16_t)(E2E_PORT_BASE + i);
    if (e2e_port_free(p)) return p;
  }
  return 0u;
}

/* ---- datagram builder ---------------------------------------------------- */

size_t e2e_build_dg(uint8_t *dg, size_t cap, uint16_t stream, uint32_t seq, uint16_t frag_idx,
                    uint16_t frag_cnt, uint8_t flags, uint64_t ts_us, const uint8_t *payload,
                    uint16_t len) {
  size_t n = (size_t)HML_FRAME_HDR_SIZE + (size_t)len + (size_t)HML_TRAIL_CRC_SIZE;
  if (cap < n) return 0u;
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
  return n;
}

/* ---- scene table --------------------------------------------------------- */

typedef int (*scene_fn)(e2e_harness *h);

typedef struct scene {
  const char *name;
  scene_fn run;
  const char *blurb;
} scene;

static const scene k_scenes[] = {
    {"udp-loopback", e2e_scene_udp,
     "T0 real socket: hml_stream + udp transport -> hml_host, in order and reordered"},
    {"roce-e2e", e2e_scene_roce,
     "T1a real socket: hml_rocev2 emitter -> hml_roce_demux, 3x9000B known-good pattern"},
    {"reasm-edge", e2e_scene_reasm,
     "hml_reasm over a real socket: gaps, duplicates, reversed, oversized, short tail, CRC"},
    {"control-beacon", e2e_scene_control,
     "real hml_control server + hml_client over loopback, plus the beacon golden vector"},
    {"mcu-profile", e2e_scene_mcu,
     "per logical MCU target: vendor frame cap, hml_mcu_demo_run at that cap, cap-sized "
     "loopback frame, over-cap fail-closed"},
};
static const unsigned k_scene_count = (unsigned)(sizeof(k_scenes) / sizeof(k_scenes[0]));

static void usage(void) {
  printf(
      "usage: e2e_cpu <scene>|--list|--all [options]\n"
      "scenes:\n");
  for (unsigned i = 0; i < k_scene_count; i++)
    printf("  %-15s %s\n", k_scenes[i].name, k_scenes[i].blurb);
  printf(
      "options:\n"
      "  --list             list the scenes and exit\n"
      "  --all              run every scene in order\n"
      "  --port-base N      first port of the private loopback range (default %u)\n"
      "  --frames N         frame count for the socket scenes (default 200, min 3)\n"
      "  --seconds N        wall-clock budget for the whole run (default 240)\n"
      "\n"
      "loopback only, ephemeral/high ports, no privileges, no external files.\n"
      "exit 0 when every case passed, 1 otherwise.\n",
      (unsigned)E2E_PORT_BASE);
}

static const scene *find_scene(const char *name) {
  for (unsigned i = 0; i < k_scene_count; i++)
    if (strcmp(k_scenes[i].name, name) == 0) return &k_scenes[i];
  return NULL;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
    usage();
    return 0;
  }
  if (strcmp(argv[1], "--list") == 0) {
    for (unsigned i = 0; i < k_scene_count; i++) printf("%s\n", k_scenes[i].name);
    return 0;
  }

  const scene *sel = NULL;
  int all = 0;
  unsigned frames = 200u;
  unsigned seconds = 240u;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--all") == 0) {
      all = 1;
    } else if (strcmp(argv[i], "--port-base") == 0 && i + 1 < argc) {
      /* accepted for the matrix; the private range is compiled in, so an
         out-of-range value is reported rather than silently ignored. */
      unsigned long v = strtoul(argv[++i], NULL, 0);
      if (v < 1024ul || v > 65000ul) {
        printf("E2E FAIL: --port-base %lu is out of the unprivileged range\n", v);
        return 2;
      }
      printf("E2E note=port-base compiled_in=%u requested=%lu (the private range is fixed)\n",
             (unsigned)E2E_PORT_BASE, v);
    } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      frames = (unsigned)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      seconds = (unsigned)strtoul(argv[++i], NULL, 0);
    } else if (argv[i][0] == '-') {
      printf("E2E FAIL: unknown option '%s'\n", argv[i]);
      usage();
      return 2;
    } else {
      sel = find_scene(argv[i]);
      if (sel == NULL) {
        printf("E2E FAIL: unknown scene '%s'\n", argv[i]);
        usage();
        return 2;
      }
    }
  }
  if (sel == NULL && !all) {
    printf("E2E FAIL: no scene named and --all not given\n");
    usage();
    return 2;
  }
  if (frames < 3u) frames = 3u;
  if (seconds < 5u) seconds = 5u;
  g_e2e_frames = frames;
  g_e2e_deadline = e2e_now_us() + (uint64_t)seconds * 1000000ull;

  printf("E2E note=cpu-only run on this machine; NOT an MCU result, see docs/cpu_only_e2e.md\n");

  unsigned worst = 0u;
  const unsigned total = all ? k_scene_count : 1u;
  for (unsigned n = 0; n < total; n++) {
    const scene *s = all ? &k_scenes[n] : sel;
    e2e_harness h;
    memset(&h, 0, sizeof(h));
    h.scene = s->name;
    h.t0_us = e2e_now_us();
    int rc = s->run(&h);
    if (rc != 0) worst = 1u;
    if ((h.fails == 0u) != (rc == 0)) {
      printf("E2E scene=%s FAIL harness_rc=%d but fails=%u: the exit code and the case "
             "counters disagree\n",
             s->name, rc, h.fails);
      worst = 1u;
    }
  }
  return worst == 0u ? 0 : 1;
}
