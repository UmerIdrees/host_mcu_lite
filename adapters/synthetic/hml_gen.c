#include "hml/hml_port.h"
#include "hml/hml_config.h"
#include "hml/hml_crc.h"
#include "hml/hml_protocol.h"
#include "hml/hml_sock.h"
#include "hml/hml_stream.h"
#include "hml/hml_transport.h"

#include "hml_roce_priv.h"
#include "hml_udp_priv.h"
#include "hml_synth_priv.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * T0 (udp) and T1a (rocev2) are both reachable from this one CLI. The datagram
 * shaping, the BTH/RETH/ICRC bytes and the 20-byte end-of-frame metadata record
 * stay in src/transports/rocev2; --xport only picks which hml_xport_ops the
 * registry hands back, and the framing stays in hml_stream either way.
 */
#define GEN_DEFAULT_ROCE_PORT 14791u /* unprivileged mirror of HML_ROCE_PORT (4791) */
#define GEN_DEFAULT_RKEY 0xDEADBEEFu
#define GEN_DEFAULT_WINDOW_BASE 0x100000ull
#define GEN_DEFAULT_WINDOW_4K 16u /* window_len units: 16 x 4 KiB = 64 KiB */

/*
 * Synthetic frame generator (device side). One line per frame on stdout so a test
 * harness can diff it against hml_host --verbose output:
 *   sent frame=<i> bytes=<n> hash=0x%08x
 */

static int parse_ipv4(const char *s, uint32_t *out_ne) {
  unsigned a, b, c, d;
  char extra;
  if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return -1;
  if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
  *out_ne = (a << 24) | (b << 16) | (c << 8) | d; /* already network byte order */
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
          "usage: %s [--dest-ip A.B.C.D] [--port N] [--frames N] [--size N] [--rate N]\n"
          "          [--seed N] [--xport NAME] [--stream-id N] [--seq-start N] [--eof-only]\n"
          "          [--rkey HEX] [--window-base HEX] [--qp N] [--psn N] [--frame-size N]\n"
          "  --dest-ip   destination IPv4 (default 127.0.0.1)\n"
          "  --port      destination UDP port (default %u for udp, %u for rocev2)\n"
          "  --frames    frames to send (default 10)\n"
          "  --size      payload bytes per frame (default 9000)\n"
          "  --rate      frames per second, 0 = as fast as possible (default 0)\n"
          "  --seed      synthetic pattern seed (default 1)\n"
          "  --xport     transport name from the registry: udp (T0) or rocev2 (T1a)\n"
          "  --eof-only  close frames with a zero-payload EOF-only flush\n"
          "  --rkey      rocev2 RETH remote key (default 0x%08" PRIX32 ")\n"
          "  --window-base  rocev2 remote window base VA, the first data write address\n"
          "                (default 0x%x)\n"
          "  --qp        rocev2 queue pair number (default 1)\n"
          "  --psn       rocev2 initial PSN, 24 bits (default 1)\n"
          "  --frame-size  rocev2 bytes per frame; 0 = use --size (default 0). Also places\n"
          "                the end-of-frame metadata slot at VA base + frame_size\n",
          argv0, (unsigned)HML_DATA_PORT, (unsigned)GEN_DEFAULT_ROCE_PORT, (uint32_t)GEN_DEFAULT_RKEY,
          (unsigned)GEN_DEFAULT_WINDOW_BASE);
}

int main(int argc, char **argv) {
  const char *dest_ip = "127.0.0.1";
  const char *xport_name = "udp";
  uint16_t port = (uint16_t)HML_DATA_PORT;
  int port_given = 0;
  uint64_t nframes = 10;
  size_t size = 9000;
  uint32_t rate = 0;
  uint32_t seed = 1;
  uint32_t stream_id = 1;
  uint32_t seq_start = 0;
  uint64_t rkey = GEN_DEFAULT_RKEY;
  uint64_t window_base = GEN_DEFAULT_WINDOW_BASE;
  uint64_t qp = 1;
  uint64_t psn = 1;
  uint64_t frame_size_opt = 0;
  int eof_only = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--dest-ip") == 0 && i + 1 < argc) {
      dest_ip = argv[++i];
    } else if (strcmp(a, "--port") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 0);
      if (v <= 0 || v > 65535) goto bad_arg;
      port = (uint16_t)v;
      port_given = 1;
    } else if (strcmp(a, "--frames") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 0);
      if (v < 0) goto bad_arg;
      nframes = (uint64_t)v;
    } else if (strcmp(a, "--size") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 0);
      if (v <= 0 || (size_t)v > (size_t)HML_MAX_FRAME_SIZE) goto bad_arg;
      size = (size_t)v;
    } else if (strcmp(a, "--rate") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 0);
      if (v < 0) goto bad_arg;
      rate = (uint32_t)v;
    } else if (strcmp(a, "--seed") == 0 && i + 1 < argc) {
      seed = (uint32_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--xport") == 0 && i + 1 < argc) {
      xport_name = argv[++i];
    } else if (strcmp(a, "--stream-id") == 0 && i + 1 < argc) {
      stream_id = (uint16_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--seq-start") == 0 && i + 1 < argc) {
      seq_start = (uint32_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--rkey") == 0 && i + 1 < argc) {
      rkey = strtoull(argv[++i], NULL, 0);
    } else if (strcmp(a, "--window-base") == 0 && i + 1 < argc) {
      window_base = strtoull(argv[++i], NULL, 0);
    } else if (strcmp(a, "--qp") == 0 && i + 1 < argc) {
      qp = strtoull(argv[++i], NULL, 0);
    } else if (strcmp(a, "--psn") == 0 && i + 1 < argc) {
      psn = strtoull(argv[++i], NULL, 0) & 0xFFFFFFull;
    } else if (strcmp(a, "--frame-size") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 0);
      if (v < 0 || (size_t)v > (size_t)HML_MAX_FRAME_SIZE) goto bad_arg;
      frame_size_opt = (uint64_t)v;
    } else if (strcmp(a, "--eof-only") == 0) {
      eof_only = 1;
    } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "hml_gen: unknown argument '%s'\n", a);
      usage(argv[0]);
      return 2;
    }
    continue;
  bad_arg:
    fprintf(stderr, "hml_gen: bad value for %s\n", a);
    return 2;
  }

  const int is_roce = (strcmp(xport_name, "rocev2") == 0);
  if (is_roce && !port_given) port = (uint16_t)GEN_DEFAULT_ROCE_PORT;
  if (is_roce && eof_only) {
    /* An EOF-only flush emits a zero-payload fragment, which the RoCEv2
     * transport turns into a metadata write with no data behind it. Allowed,
     * but the frame the metadata describes is empty, so say so rather than
     * letting it look like a completed frame. */
    fprintf(stderr, "hml_gen: --eof-only closes an empty rocev2 frame; frame_size carries no data\n");
  }

  /* sensor */
  hml_synth_cfg scfg;
  scfg.seed = seed;
  scfg.frame_bytes = (size_t)HML_MAX_FRAG_PAYLOAD;
  hml_sensor_t sensor;
  memset(&sensor, 0, sizeof(sensor));
  sensor.ops = hml_synth_ops();
  hml_status st = sensor.ops->open(&sensor, &scfg);
  if (st != HML_OK) {
    fprintf(stderr, "hml_gen: sensor open failed (%d)\n", (int)st);
    return 1;
  }

  /* transport */
  (void)hml_udp_register();
  (void)hml_rocev2_register();
  const hml_xport_ops *ops = hml_transport_lookup(xport_name);
  if (!ops) {
    fprintf(stderr, "hml_gen: unknown transport '%s'\n", xport_name);
    sensor.ops->close(&sensor);
    return 1;
  }
  hml_dest_cfg dest;
  memset(&dest, 0, sizeof(dest));
  dest.kind = is_roce ? HML_XPORT_ROCEV2 : HML_XPORT_UDP;
  if (parse_ipv4(dest_ip, &dest.ipv4) != 0) {
    fprintf(stderr, "hml_gen: bad --dest-ip '%s'\n", dest_ip);
    sensor.ops->close(&sensor);
    return 2;
  }
  dest.udp_port = port;
  if (frame_size_opt == 0) frame_size_opt = (uint64_t)size;
  if (frame_size_opt < (uint64_t)size) {
    fprintf(stderr, "hml_gen: --frame-size %" PRIu64 " is smaller than --size %zu\n", frame_size_opt,
            size);
    sensor.ops->close(&sensor);
    return 2;
  }
  dest.frame_size = (uint32_t)frame_size_opt;
  dest.hop_limit = 64;
  if (is_roce) {
    /* The window, rkey, VA base and PSN are what the receiver's demux uses to
     * place a write and to find the end-of-frame metadata slot; the defaults
     * match hml_host_roce so the two interoperate with no flags at all. */
    dest.qp = (uint32_t)qp;
    dest.psn = (uint32_t)psn;
    dest.rkey = (uint32_t)rkey;
    dest.remote_addr = window_base;
    dest.window_len = (uint16_t)GEN_DEFAULT_WINDOW_4K;
  }

  hml_xport_t xport;
  memset(&xport, 0, sizeof(xport));
  xport.ops = ops;
  st = ops->open(&xport, &dest, (uint16_t)stream_id);
  if (st != HML_OK) {
    fprintf(stderr, "hml_gen: transport open failed (%d)\n", (int)st);
    sensor.ops->close(&sensor);
    return 1;
  }

  hml_stream_cfg stream_cfg;
  stream_cfg.stream_id = (uint16_t)stream_id;
  stream_cfg.max_frame_bytes = size;
  stream_cfg.seq_start = seq_start;
  stream_cfg.mtu_effective = (size_t)HML_DEFAULT_MTU;
  hml_stream_t *stream = NULL;
  st = hml_stream_open(&stream, &stream_cfg, &xport);
  if (st != HML_OK) {
    fprintf(stderr, "hml_gen: stream open failed (%d)\n", (int)st);
    ops->close(&xport);
    sensor.ops->close(&sensor);
    return 1;
  }

  uint8_t *fbuf = (uint8_t *)malloc(size);
  if (!fbuf) {
    fprintf(stderr, "hml_gen: out of memory\n");
    hml_stream_close(stream);
    sensor.ops->close(&sensor);
    return 1;
  }

  int rc = 0;
  uint64_t sent_frames = 0;
  for (uint64_t f = 0; f < nframes; f++) {
    size_t off = 0;
    int first = 1;
    while (off < size) {
      size_t want = size - off;
      if (want > (size_t)HML_MAX_FRAG_PAYLOAD) want = (size_t)HML_MAX_FRAG_PAYLOAD;
      size_t got = 0;
      st = sensor.ops->acquire(&sensor, fbuf + off, want, &got);
      if (st != HML_OK || got == 0) {
        fprintf(stderr, "hml_gen: acquire failed (%d)\n", (int)st);
        rc = 1;
        goto done;
      }
      int last = (!eof_only && (off + got == size));
      for (;;) {
        st = hml_stream_send_frag(stream, fbuf + off, got, first != 0, last != 0, hml_clock_us());
        if (st == HML_ERR_AGAIN) {
          sleep_us(200);
          continue;
        }
        break;
      }
      if (st != HML_OK) {
        fprintf(stderr, "hml_gen: send_frag failed (%d)\n", (int)st);
        rc = 1;
        goto done;
      }
      off += got;
      first = 0;
      st = sensor.ops->release(&sensor);
      if (st != HML_OK) {
        fprintf(stderr, "hml_gen: release failed (%d)\n", (int)st);
        rc = 1;
        goto done;
      }
    }
    if (eof_only) {
      for (;;) {
        st = hml_stream_flush_eof(stream, hml_clock_us());
        if (st == HML_ERR_AGAIN) {
          sleep_us(200);
          continue;
        }
        break;
      }
      if (st != HML_OK) {
        fprintf(stderr, "hml_gen: flush_eof failed (%d)\n", (int)st);
        rc = 1;
        goto done;
      }
    }

    if (is_roce) {
      /* T1a closes a frame with the metadata write, not a flag, so report the
       * CRC-32 the metadata record carries -- the value hml_host_roce prints as
       * crc= -- next to the FNV-1a the T0 receiver prints as hash=. */
      printf("sent frame=%llu bytes=%llu hash=0x%08x crc=0x%08x\n", (unsigned long long)f,
             (unsigned long long)size, (unsigned)hml_fnv1a32(fbuf, size),
             (unsigned)hml_crc32_ieee(fbuf, size));
    } else {
      printf("sent frame=%llu bytes=%llu hash=0x%08x\n", (unsigned long long)f,
             (unsigned long long)size, (unsigned)hml_fnv1a32(fbuf, size));
    }
    fflush(stdout);
    sent_frames++;
    if (rate > 0 && f + 1 < nframes) sleep_us(1000000ull / rate);
  }

done:
  printf("sent frames=%llu xport=%s port=%u\n", (unsigned long long)sent_frames, xport_name,
         (unsigned)port);
  fflush(stdout);
  free(fbuf);
  hml_stream_close(stream); /* closes the transport too */
  sensor.ops->close(&sensor);
  return rc;
}
