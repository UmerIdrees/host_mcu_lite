/*
 * hml_host_roce -- host-side RoCEv2 receiver CLI (M3 / T1a).
 *
 * Binds the RoCEv2 UDP port, demultiplexes socket-crafted UC RDMA WRITEs into
 * the simulated remote window, and prints one line per completed frame. The
 * default port is 14791 rather than the IANA RoCEv2 port 4791 because 4791 is
 * privileged on Linux (< 1024) and needs root or CAP_NET_BIND_SERVICE.
 *
 *   hml_host_roce [--port N] [--window-base HEX] [--window-len N] [--frame-size N]
 *                 [--rkey HEX] [--frames N] [--no-icrc] [--verbose]
 */

#include "hml_roce_demux.h"

#include "hml/hml_crc.h"
#include "hml/hml_port.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [--port N] [--window-base HEX] [--window-len N] [--frame-size N]\n"
          "          [--rkey HEX] [--frames N] [--no-icrc] [--verbose]\n",
          argv0);
}

typedef struct host_ctx {
  hml_roce_rx_t *rx;
  uint32_t frame_size;
  int verbose;
  uint64_t seen;
  uint64_t bytes;
  uint64_t bad_len;
  uint64_t bad_crc;
} host_ctx;

static void on_frame(void *vctx, uint32_t frame_seq, const uint8_t *frame, size_t n,
                     uint32_t crc32) {
  host_ctx *hc = (host_ctx *)vctx;
  hc->seen++;
  hc->bytes += n;
  if (n != hc->frame_size) hc->bad_len++;
  if (crc32 != hml_crc32_ieee(frame, n)) hc->bad_crc++;
  if (hc->verbose) {
    printf("frame seq=%u bytes=%zu crc=0x%08x\n", frame_seq, n, crc32);
    fflush(stdout);
  }
}

static int arg_u64(const char *s, uint64_t *out) {
  char *end = NULL;
  errno = 0;
  unsigned long long v = strtoull(s, &end, 0);
  if (errno != 0 || !end || *end != '\0') return -1;
  *out = (uint64_t)v;
  return 0;
}

int main(int argc, char **argv) {
  hml_roce_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.udp_port = 14791; /* unprivileged mirror of HML_ROCE_PORT (4791) */
  cfg.window_base = 0x100000ull;
  cfg.window_len_bytes = 65536;
  cfg.frame_size = 9000;
  cfg.rkey = 0xDEADBEEFu;
  cfg.verify_icrc = true;
  host_ctx hc;
  memset(&hc, 0, sizeof(hc));
  hc.frame_size = cfg.frame_size;
  uint64_t want_frames = 0; /* 0 = forever */

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--verbose") == 0) {
      hc.verbose = 1;
      continue;
    }
    if (strcmp(a, "--no-icrc") == 0) {
      cfg.verify_icrc = false;
      continue;
    }
    if (i + 1 >= argc) {
      usage(argv[0]);
      return 2;
    }
    uint64_t v = 0;
    if (arg_u64(argv[++i], &v) != 0) {
      fprintf(stderr, "hml_host_roce: bad value for %s: %s\n", a, argv[i]);
      return 2;
    }
    if (strcmp(a, "--port") == 0) {
      cfg.udp_port = (uint16_t)v;
    } else if (strcmp(a, "--window-base") == 0) {
      cfg.window_base = v;
    } else if (strcmp(a, "--window-len") == 0) {
      cfg.window_len_bytes = (uint32_t)v;
    } else if (strcmp(a, "--frame-size") == 0) {
      cfg.frame_size = (uint32_t)v;
    } else if (strcmp(a, "--rkey") == 0) {
      cfg.rkey = (uint32_t)v;
    } else if (strcmp(a, "--frames") == 0) {
      want_frames = v;
    } else {
      fprintf(stderr, "hml_host_roce: unknown option %s\n", a);
      usage(argv[0]);
      return 2;
    }
    hc.frame_size = cfg.frame_size;
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  (void)sigaction(SIGINT, &sa, NULL);
  (void)sigaction(SIGTERM, &sa, NULL);

  hml_status st = hml_roce_rx_open(&hc.rx, &cfg, on_frame, &hc);
  if (st != HML_OK) {
    fprintf(stderr,
            "hml_host_roce: open udp/%u failed: %s\n", (unsigned)cfg.udp_port,
            st == HML_ERR_IO ? "port busy or bind not permitted" : "invalid config");
    return 1;
  }
  fprintf(stderr,
          "hml_host_roce: listening udp/%u window=0x%" PRIx64 " len=%u frame=%u rkey=0x%08x icrc=%s\n",
          (unsigned)cfg.udp_port, cfg.window_base, cfg.window_len_bytes, cfg.frame_size, cfg.rkey,
          cfg.verify_icrc ? "on" : "off");

  while (!g_stop) {
    st = hml_roce_rx_poll(hc.rx, 200);
    if (want_frames && hc.seen >= want_frames) break;
    if (st != HML_OK && st != HML_ERR_AGAIN && st != HML_ERR_TIMEOUT) {
      fprintf(stderr, "hml_host_roce: poll error %d\n", (int)st);
      break;
    }
  }

  hml_roce_stats s;
  hml_roce_rx_stats(hc.rx, &s);
  printf("stats pkts=%" PRIu64 " icrc_err=%" PRIu64 " writes=%" PRIu64 " bytes=%" PRIu64
         " eof_frames=%" PRIu64 " psn_gaps=%" PRIu64 " proto_err=%" PRIu64
         " meta_drops=%" PRIu64 "\n",
         s.pkts, s.icrc_err, s.writes, s.bytes, s.eof_frames, s.psn_gaps, s.proto_err,
         s.meta_drops);
  printf("host frames=%" PRIu64 " bytes=%" PRIu64 " bad_len=%" PRIu64 " bad_crc=%" PRIu64 "\n",
         hc.seen, hc.bytes, hc.bad_len, hc.bad_crc);
  fflush(stdout);

  hml_roce_rx_close(hc.rx);
  return (hc.bad_crc || hc.bad_len) ? 1 : 0;
}
