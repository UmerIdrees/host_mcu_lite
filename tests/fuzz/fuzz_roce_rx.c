/*
 * fuzz_roce_rx -- the T1a receive parser (host/roce_demux/hml_roce_demux.c).
 *
 * rx_datagram() is file-static, so the only way to reach it is through a real
 * socket: open a receiver, send the input as one datagram, poll. It is worth the
 * socket, because that function is the single largest attacker-controlled parser
 * in the tree -- an IPv4 header with variable IHL, a UDP header, a BTH with a
 * 24-bit PSN, a RETH whose length field is 32 bits wide for a 28-byte header, a
 * VA that indexes a simulated RDMA window, an ICRC, and a 20-byte little-endian
 * end-of-frame record whose frame_bytes feeds a CRC over the window.
 *
 * The destination port is compared against the configuration, so the harness
 * derives the receiver port from the input and rewrites the datagram's UDP
 * destination port to match; that keeps the accept path reachable without
 * changing anything else in the datagram. Every other byte is the input's.
 *
 * Invariants asserted (all checked against hml_roce_rx_stats, so they are
 * statements about the receiver's own accounting):
 *   - a datagram is never counted twice: the sum of the failure counters and the
 *     write counter never exceeds pkts. A stale-PSN datagram is counted by none
 *     of them, which is why this is an upper bound and not an equality;
 *   - bytes never exceeds writes times the receive buffer, i.e. one accepted
 *     datagram can never copy more than the buffer it was read into;
 *   - a delivered frame is never longer than the configured window, and its
 *     CRC-32 recomputes to the value the receiver reported;
 *   - a sealed datagram with a recomputed ICRC is accepted, not counted as an
 *     ICRC or protocol error.
 *
 * The sealed datagram is sent FIRST, on a receiver that has seen nothing: the
 * demux accepts the first PSN it sees unconditionally, whereas a duplicate or
 * backwards PSN is dropped without any counter, so a sealed packet sent after a
 * hostile one would sometimes be ignored for a legitimate reason.
 *
 * Ports: a free UDP port is picked at startup from a private range and a small
 * window is rotated per input. A busy port, or a datagram the kernel dropped, is
 * counted and reported rather than treated as a defect.
 */

#include "hml_fuzz.h"

#include "hml/hml_crc.h"
#include "hml/hml_port.h"
#include "hml_roce_demux.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT_LO 48000u
#define PORT_HI 48999u
#define PORT_SPAN 16u

#define RX_WINDOW_BASE 0x100000ull
#define RX_FRAME_SIZE 9000u
#define RX_WINDOW_LEN 65536u
#define RX_RKEY 0xdeadbeefu
/* HML_RX_MAX_DG in the demux: the receive buffer, so the hard ceiling on what a
 * single accepted datagram can copy. */
#define RX_MAX_DG 2048u
/* A datagram sent on loopback is already queued when poll() is called, so this
 * budget only bounds the case where the kernel dropped it. */
#define RX_POLL_MS 5

static uint16_t g_port0;
static unsigned long g_sent, g_delivered, g_accepted, g_unreachable, g_skipped;

typedef struct rx_probe {
  unsigned long frames;
  unsigned long bad_crc;
} rx_probe;

static void on_frame(void *ctx, uint32_t frame_seq, const uint8_t *frame, size_t n,
                     uint32_t crc32) {
  rx_probe *p = (rx_probe *)ctx;
  if (n > RX_WINDOW_LEN) {
    hml_fuzz_finding("demux delivered a %zu byte frame from a %u byte window", n,
                     (unsigned)RX_WINDOW_LEN);
  }
  if (frame != NULL && n > 0u && hml_crc32_ieee(frame, n) != crc32) {
    p->bad_crc++;
    hml_fuzz_finding("demux delivered frame %u whose CRC-32 does not match the value it "
                     "reported",
                     (unsigned)frame_seq);
  }
  p->frames++;
}

/* Build a structurally valid RoCEv2 UC RDMA WRITE datagram around the input so
 * the accept path is reachable: correct ICRC, correct RETH length, and a UDP
 * destination port the receiver is actually listening on. */
static size_t seal_datagram(const uint8_t *data, size_t n, uint16_t port, uint8_t *out,
                            size_t cap) {
  const size_t ip_hdr = 20u;
  const size_t bth = 12u;
  const size_t reth = 16u;
  const size_t icrc = 4u;
  if (cap < ip_hdr + 8u + bth + reth + icrc) return 0;

  size_t wlen = n > 1u ? (size_t)(((uint32_t)data[0] << 8) | data[1]) : 0u;
  /* Two ceilings, and both matter. cap is the output buffer. RX_MAX_DG is the
   * receiver's own buffer: a datagram larger than that is silently clipped on
   * arrival, so the RETH length would then describe more bytes than the demux ever
   * saw and a valid packet would be rejected. That is the demux behaving correctly
   * on a truncated read, and it would be a false finding here -- so the sealed
   * write is clamped to what the receiver can actually take. */
  const size_t overhead = ip_hdr + 8u + bth + reth + icrc;
  const size_t maxw = (RX_MAX_DG > overhead ? RX_MAX_DG - overhead : 0u);
  if (wlen > maxw) wlen = maxw;
  if (wlen + overhead > cap) wlen = cap - overhead;

  const size_t total = ip_hdr + 8u + bth + reth + wlen + icrc;
  memset(out, 0, total);
  out[0] = 0x45u;                     /* version 4, IHL 5 */
  out[2] = (uint8_t)(total >> 8);
  out[3] = (uint8_t)total;
  out[8] = 64u;                       /* ttl */
  out[9] = 17u;                       /* UDP */
  hml_put_be16(out + 10, 0u);         /* ip checksum: not verified by the demux */

  uint8_t *udp = out + ip_hdr;
  hml_put_be16(udp + 0, 0x1000u);     /* source port */
  hml_put_be16(udp + 2, port);        /* destination port: the receiver's */
  hml_put_be16(udp + 4, (uint16_t)(8u + bth + reth + wlen + icrc));
  hml_put_be16(udp + 6, 0u);          /* udp checksum: accepted, not verified */

  uint8_t *bh = udp + 8u;
  bh[0] = 0x2au;                      /* BTH OpCode: UC RDMA Write */
  hml_put_be32(bh + 3, 1u);           /* PSN, 24-bit BE at bytes 3..5 */
  hml_put_be16(bh + 6, 0u);           /* AETH */
  hml_put_be32(bh + 8, 0u);           /* reserved */

  uint8_t *rh = bh + bth;
  hml_put_be64(rh + 0, RX_WINDOW_BASE);
  hml_put_be32(rh + 8, RX_RKEY);
  hml_put_be32(rh + 12, (uint32_t)wlen);

  uint8_t *pay = rh + reth;
  for (size_t i = 0; i < wlen; i++) pay[i] = (n > 2u) ? data[2u + (i % (n - 2u))] : (uint8_t)i;

  const size_t cov = bth + reth + wlen;
  uint32_t c = hml_crc32_ieee(bh, cov); /* the ICRC is little-endian on the wire */
  pay[wlen + 0u] = (uint8_t)c;
  pay[wlen + 1u] = (uint8_t)(c >> 8);
  pay[wlen + 2u] = (uint8_t)(c >> 16);
  pay[wlen + 3u] = (uint8_t)(c >> 24);
  return total;
}

/* Point the datagram's UDP destination port at the receiver, when the input has
 * an IPv4 header shape the demux will walk past. */
static void retarget(uint8_t *dg, size_t n, uint16_t port) {
  if (n < 1u) return;
  if ((uint8_t)(dg[0] >> 4) != 4u) return;
  size_t ihl = (size_t)(dg[0] & 0x0fu) * 4u;
  if (ihl < 20u || ihl > 60u) return;
  if (n < ihl + 4u) return;
  hml_put_be16(dg + ihl + 2u, port);
}

static void check_accounting(const hml_roce_stats *s) {
  uint64_t classified = s->icrc_err + s->proto_err + s->writes;
  if (classified > s->pkts) {
    hml_fuzz_finding("%llu datagrams received but %llu accounted for (icrc=%llu proto=%llu "
                     "writes=%llu)",
                     (unsigned long long)s->pkts, (unsigned long long)classified,
                     (unsigned long long)s->icrc_err, (unsigned long long)s->proto_err,
                     (unsigned long long)s->writes);
  }
  /* wlen is checked against cov - 28 where cov is derived from the datagram as
   * read into HML_RX_MAX_DG bytes, so one accepted datagram copies at most that
   * many bytes into the window. */
  if (s->bytes > s->writes * (uint64_t)RX_MAX_DG) {
    hml_fuzz_finding("bytes counter %llu exceeds %llu writes x %u receive bytes",
                     (unsigned long long)s->bytes, (unsigned long long)s->writes,
                     (unsigned)RX_MAX_DG);
  }
}

static void run(const uint8_t *data, size_t n) {
  uint16_t port =
      (uint16_t)(PORT_LO + (((uint32_t)(g_port0 - PORT_LO) + (g_sent % PORT_SPAN)) % PORT_SPAN));
  if (!hml_fuzz_port_free(port)) {
    g_skipped++;
    return;
  }

  hml_roce_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.udp_port = port;
  cfg.window_base = RX_WINDOW_BASE;
  cfg.window_len_bytes = RX_WINDOW_LEN;
  cfg.frame_size = RX_FRAME_SIZE;
  cfg.rkey = RX_RKEY;
  cfg.verify_icrc = true;

  rx_probe probe;
  memset(&probe, 0, sizeof(probe));
  hml_roce_rx_t *rx = NULL;
  if (hml_roce_rx_open(&rx, &cfg, on_frame, &probe) != HML_OK || rx == NULL) {
    g_skipped++;
    return;
  }

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    hml_roce_rx_close(rx);
    g_skipped++;
    return;
  }
  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(0x7f000001u);
  to.sin_port = htons(port);

  static uint8_t dg[HML_FUZZ_MAX_SEAL * 2u];

  /* Pass 1: a sealed datagram, on a receiver that has not seen a PSN yet, so it
   * is guaranteed to reach the accept path. */
  size_t m = seal_datagram(data, n, port, dg, sizeof(dg));
  if (m > 0u) {
    hml_roce_stats before;
    hml_roce_rx_stats(rx, &before);
    (void)sendto(fd, dg, m, 0, (struct sockaddr *)&to, sizeof(to));
    (void)hml_roce_rx_poll(rx, RX_POLL_MS);
    hml_roce_stats after;
    hml_roce_rx_stats(rx, &after);
    check_accounting(&after);
    if (after.pkts == before.pkts + 1u) {
      g_accepted++;
      if (after.icrc_err != before.icrc_err || after.proto_err != before.proto_err) {
        hml_fuzz_finding("a sealed datagram with a recomputed ICRC was rejected "
                         "(icrc %llu->%llu proto %llu->%llu)",
                         (unsigned long long)before.icrc_err,
                         (unsigned long long)after.icrc_err,
                         (unsigned long long)before.proto_err,
                         (unsigned long long)after.proto_err);
      }
      if (after.writes != before.writes + 1u) {
        hml_fuzz_finding("a sealed datagram produced %llu writes, want 1",
                         (unsigned long long)(after.writes - before.writes));
      }
    }
  }

  /* Pass 2: the raw input. Whatever it is, it must be counted, not acted on. */
  size_t len = n < sizeof(dg) ? n : sizeof(dg);
  memcpy(dg, data, len);
  retarget(dg, len, port);
  (void)sendto(fd, dg, len, 0, (struct sockaddr *)&to, sizeof(to));
  g_sent++;
  hml_roce_stats before_raw;
  hml_roce_rx_stats(rx, &before_raw);
  (void)hml_roce_rx_poll(rx, RX_POLL_MS);
  hml_roce_stats s1;
  hml_roce_rx_stats(rx, &s1);
  if (s1.pkts == before_raw.pkts) {
    g_unreachable++; /* the kernel dropped it; there is nothing to assert */
  } else {
    check_accounting(&s1);
  }

  g_delivered += probe.frames;
  (void)close(fd);
  hml_roce_rx_close(rx);
}

static void setup(void) {
  uint16_t seed = (uint16_t)(20000u + ((unsigned)getpid() % 20000u));
  g_port0 = hml_fuzz_port_pick(seed, PORT_LO, PORT_HI, PORT_SPAN);
  if (g_port0 == 0u) {
    fprintf(stderr, "fuzz_roce_rx: no free UDP port in %u..%u\n", PORT_LO, PORT_HI);
    exit(HML_FUZZ_EXIT_ERROR);
  }
}

static void summary(void) {
  printf("fuzz_roce_rx: sent=%lu sealed_accepted=%lu undelivered=%lu skipped=%lu "
         "frames=%lu\n",
         g_sent, g_accepted, g_unreachable, g_skipped, g_delivered);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  run(data, size);
  return 0;
}

#ifndef HML_FUZZ_LIBFUZZER
int main(int argc, char **argv) {
  setup();
  int rc = hml_fuzz_main(argc, argv, "fuzz_roce_rx", run, NULL);
  summary();
  return rc;
}
#endif
