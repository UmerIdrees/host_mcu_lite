#include "hml_host.h"

#include "hml/hml_protocol.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t g_stop = 0;
static int g_verbose = 0;
static int g_show_latency = 0;
static uint64_t g_budget_us = 0; /* 0 = unset */
static FILE *g_dump_fp = NULL;

static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

static void on_data(void *ctx, uint16_t stream_id, uint32_t frame_seq,
                    const uint8_t *frame_data, size_t frame_bytes) {
  (void)ctx;
  (void)stream_id;
  (void)frame_seq;
  if (g_dump_fp && frame_data && frame_bytes > 0) {
    fwrite(frame_data, 1, frame_bytes, g_dump_fp);
    fflush(g_dump_fp);
  }
}

static void on_frame(void *ctx, uint16_t stream_id, uint32_t frame_seq, size_t frame_bytes,
                     uint32_t hash) {
  (void)ctx;
  if (!g_verbose) return;
  printf("frame seq=%u bytes=%u hash=0x%08x\n", (unsigned)frame_seq, (unsigned)frame_bytes,
         (unsigned)hash);
  fflush(stdout);
  (void)stream_id;
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [--port N] [--frames N] [--verbose] [--stats] [--max-frame BYTES]\n"
          "            [--latency] [--budget-us N] [--dump FILE]\n"
          "  --port N       UDP data port to bind (default %u)\n"
          "  --frames N     stop after N completed frames (default 0 = forever)\n"
          "  --verbose      print one line per completed frame\n"
          "  --stats        print a stats line every second while running\n"
          "  --max-frame N  reassembly buffer (default %u)\n"
          "  --latency      print the EOF latency histogram on every stats line\n"
          "  --budget-us N  p99 EOF-latency budget for the exit verdict (default unset)\n"
          "  --dump FILE    write assembled frame payloads to FILE\n",
          argv0, (unsigned)HML_DATA_PORT, (unsigned)HML_MAX_FRAME_SIZE);
}

static void print_latency(const hml_host_t *h, uint64_t budget_us) {
  hml_latency lat;
  if (hml_host_latency(h, &lat) != HML_OK) return;
  uint64_t p50 = 0, p95 = 0, p99 = 0;
  const char *p50s = "n/a", *p95s = "n/a", *p99s = "n/a", *mins = "n/a", *maxs = "n/a";
  char p50b[32], p95b[32], p99b[32], minb[32], maxb[32];
  if (lat.n > 0u) {
    if (hml_latency_percentile(&lat, 50, &p50) == HML_OK) p50s = p50b;
    if (hml_latency_percentile(&lat, 95, &p95) == HML_OK) p95s = p95b;
    if (hml_latency_percentile(&lat, 99, &p99) == HML_OK) p99s = p99b;
    snprintf(p50b, sizeof(p50b), "%llu", (unsigned long long)p50);
    snprintf(p95b, sizeof(p95b), "%llu", (unsigned long long)p95);
    snprintf(p99b, sizeof(p99b), "%llu", (unsigned long long)p99);
    snprintf(minb, sizeof(minb), "%llu", (unsigned long long)lat.min_us);
    snprintf(maxb, sizeof(maxb), "%llu", (unsigned long long)lat.max_us);
    mins = minb;
    maxs = maxb;
  }
  printf("  latency p50=%sus p95=%sus p99=%sus min=%sus max=%sus n=%llu skew=%u overflow=%u bucket=%uus",
         p50s, p95s, p99s, mins, maxs, (unsigned long long)lat.n,
         (unsigned)lat.skew_samples, (unsigned)lat.overflow, (unsigned)lat.bucket_us);
  if (budget_us != 0u) {
    printf(" budget=%lluus", (unsigned long long)budget_us);
  } else {
    printf(" budget=unset");
  }
  printf("\n");
}

static const char *verdict_str(uint64_t n, uint32_t skew, uint64_t budget_us, bool ok) {
  if (n == 0u || budget_us == 0u || skew != 0u) return "TBD";
  return ok ? "PASS" : "FAIL";
}

static void print_latency_verdict(const hml_host_t *h, uint64_t budget_us) {
  hml_latency lat;
  if (hml_host_latency(h, &lat) != HML_OK) {
    printf("latency p50=n/a p95=n/a p99=n/a max=n/a n=0 skew=0 budget=unset verdict=TBD\n");
    return;
  }
  uint64_t p99 = 0;
  bool ok = hml_host_latency_ok(h, budget_us, &p99);
  const char *sk = (lat.skew_samples == 0u) ? "skew=0" : "skew>0";
  printf("latency ");
  if (lat.n > 0u) {
    uint64_t p50 = 0, p95 = 0;
    (void)hml_latency_percentile(&lat, 50, &p50);
    (void)hml_latency_percentile(&lat, 95, &p95);
    printf("p50=%lluus p95=%lluus p99=%lluus max=%lluus n=%llu %s budget=",
           (unsigned long long)p50, (unsigned long long)p95, (unsigned long long)p99,
           (unsigned long long)lat.max_us, (unsigned long long)lat.n, sk);
  } else {
    printf("p50=n/a p95=n/a p99=n/a max=n/a n=0 %s budget=", sk);
  }
  if (budget_us != 0u) {
    printf("%lluus", (unsigned long long)budget_us);
  } else {
    printf("unset");
  }
  printf(" verdict=%s\n", verdict_str(lat.n, lat.skew_samples, budget_us, ok));
}

static void print_summary(const hml_host_t *h) {
  hml_reasm_stats rs;
  uint64_t frames = 0, bytes = 0, eof = 0, hash = 0;
  hml_host_stats(h, &rs, &frames, &bytes, &eof, &hash);
  printf("summary frames=%llu bytes=%llu eof=%llu last_hash=0x%08llx\n", (unsigned long long)frames,
         (unsigned long long)bytes, (unsigned long long)eof, (unsigned long long)hash);
  printf("reasm frames_ok=%u frames_dropped=%u frags_recv=%u gaps=%u crc_fail=%u proto_fail=%u\n",
         (unsigned)rs.frames_ok, (unsigned)rs.frames_dropped, (unsigned)rs.frags_recv,
         (unsigned)rs.gaps, (unsigned)rs.crc_fail, (unsigned)rs.proto_fail);
  if (g_show_latency) print_latency(h, g_budget_us);
  fflush(stdout);
}

int main(int argc, char **argv) {
  uint16_t port = (uint16_t)HML_DATA_PORT;
  uint64_t want_frames = 0;
  size_t max_frame = (size_t)HML_MAX_FRAME_SIZE;
  int periodic = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--port") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 0);
      if (v <= 0 || v > 65535) {
        fprintf(stderr, "hml_host: bad --port\n");
        return 2;
      }
      port = (uint16_t)v;
    } else if (strcmp(a, "--frames") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 0);
      if (v < 0) {
        fprintf(stderr, "hml_host: bad --frames\n");
        return 2;
      }
      want_frames = (uint64_t)v;
    } else if (strcmp(a, "--max-frame") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 0);
      if (v <= 0) {
        fprintf(stderr, "hml_host: bad --max-frame\n");
        return 2;
      }
      max_frame = (size_t)v;
    } else if (strcmp(a, "--verbose") == 0) {
      g_verbose = 1;
    } else if (strcmp(a, "--stats") == 0) {
      periodic = 1;
    } else if (strcmp(a, "--latency") == 0) {
      g_show_latency = 1;
    } else if (strcmp(a, "--budget-us") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 0);
      if (v < 0) {
        fprintf(stderr, "hml_host: bad --budget-us\n");
        return 2;
      }
      g_budget_us = (uint64_t)v;
    } else if (strcmp(a, "--dump") == 0 && i + 1 < argc) {
      const char *path = argv[++i];
      g_dump_fp = fopen(path, "wb");
      if (!g_dump_fp) {
        fprintf(stderr, "hml_host: cannot open dump file '%s'\n", path);
        return 2;
      }
    } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "hml_host: unknown argument '%s'\n", a);
      usage(argv[0]);
      return 2;
    }
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  (void)sigaction(SIGINT, &sa, NULL);
  (void)sigaction(SIGTERM, &sa, NULL);

  hml_host_t *h = NULL;
  hml_status st = hml_host_open(&h, port, max_frame);
  if (st != HML_OK) {
    if (g_dump_fp) fclose(g_dump_fp);
    fprintf(stderr, "hml_host: open failed (status %d)\n", (int)st);
    return 1;
  }
  (void)hml_host_set_frame_cb(h, on_frame, NULL);
  if (g_dump_fp) (void)hml_host_set_data_cb(h, on_data, NULL);

  uint64_t last_report = hml_clock_us();
  int rc = 0;
  for (;;) {
    if (g_stop) break;
    st = hml_host_poll(h, 200);
    if (st == HML_ERR_IO) {
      fprintf(stderr, "hml_host: socket error\n");
      rc = 1;
      break;
    }
    if (want_frames != 0) {
      uint64_t frames = 0;
      hml_host_stats(h, NULL, &frames, NULL, NULL, NULL);
      if (frames >= want_frames) break;
    }
    if (periodic) {
      uint64_t now = hml_clock_us();
      if (now - last_report >= 1000000ull) {
        last_report = now;
        print_summary(h);
      }
    }
  }

  print_summary(h);
  print_latency_verdict(h, g_budget_us);
  if (g_dump_fp) fclose(g_dump_fp);
  hml_host_close(h);
  return rc;
}
