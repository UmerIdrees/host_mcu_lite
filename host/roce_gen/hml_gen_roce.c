/*
 * hml_gen_roce -- RoCEv2 T1a sender CLI (M3 / T1a), the RoCE counterpart of
 * adapters/synthetic/hml_gen.
 *
 * hml_gen can only reach the T0 UDP transport, so the T1a wire format had no
 * standalone driver: T1a was only reachable from tests/m3/test_roce_e2e. This
 * tool closes that gap without adding a second packet builder -- it opens the
 * real hml_rocev2 transport from a hml_dest_cfg and pushes frames through
 * hml_stream, so BTH/RETH/ICRC construction and the 20-byte end-of-frame
 * metadata record live in exactly one place (src/transports/rocev2).
 *
 * Frame content is the deterministic synthetic pattern, identical to what
 * hml_gen feeds the T0 path, so the same frame hash comes out of both tools.
 * Per frame it prints the FNV-1a hash (the value hml_host prints as `hash=` in
 * T0) and the CRC-32/IEEE the frame carries in its metadata record (the value
 * hml_host_roce prints as `crc=` in T1a):
 *
 *   sent frame=0 bytes=9000 chunks=7 hash=0xb6bc71f8 crc=0x1e8d3f77
 *
 * The default port is 14791 rather than the IANA RoCEv2 port 4791 because 4791
 * is below 1024 and needs root or CAP_NET_BIND_SERVICE; production uses 4791.
 */

#include "hml_roce_gen.h"

#include "hml/hml_crc.h"
#include "hml/hml_config.h"
#include "hml/hml_port.h"
#include "hml/hml_protocol.h"
#include "hml/hml_stream.h"
#include "hml/hml_transport.h"
#include "hml_roce_priv.h"
#include "hml_synth_priv.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GEN_DEFAULT_PORT 14791u /* unprivileged mirror of HML_ROCE_PORT (4791) */
#define GEN_DEFAULT_WINDOW_BASE 0x100000ull
#define GEN_DEFAULT_RKEY 0xDEADBEEFu
#define GEN_DEFAULT_WINDOW_4K 16u /* window_len units: 16 x 4 KiB = 64 KiB */
#define GEN_SELFTEST_BYTES 16u

/* ---- first-datagram capture (--verbose), via the existing fault hook ----- */

static uint8_t g_first[HML_ROCE_DG_MAX];
static size_t g_first_len;
static bool g_have_first;

static bool capture_hook(void *ctx, unsigned dg_index, uint8_t *dg, size_t *len) {
  (void)ctx;
  if (dg_index != 0 || *len > sizeof(g_first)) return false;
  memcpy(g_first, dg, *len);
  g_first_len = *len;
  g_have_first = true;
  return false; /* never drop: this is a tap, not a fault */
}

static void print_first_packet(void) {
  if (!g_have_first) {
    fprintf(stderr, "hml_gen_roce: no packet captured\n");
    return;
  }
  const uint8_t *bth = g_first + HML_ROCE_OFF_BTH;
  const uint8_t *reth = g_first + HML_ROCE_OFF_RETH;
  const uint32_t psn = ((uint32_t)bth[HML_ROCE_BTH_PSN] << 16) |
                      ((uint32_t)bth[HML_ROCE_BTH_PSN + 1] << 8) |
                      (uint32_t)bth[HML_ROCE_BTH_PSN + 2];
  printf("pkt0 len=%zu bth0=0x%02x psn=0x%06" PRIx32 " va=0x%08" PRIx64 " rkey=0x%08" PRIx32
         " rlen=%" PRIu32 "\n",
         g_first_len, (unsigned)bth[HML_ROCE_BTH_OPCODE], psn, hml_get_be64(reth),
         hml_get_be32(reth + 8), hml_get_be32(reth + 12));
  fflush(stdout);
}

/* ---- helpers ------------------------------------------------------------ */

static int parse_ipv4(const char *s, uint32_t *out_ne) {
  unsigned a, b, c, d;
  char extra;
  if (!s || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return -1;
  if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
  *out_ne = (a << 24) | (b << 16) | (c << 8) | d; /* already network byte order */
  return 0;
}

static int arg_u64(const char *flag, const char *s, uint64_t lo, uint64_t hi, uint64_t *out) {
  char *end = NULL;
  unsigned long long v = strtoull(s, &end, 0);
  if (!s || !end || *end != '\0' || v < lo || v > hi) {
    fprintf(stderr, "hml_gen_roce: bad value for %s: %s\n", flag, s ? s : "(missing)");
    return -1;
  }
  *out = (uint64_t)v;
  return 0;
}

static void sleep_us(uint64_t us) {
  struct timespec ts;
  ts.tv_sec = (time_t)(us / 1000000ull);
  ts.tv_nsec = (long)((us % 1000000ull) * 1000ull);
  (void)nanosleep(&ts, NULL);
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [--dest-ip A.B.C.D] [--port N] [--src-port N] [--frames N] [--size N]\n"
          "          [--rkey HEX] [--qp N] [--psn N] [--window-base HEX] [--frame-size N]\n"
          "          [--stream-id N] [--seed N] [--rate N] [--verbose] [--fallback-t0]\n"
          "          [--selftest]\n"
          "  --dest-ip      destination IPv4 (default 127.0.0.1)\n"
          "  --port         destination UDP port (default %u; production is %u and\n"
          "                 needs root / CAP_NET_BIND_SERVICE)\n"
          "  --src-port     source UDP port, 0 = ephemeral (default 0)\n"
          "  --frames       frames to send (default 10)\n"
          "  --size         payload bytes per frame (default 9000)\n"
          "  --rkey         RETH remote key (default 0xDEADBEEF)\n"
          "  --qp           RoCEv2 queue pair number (default 1)\n"
          "  --psn          initial RoCEv2 PSN, 24 bits (default 1)\n"
          "  --window-base  remote window base VA, also the first data write address\n"
          "                 (default 0x%x)\n"
          "  --frame-size   bytes per frame; 0 = use --size (default 0). Also places\n"
          "                 the end-of-frame metadata slot at VA base + frame_size\n"
          "  --stream-id    stream id (default 1)\n"
          "  --seed         synthetic pattern seed (default 1)\n"
          "  --rate         frames per second, 0 = as fast as possible (default 0)\n"
          "  --verbose      print the config and the first packet's BTH/PSN/RETH\n"
          "  --fallback-t0  if the RoCEv2 open fails, send as T0 UDP instead\n"
          "  --selftest     build one datagram in-process, print it, exit (no sockets)\n"
          "  --help         this text\n"
          "\n"
          "per frame:   sent frame=<i> bytes=<n> chunks=<k> hash=0x%%08x crc=0x%%08x\n"
          "             hash = FNV-1a  (matches hml_host on T0)\n"
          "             crc  = CRC-32/IEEE (matches hml_host_roce crc= on T1a)\n",
          argv0, (unsigned)GEN_DEFAULT_PORT, (unsigned)HML_ROCE_PORT,
          (unsigned)GEN_DEFAULT_WINDOW_BASE);
}

/*
 * --selftest: no sockets at all. hml_roce_build_datagram() is a pure function,
 * so the first wire datagram can be produced and printed in any sandbox.
 */
static int selftest(const hml_dest_cfg *dest, uint32_t seed) {
  uint8_t pay[HML_ROCE_MAX_PAYLOAD];
  uint8_t dg[HML_ROCE_DG_MAX];
  hml_synth_pattern_fill(pay, sizeof(pay), 0, seed);

  hml_roce_wire_cfg w;
  memset(&w, 0, sizeof(w));
  w.src_ip = (dest->ipv4 == HML_ROCE_LOOPBACK_IP) ? HML_ROCE_LOOPBACK_IP : 0u;
  w.dst_ip = dest->ipv4;
  w.udp_port = dest->udp_port ? dest->udp_port : (uint16_t)HML_ROCE_PORT;
  w.ttl = dest->hop_limit ? dest->hop_limit : (uint8_t)HML_ROCE_DEFAULT_TTL;
  w.ip_id = 0;
  w.psn = dest->psn & 0x00FFFFFFu;
  w.va = dest->remote_addr;
  w.rkey = dest->rkey;

  size_t n = hml_roce_build_datagram(dg, sizeof(dg), &w, pay, sizeof(pay));
  if (n == 0) {
    fprintf(stderr, "hml_gen_roce: selftest build failed\n");
    return 1;
  }
  printf("selftest len=%zu payload=%u first16=", n, (unsigned)sizeof(pay));
  for (unsigned i = 0; i < GEN_SELFTEST_BYTES; i++) printf("%02x", dg[i]);
  printf("\nselftest bth0=0x%02x psn=0x%06" PRIx32 " va=0x%08" PRIx64 " rkey=0x%08" PRIx32
         " icrc_le=0x%02x%02x%02x%02x\n",
         (unsigned)dg[HML_ROCE_OFF_BTH + HML_ROCE_BTH_OPCODE],
         ((uint32_t)dg[HML_ROCE_OFF_BTH + HML_ROCE_BTH_PSN] << 16) |
             ((uint32_t)dg[HML_ROCE_OFF_BTH + HML_ROCE_BTH_PSN + 1] << 8) |
             (uint32_t)dg[HML_ROCE_OFF_BTH + HML_ROCE_BTH_PSN + 2],
         hml_get_be64(dg + HML_ROCE_OFF_RETH), hml_get_be32(dg + HML_ROCE_OFF_RETH + 8),
         dg[n - 4], dg[n - 3], dg[n - 2], dg[n - 1]);
  fflush(stdout);
  return 0;
}

int main(int argc, char **argv) {
  const char *dest_ip = "127.0.0.1";
  uint16_t port = (uint16_t)GEN_DEFAULT_PORT;
  uint16_t src_port = 0;
  uint64_t nframes = 10;
  size_t size = 9000;
  uint64_t rkey = GEN_DEFAULT_RKEY;
  uint64_t qp = 1;
  uint64_t psn = 1;
  uint64_t window_base = GEN_DEFAULT_WINDOW_BASE;
  uint64_t frame_size_opt = 0;
  uint32_t stream_id = 1;
  uint32_t seed = 1;
  uint32_t rate = 0;
  int verbose = 0;
  int fallback_t0 = 0;
  int selftest_only = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (strcmp(a, "--dest-ip") == 0 && v) {
      dest_ip = argv[++i];
    } else if (strcmp(a, "--verbose") == 0) {
      verbose = 1;
    } else if (strcmp(a, "--fallback-t0") == 0) {
      fallback_t0 = 1;
    } else if (strcmp(a, "--selftest") == 0) {
      selftest_only = 1;
    } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(argv[0]);
      return 0;
    } else if (!v) {
      fprintf(stderr, "hml_gen_roce: %s needs a value\n", a);
      return 2;
    } else if (strcmp(a, "--port") == 0) {
      uint64_t t = 0;
      if (arg_u64(a, argv[++i], 1, 65535, &t) != 0) return 2;
      port = (uint16_t)t;
    } else if (strcmp(a, "--src-port") == 0) {
      uint64_t t = 0;
      if (arg_u64(a, argv[++i], 0, 65535, &t) != 0) return 2;
      src_port = (uint16_t)t;
    } else if (strcmp(a, "--frames") == 0) {
      if (arg_u64(a, argv[++i], 0, 1000000, &nframes) != 0) return 2;
    } else if (strcmp(a, "--size") == 0) {
      uint64_t t = 0;
      if (arg_u64(a, argv[++i], 1, (uint64_t)HML_MAX_FRAME_SIZE, &t) != 0) return 2;
      size = (size_t)t;
    } else if (strcmp(a, "--rkey") == 0) {
      if (arg_u64(a, argv[++i], 0, 0xFFFFFFFFull, &rkey) != 0) return 2;
    } else if (strcmp(a, "--qp") == 0) {
      if (arg_u64(a, argv[++i], 0, 0xFFFFFFFFull, &qp) != 0) return 2;
    } else if (strcmp(a, "--psn") == 0) {
      if (arg_u64(a, argv[++i], 0, 0xFFFFFFull, &psn) != 0) return 2;
    } else if (strcmp(a, "--window-base") == 0) {
      if (arg_u64(a, argv[++i], 0, UINT64_MAX, &window_base) != 0) return 2;
    } else if (strcmp(a, "--frame-size") == 0) {
      if (arg_u64(a, argv[++i], 0, (uint64_t)HML_MAX_FRAME_SIZE, &frame_size_opt) != 0) return 2;
    } else if (strcmp(a, "--stream-id") == 0) {
      uint64_t t = 0;
      if (arg_u64(a, argv[++i], 0, 0xFFFF, &t) != 0) return 2;
      stream_id = (uint32_t)t;
    } else if (strcmp(a, "--seed") == 0) {
      uint64_t t = 0;
      if (arg_u64(a, argv[++i], 0, 0xFFFFFFFFull, &t) != 0) return 2;
      seed = (uint32_t)t;
    } else if (strcmp(a, "--rate") == 0) {
      uint64_t t = 0;
      if (arg_u64(a, argv[++i], 0, 1000000, &t) != 0) return 2;
      rate = (uint32_t)t;
    } else {
      fprintf(stderr, "hml_gen_roce: unknown argument '%s'\n", a);
      usage(argv[0]);
      return 2;
    }
  }

  if (frame_size_opt == 0) frame_size_opt = size;
  if (frame_size_opt < size) {
    fprintf(stderr, "hml_gen_roce: --frame-size %" PRIu64 " is smaller than --size %zu\n",
            frame_size_opt, size);
    return 2;
  }

  hml_dest_cfg dest;
  memset(&dest, 0, sizeof(dest));
  dest.kind = HML_XPORT_ROCEV2;
  if (parse_ipv4(dest_ip, &dest.ipv4) != 0) {
    fprintf(stderr, "hml_gen_roce: bad --dest-ip '%s'\n", dest_ip);
    return 2;
  }
  dest.udp_port = port;
  dest.qp = (uint32_t)qp;
  dest.psn = (uint32_t)psn;
  dest.rkey = (uint32_t)rkey;
  dest.remote_addr = window_base;
  dest.frame_size = (uint32_t)frame_size_opt;
  dest.window_len = GEN_DEFAULT_WINDOW_4K;
  dest.hop_limit = 64;

  if (selftest_only) return selftest(&dest, seed);

  hml_roce_gen_xport gx;
  hml_status st = hml_roce_gen_xport_open(&gx, &dest, (uint16_t)stream_id, src_port, fallback_t0);
  if (st != HML_OK) {
    fprintf(stderr, "hml_gen_roce: transport open failed (%d)\n", (int)st);
    return 1;
  }
  if (gx.fallback) {
    printf("fallback: roce open failed, using T0 (udp) status=%d\n", (int)gx.roce_status);
    fflush(stdout);
  }

  hml_stream_cfg scfg;
  memset(&scfg, 0, sizeof(scfg));
  scfg.stream_id = (uint16_t)stream_id;
  scfg.max_frame_bytes = size;
  scfg.seq_start = 0;
  scfg.mtu_effective = (size_t)HML_DEFAULT_MTU;
  hml_stream_t *stream = NULL;
  st = hml_stream_open(&stream, &scfg, &gx.xport);
  if (st != HML_OK) {
    fprintf(stderr, "hml_gen_roce: stream open failed (%d)\n", (int)st);
    hml_roce_gen_xport_close(&gx);
    return 1;
  }

  if (verbose) {
    printf("gen xport=%s dest=%s:%u qp=%" PRIu32 " psn=%" PRIu32 " rkey=0x%08" PRIx32
           " va=0x%08" PRIx64 " frame_size=%" PRIu32 " window=%" PRIu32 "KiB stream=%" PRIu32
           " frames=%" PRIu64 " size=%zu seed=%" PRIu32 "\n",
           gx.ops->name, dest_ip, (unsigned)port, dest.qp, dest.psn, dest.rkey, dest.remote_addr,
           dest.frame_size, (uint32_t)dest.window_len * 4u, (uint32_t)stream_id, nframes, size,
           seed);
    fflush(stdout);
  }
  /* The T1a wire header is only observable on the RoCEv2 transport, so the tap
   * is armed for that one; a T0 fallback run has no BTH/RETH to report. */
  if (verbose && !gx.fallback) hml_roce_set_fault_hook(capture_hook, NULL);

  uint8_t *frame = (uint8_t *)malloc(size);
  if (!frame) {
    fprintf(stderr, "hml_gen_roce: out of memory\n");
    hml_stream_close(stream);
    hml_roce_gen_xport_close(&gx);
    return 1;
  }

  uint64_t sent_frames = 0;
  uint64_t sent_bytes = 0;
  uint64_t sent_dgs = 0;
  int rc = 0;
  for (uint64_t f = 0; f < nframes; f++) {
    hml_roce_gen_frame_fill(frame, size, f, seed);
    size_t chunks = 0;
    st = hml_roce_gen_frame_send(stream, frame, size, &chunks);
    if (st != HML_OK) {
      fprintf(stderr, "hml_gen_roce: send failed on frame %" PRIu64 " (%d)\n", f, (int)st);
      rc = 1;
      break;
    }
    /* One metadata write per frame closes it; the T0 path closes on the END
     * fragment, so both transports cost chunks + 1 datagram per frame. */
    sent_dgs += chunks + 1u;
    sent_bytes += size;
    sent_frames++;
    printf("sent frame=%" PRIu64 " bytes=%zu chunks=%zu hash=0x%08x crc=0x%08x\n", f, size, chunks,
           hml_fnv1a32(frame, size), hml_crc32_ieee(frame, size));
    fflush(stdout);
    if (rate > 0 && f + 1 < nframes) sleep_us(1000000ull / rate);
    if (verbose && !gx.fallback && f == 0) hml_roce_set_fault_hook(NULL, NULL);
  }

  /* Safety net: the metadata write is already out, this only closes a frame
   * whose END fragment never made it. */
  st = gx.ops->flush(&gx.xport);
  if (st != HML_OK && rc == 0) {
    fprintf(stderr, "hml_gen_roce: flush failed (%d)\n", (int)st);
    rc = 1;
  }

  const uint32_t psn_first = (uint32_t)psn & 0x00FFFFFFu;
  const uint32_t psn_last =
      sent_dgs ? (uint32_t)((psn_first + sent_dgs - 1u) & 0x00FFFFFFu) : psn_first;
  printf("sent frames=%" PRIu64 " bytes=%" PRIu64 " psn_first=%" PRIu32 " psn_last=%" PRIu32 "\n",
         sent_frames, sent_bytes, psn_first, psn_last);
  if (verbose && !gx.fallback) print_first_packet();
  fflush(stdout);

  free(frame);
  hml_stream_close(stream); /* closes the transport too */
  hml_roce_gen_xport_close(&gx); /* no-op then; keeps the contract explicit */
  return rc;
}
