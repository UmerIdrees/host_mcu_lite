/*
 * hml_filestream -- file-stream sensor adapter CLI.
 *
 *   send <file>    replay the frames in an HMLF file onto the T0 UDP data path
 *   listen <file>  bind the T0 UDP data port and record the frames that arrive
 *
 * The two are the same format seen from opposite ends, so a capture and a
 * replay compose: `listen` records whatever a live sender emits, and `send`
 * puts that recording back on the wire with the frame lengths, the FNV-1a
 * hashes and the CRC-16s unchanged. The per-frame line matches the shape
 * hml_gen prints for the synthetic adapter, so the two can be diffed:
 *
 *   sent frame=0 bytes=9000 crc=0x1234 hash=0xb6bc71f8
 *
 * One line per frame plus a summary, and no silent success: a malformed record,
 * a short payload, an oversized frame or a writer failure all end the run with
 * a non-zero status and a message naming the record.
 */

#include "hml/hml_config.h"
#include "hml/hml_port.h"
#include "hml/hml_protocol.h"
#include "hml/hml_reasm.h"
#include "hml/hml_sock.h"
#include "hml/hml_stream.h"
#include "hml_filestream.h"
#include "hml_synth_priv.h" /* hml_fnv1a32 (hml_synth is linked into this target) */
#include "hml_udp_priv.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

static void sleep_us(uint64_t us) {
  struct timespec ts;
  ts.tv_sec = (time_t)(us / 1000000ull);
  ts.tv_nsec = (long)((us % 1000000ull) * 1000ull);
  (void)nanosleep(&ts, NULL);
}

static int parse_ipv4(const char *s, uint32_t *out_ne) {
  unsigned a, b, c, d;
  char extra;
  if (!s || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return -1;
  if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
  *out_ne = (a << 24) | (b << 16) | (c << 8) | d; /* already network byte order */
  return 0;
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s send <file> [--host A.B.C.D] [--port N] [--frames N] [--repeat]\n"
          "               [--rate N] [--stream-id N] [--seq-start N] [--max-frame N]\n"
          "       %s listen <file> [--port N] [--frames N] [--repeat] [--timeout-ms N]\n"
          "                    [--max-frame N]\n"
          "  send <file>     replay every HMLF record in <file> as T0 UDP frames\n"
          "  listen <file>   bind the UDP data port and append every completed frame\n"
          "                  to <file> in the same HMLF format\n"
          "  --host A.B.C.D  destination IPv4 (default 127.0.0.1)\n"
          "  --port N        UDP data port (default %u)\n"
          "  --frames N      stop after N frames (default 0 = until the file ends)\n"
          "  --repeat        rewind at end of file and keep going, for soak runs\n"
          "  --rate N        send only: frames per second, 0 = as fast as possible\n"
          "  --stream-id N   send only: stream id (default 1)\n"
          "  --seq-start N   send only: first fragment sequence number (default 0)\n"
          "  --max-frame N   reject records longer than N (default %u)\n"
          "  --timeout-ms N  listen only: give up after N ms with no datagram\n"
          "                   (default 0 = wait forever)\n"
          "\n"
          "per frame:   sent frame=<i> bytes=<n> crc=0x%%04x hash=0x%%08x\n"
          "             rx   frame=<i> seq=<n> bytes=<n> crc=0x%%04x hash=0x%%08x\n"
          "             crc  = CRC-16/CCITT over the frame, i.e. the HMLF record payload\n"
          "             hash = FNV-1a 32, the value hml_host prints as hash=\n",
          argv0, argv0, (unsigned)HML_DATA_PORT, (unsigned)HML_MAX_FRAME_SIZE);
}

/* ---- send --------------------------------------------------------------- */

static int cmd_send(const char *path, const char *host, uint16_t port, uint64_t want_frames,
                    int repeat, uint32_t rate, uint32_t stream_id, uint32_t seq_start,
                    size_t max_frame) {
  hml_fs_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.path = path;
  cfg.max_frame = max_frame;
  cfg.repeat = repeat != 0;

  hml_sensor_t sensor;
  memset(&sensor, 0, sizeof(sensor));
  sensor.ops = hml_filestream_ops();
  hml_status st = sensor.ops->open(&sensor, &cfg);
  if (st != HML_OK) {
    fprintf(stderr, "hml_filestream: open '%s' failed (%d): %s\n", path, (int)st,
            hml_filestream_last_error(&sensor));
    sensor.ops->close(&sensor);
    return 1;
  }

  hml_dest_cfg dest;
  memset(&dest, 0, sizeof(dest));
  dest.kind = HML_XPORT_UDP;
  if (parse_ipv4(host, &dest.ipv4) != 0) {
    fprintf(stderr, "hml_filestream: bad --host '%s'\n", host);
    sensor.ops->close(&sensor);
    return 2;
  }
  dest.udp_port = port;
  dest.frame_size = (uint32_t)max_frame;
  dest.hop_limit = 64;

  hml_xport_t xport;
  memset(&xport, 0, sizeof(xport));
  xport.ops = hml_udp_ops();
  st = xport.ops->open(&xport, &dest, (uint16_t)stream_id);
  if (st != HML_OK) {
    fprintf(stderr, "hml_filestream: transport open failed (%d)\n", (int)st);
    sensor.ops->close(&sensor);
    return 1;
  }

  hml_stream_cfg scfg;
  memset(&scfg, 0, sizeof(scfg));
  scfg.stream_id = (uint16_t)stream_id;
  scfg.max_frame_bytes = max_frame;
  scfg.seq_start = seq_start;
  scfg.mtu_effective = (size_t)HML_DEFAULT_MTU;
  hml_stream_t *stream = NULL;
  st = hml_stream_open(&stream, &scfg, &xport);
  if (st != HML_OK) {
    fprintf(stderr, "hml_filestream: stream open failed (%d)\n", (int)st);
    xport.ops->close(&xport);
    sensor.ops->close(&sensor);
    return 1;
  }

  uint8_t *frame = (uint8_t *)malloc(max_frame);
  if (!frame) {
    fprintf(stderr, "hml_filestream: out of memory for a %zu byte frame\n", max_frame);
    hml_stream_close(stream);
    sensor.ops->close(&sensor);
    return 1;
  }

  printf("send file=%s host=%s port=%u stream=%u max_frame=%zu repeat=%d\n", path, host,
         (unsigned)port, stream_id, max_frame, repeat);
  fflush(stdout);

  uint64_t frames = 0;
  uint64_t bytes = 0;
  hml_fs_stats fst;
  int rc = 0;
  for (;;) {
    if (g_stop) {
      fprintf(stderr, "hml_filestream: interrupted after %llu frames\n",
              (unsigned long long)frames);
      rc = 1;
      break;
    }
    if (want_frames != 0 && frames >= want_frames) break;

    size_t want = 0;
    st = hml_filestream_peek(&sensor, &want);
    if (st != HML_OK) {
      fprintf(stderr, "hml_filestream: %s\n", hml_filestream_last_error(&sensor));
      rc = 1;
      break;
    }
    if (want == 0) break; /* clean end of file */

    size_t got = 0;
    st = sensor.ops->acquire(&sensor, frame, want, &got);
    if (st != HML_OK) {
      fprintf(stderr, "hml_filestream: %s\n", hml_filestream_last_error(&sensor));
      rc = 1;
      break;
    }
    if (got != want) {
      fprintf(stderr, "hml_filestream: short read, %zu of %zu bytes\n", got, want);
      rc = 1;
      break;
    }
    st = sensor.ops->release(&sensor);
    if (st != HML_OK) {
      fprintf(stderr, "hml_filestream: release failed (%d)\n", (int)st);
      rc = 1;
      break;
    }

    hml_filestream_get_stats(&sensor, &fst);

    for (size_t off = 0; off < got;) {
      size_t chunk = got - off;
      if (chunk > (size_t)HML_MAX_FRAG_PAYLOAD) chunk = (size_t)HML_MAX_FRAG_PAYLOAD;
      const bool last = (off + chunk == got);
      for (;;) {
        st = hml_stream_send_frag(stream, frame + off, chunk, off == 0, last, hml_clock_us());
        if (st == HML_ERR_AGAIN) {
          sleep_us(200);
          continue;
        }
        break;
      }
      if (st != HML_OK) {
        fprintf(stderr, "hml_filestream: send_frag failed (%d)\n", (int)st);
        rc = 1;
        goto done;
      }
      off += chunk;
    }

    frames++;
    bytes += (uint64_t)got;
    printf("sent frame=%llu bytes=%zu crc=0x%04x hash=0x%08x\n", (unsigned long long)(frames - 1),
           got, (unsigned)fst.frame_crc, (unsigned)hml_fnv1a32(frame, got));
    fflush(stdout);
    if (rate > 0 && (want_frames == 0 || frames < want_frames)) sleep_us(1000000ull / rate);
  }

done:
  hml_filestream_get_stats(&sensor, &fst);
  printf("sent frames=%llu bytes=%llu file_crc=0x%04x records=%llu rewinds=%llu\n",
         (unsigned long long)frames, (unsigned long long)bytes, (unsigned)fst.file_crc,
         (unsigned long long)fst.records, (unsigned long long)fst.rewinds);
  fflush(stdout);

  free(frame);
  hml_stream_close(stream); /* closes the transport too */
  sensor.ops->close(&sensor);
  return rc;
}

/* ---- listen ------------------------------------------------------------- */

typedef struct rx_ctx {
  hml_fs_writer_t *w;
  uint64_t frames;
  uint64_t bytes;
  uint64_t eof_only;
  uint16_t last_crc;
  int failed;
} rx_ctx;

static void on_frame(void *ctx, uint16_t stream_id, uint32_t frame_seq, const uint8_t *frame,
                     size_t frame_bytes, const hml_frame_hdr *final_hdr) {
  (void)stream_id;
  (void)final_hdr;
  rx_ctx *r = (rx_ctx *)ctx;
  if (frame_bytes == 0) { /* EOF-only flush: no payload, so nothing to record */
    r->eof_only++;
    return;
  }
  uint16_t crc = 0;
  hml_status st = hml_fs_writer_add(r->w, frame, frame_bytes, &crc);
  if (st != HML_OK) {
    fprintf(stderr, "hml_filestream: %s\n", hml_fs_writer_last_error(r->w));
    r->failed = 1;
    return;
  }
  r->frames++;
  r->bytes += (uint64_t)frame_bytes;
  r->last_crc = crc;
  printf("rx frame=%llu seq=%u bytes=%zu crc=0x%04x hash=0x%08x\n",
         (unsigned long long)(r->frames - 1), (unsigned)frame_seq, frame_bytes, (unsigned)crc,
         (unsigned)hml_fnv1a32(frame, frame_bytes));
  fflush(stdout);
}

static int cmd_listen(const char *path, uint16_t port, uint64_t want_frames, int repeat,
                      size_t max_frame, uint32_t timeout_ms) {
  if (port == 0) {
    fprintf(stderr, "hml_filestream: listen needs an explicit --port to bind\n");
    return 2;
  }

  rx_ctx r;
  memset(&r, 0, sizeof(r));
  hml_status st = hml_fs_writer_open(&r.w, path);
  if (st != HML_OK) {
    fprintf(stderr, "hml_filestream: cannot write '%s' (%d)\n", path, (int)st);
    return 1;
  }

  hml_sock_t *sock = NULL;
  st = hml_sock_udp_create(&sock, 0u, port, false);
  if (st != HML_OK) {
    fprintf(stderr, "hml_filestream: bind udp/%u failed (%d): port busy or not permitted\n",
            (unsigned)port, (int)st);
    (void)hml_fs_writer_close(r.w);
    return 1;
  }

  hml_reasm_ctx_t *reasm = NULL;
  st = hml_reasm_open(&reasm, max_frame);
  if (st != HML_OK) {
    fprintf(stderr, "hml_filestream: reassembly open failed (%d)\n", (int)st);
    hml_sock_close(sock);
    (void)hml_fs_writer_close(r.w);
    return 1;
  }
  hml_reasm_set_eof_cb(reasm, on_frame, &r);

  uint8_t *buf = (uint8_t *)malloc((size_t)HML_MAX_FRAG_SIZE);
  if (!buf) {
    fprintf(stderr, "hml_filestream: out of memory\n");
    hml_reasm_close(reasm);
    hml_sock_close(sock);
    (void)hml_fs_writer_close(r.w);
    return 1;
  }

  printf("listen file=%s port=%u max_frame=%zu frames=%llu timeout_ms=%u\n", path, (unsigned)port,
         max_frame, (unsigned long long)want_frames, (unsigned)timeout_ms);
  fflush(stdout);

  const uint64_t idle_us = (uint64_t)timeout_ms * 1000ull;
  uint64_t last_rx = hml_clock_us();
  uint64_t dgrams = 0;
  int rc = 0;
  for (;;) {
    if (g_stop) {
      fprintf(stderr, "hml_filestream: interrupted after %llu frames\n",
              (unsigned long long)r.frames);
      rc = 1;
      break;
    }
    if (r.failed) {
      rc = 1;
      break;
    }
    if (want_frames != 0 && r.frames >= want_frames && !repeat) break;
    if (timeout_ms != 0 && hml_clock_us() - last_rx > idle_us) {
      fprintf(stderr, "hml_filestream: no datagram for %u ms, stopping\n", (unsigned)timeout_ms);
      rc = (want_frames != 0 && r.frames < want_frames) ? 1 : 0;
      break;
    }

    size_t n = 0;
    st = hml_sock_recv(sock, buf, (size_t)HML_MAX_FRAG_SIZE, &n, 20);
    if (st == HML_ERR_AGAIN || st == HML_ERR_TIMEOUT) continue;
    if (st != HML_OK) {
      fprintf(stderr, "hml_filestream: socket error (%d)\n", (int)st);
      rc = 1;
      break;
    }
    dgrams++;
    last_rx = hml_clock_us();
    (void)hml_reasm_feed_datagram(reasm, buf, n);

    /* drain whatever else is already queued; this never blocks */
    for (;;) {
      n = 0;
      st = hml_sock_recv(sock, buf, (size_t)HML_MAX_FRAG_SIZE, &n, 0);
      if (st != HML_OK) break;
      dgrams++;
      (void)hml_reasm_feed_datagram(reasm, buf, n);
    }
  }

  hml_reasm_stats rs;
  hml_reasm_get_stats(reasm, &rs);
  uint64_t w_records = 0;
  uint64_t w_bytes = 0;
  uint16_t file_crc = 0;
  hml_fs_writer_stats(r.w, &w_records, &w_bytes, &file_crc);
  printf("rx frames=%llu bytes=%llu dgrams=%llu eof_only=%llu file_crc=0x%04x records=%llu\n",
         (unsigned long long)r.frames, (unsigned long long)r.bytes, (unsigned long long)dgrams,
         (unsigned long long)r.eof_only, (unsigned)file_crc, (unsigned long long)w_records);
  printf("reasm frames_ok=%u frames_dropped=%u frags_recv=%u gaps=%u crc_fail=%u proto_fail=%u\n",
         (unsigned)rs.frames_ok, (unsigned)rs.frames_dropped, (unsigned)rs.frags_recv,
         (unsigned)rs.gaps, (unsigned)rs.crc_fail, (unsigned)rs.proto_fail);
  fflush(stdout);

  free(buf);
  hml_reasm_close(reasm);
  hml_sock_close(sock);
  if (hml_fs_writer_close(r.w) != HML_OK) rc = 1;
  return rc;
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv) {
  if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
    usage(argv[0]);
    return 0;
  }
  if (argc < 3) {
    usage(argv[0]);
    return 2;
  }
  const char *mode = argv[1];
  const char *path = argv[2];
  const int is_send = (strcmp(mode, "send") == 0);
  const int is_listen = (strcmp(mode, "listen") == 0);
  if (!is_send && !is_listen) {
    fprintf(stderr, "hml_filestream: unknown subcommand '%s'\n", mode);
    usage(argv[0]);
    return 2;
  }

  const char *host = "127.0.0.1";
  uint16_t port = (uint16_t)HML_DATA_PORT;
  uint64_t nframes = 0;
  uint32_t rate = 0;
  uint32_t stream_id = 1;
  uint32_t seq_start = 0;
  uint32_t timeout_ms = 0;
  size_t max_frame = (size_t)HML_MAX_FRAME_SIZE;
  int repeat = 0;

  for (int i = 3; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--host") == 0 && i + 1 < argc) {
      host = argv[++i];
    } else if (strcmp(a, "--port") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 0);
      if (v <= 0 || v > 65535) goto bad_arg;
      port = (uint16_t)v;
    } else if (strcmp(a, "--frames") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 0);
      if (v < 0) goto bad_arg;
      nframes = (uint64_t)v;
    } else if (strcmp(a, "--rate") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 0);
      if (v < 0) goto bad_arg;
      rate = (uint32_t)v;
    } else if (strcmp(a, "--stream-id") == 0 && i + 1 < argc) {
      stream_id = (uint32_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--seq-start") == 0 && i + 1 < argc) {
      seq_start = (uint32_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--timeout-ms") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 0);
      if (v < 0) goto bad_arg;
      timeout_ms = (uint32_t)v;
    } else if (strcmp(a, "--max-frame") == 0 && i + 1 < argc) {
      long v = strtol(argv[++i], NULL, 0);
      if (v <= 0 || (size_t)v > (size_t)HML_MAX_FRAME_SIZE) goto bad_arg;
      max_frame = (size_t)v;
    } else if (strcmp(a, "--repeat") == 0) {
      repeat = 1;
    } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "hml_filestream: unknown argument '%s'\n", a);
      usage(argv[0]);
      return 2;
    }
    continue;
  bad_arg:
    fprintf(stderr, "hml_filestream: bad value for %s\n", a);
    return 2;
  }

  if (is_listen && rate != 0) {
    fprintf(stderr, "hml_filestream: --rate is a send-side option\n");
    return 2;
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  (void)sigaction(SIGINT, &sa, NULL);
  (void)sigaction(SIGTERM, &sa, NULL);

  return is_send ? cmd_send(path, host, port, nframes, repeat, rate, stream_id, seq_start, max_frame)
                 : cmd_listen(path, port, nframes, repeat, max_frame, timeout_ms);
}
