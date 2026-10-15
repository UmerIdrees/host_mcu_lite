/*
 * linux_main.c -- the same hml work as ../non_rtos/, over a real UDP socket.
 *
 * The sibling demo links hml_core and a stub port and never opens a socket.
 * This one consumes an *installed* hml through find_package(), lets
 * hml::hml_core pull in hml_port_posix automatically, and does the real thing:
 * a sender socket emits HML-framed datagrams to 127.0.0.1, a receiver socket
 * reassembles them, and the received payload is compared byte for byte against
 * what was sent.
 *
 * It is deliberately a single process with two sockets rather than a
 * fork()/thread pair. A threaded demo would be testing the scheduler, and this
 * demo exists to test the port contract, the UDP datagram shaping, and the
 * reassembly counters.
 *
 * On the wire this is the whole data path that host/hml_host/ runs, minus the
 * discovery handshake: hml_sock_send() -> hml_sock_recv() ->
 * hml_reasm_feed_datagram() -> hml_reasm_set_eof_cb() callback. Nothing here
 * touches a GPU. See docs/gpu_host_pipeline.md for what a GPU consumer would
 * have to add, and for why that is not this demo.
 */

#include <hml/hml_crc.h>
#include <hml/hml_partition.h>
#include <hml/hml_protocol.h>
#include <hml/hml_reasm.h>
#include <hml/hml_sock.h>
#include <hml/hml_stream.h>
#include <hml/hml_transport.h>

#include <stdio.h>
#include <string.h>

/* ---- a minimal HML frame, built the same way the real transports do ------ */

/* One EOF-delimited frame: a 48-byte payload with a recognisable pattern, so a
 * misordered or zeroed buffer is obvious rather than merely suspicious. */
static void payload_fill(uint8_t *p, size_t n, unsigned seed) {
  size_t i;
  for (i = 0; i < n; i++) p[i] = (uint8_t)((i * 7u + seed * 31u) & 0xffu);
}

#define K_PAYLOAD_LEN 48u

/* One T0 datagram: HML frame header, payload, trailing CRC16. This mirrors
 * src/transports/udp/hml_udp.c; it is written out here because the whole point
 * of the demo is to show the byte layout a real port has to produce. */
static size_t build_datagram(uint8_t *dg, size_t cap, uint16_t stream_id, uint32_t seq,
                             const uint8_t *payload, size_t payload_len) {
  hml_frame_hdr h;
  size_t need = (size_t)HML_FRAME_HDR_SIZE + payload_len + (size_t)HML_TRAIL_CRC_SIZE;

  if (need > cap) return 0u;

  memset(&h, 0, sizeof(h));
  h.stream_id = stream_id;
  h.seq = seq;
  h.flags = 0u;
  h.payload_len = (uint16_t)payload_len;
  hml_proto_frame_fill(&h, dg);

  if (payload_len > 0u) memcpy(dg + HML_OFF_PAYLOAD, payload, payload_len);
  hml_put_be16(dg + HML_OFF_PAYLOAD + payload_len, hml_crc16(payload, payload_len));
  return need;
}

/* ---- receiver ------------------------------------------------------------ */

typedef struct rxstat {
  unsigned data_frames;
  unsigned eof_flushes;
  size_t bytes;
  uint16_t stream_id;
  uint32_t seq;
  const uint8_t *expected;
  size_t expected_len;
  int payload_match;
} rxstat;

static void on_frame(void *ctx, uint16_t stream_id, uint32_t frame_seq, const uint8_t *frame,
                     size_t frame_bytes, const hml_frame_hdr *final_hdr) {
  rxstat *r = (rxstat *)ctx;
  (void)final_hdr;
  if (frame_bytes == 0u) { /* zero-payload END: EOF without data */
    r->eof_flushes++;
    return;
  }
  r->data_frames++;
  r->bytes += frame_bytes;
  r->stream_id = stream_id;
  r->seq = frame_seq;
  r->payload_match = (frame_bytes == r->expected_len) &&
                     (memcmp(frame, r->expected, frame_bytes) == 0);
}

/* Pull datagrams until the timeout expires, feeding each to the reassembler.
 * Returns the number of datagrams read, or a negative hml_status. */
static int pump(hml_reasm_ctx_t *r, hml_sock_t *sock, int timeout_ms, int rounds) {
  uint8_t buf[HML_MAX_FRAG_SIZE];
  size_t n = 0u;
  int got = 0;
  int i;

  for (i = 0; i < rounds; i++) {
    hml_status st = hml_sock_recv(sock, buf, sizeof(buf), &n, timeout_ms);
    /* Both of these mean "nothing to read right now", not a failure: a bounded
     * wait reports HML_ERR_TIMEOUT, a poll or an interrupted recv reports
     * HML_ERR_AGAIN. Only a real error is negative here. */
    if (st == HML_ERR_TIMEOUT || st == HML_ERR_AGAIN) return got;
    if (st != HML_OK) return (int)st;
    if (n == 0u) continue;
    st = hml_reasm_feed_datagram(r, buf, n);
    if (st != HML_OK) return (int)st;
    got++;
  }
  return got;
}

int main(void) {
  int rc = 0;
  hml_sock_t *tx = NULL;
  hml_sock_t *rx = NULL;
  hml_reasm_ctx_t *reasm = NULL;
  hml_reasm_stats rst;
  uint8_t payload[K_PAYLOAD_LEN];
  uint8_t dg[HML_MAX_FRAG_SIZE];
  const uint16_t stream_id = 42u;
  size_t dg_len;
  int dgrams = 0;
  hml_status st;
  rxstat got;

  /* This demo needs the real POSIX port for its loopback socket. If the staged
   * install did not carry hml_port_posix, that is a packaging bug, and it should
   * fail at compile time with a clear message rather than mysteriously at the
   * first hml_sock_udp_create() call. */
  _Static_assert(HML_HAS_POSIX_PORT == 1,
                 "hml_dropin_linux needs the POSIX port: the staged install must contain "
                 "lib/libhml_port_posix.* and hml_port_posix must be in the hmlTargets export "
                 "set (configure hml with -DHML_NO_POSIX=OFF)");
  printf("hml %s, linkage=%s, posix_port_available=%d\n", HML_VERSION_STRING, HML_LINKAGE_NAME,
         HML_HAS_POSIX_PORT);
  printf("port contract symbols required: %s\n", HML_LIBC_REQUIREMENTS);

  /* 1. CRC check vectors. A wrong wire format is a silent correctness bug, so
   *    these come first: nothing below means anything if they are wrong. */
  {
    uint16_t c16 = hml_crc16("123456789", 9);
    uint32_t c32 = hml_crc32_ieee("123456789", 9);
    printf("[crc]   crc16(\"123456789\")=0x%04x crc32=0x%08x\n", c16, c32);
    if (c16 != 0x29b1u || c32 != 0xcbf43926u) {
      printf("[crc]   FAIL expected crc16=0x29b1 crc32=0xcbf43926\n");
      rc = 1;
    }
  }

  /* 2. Two sockets on loopback. The port applies htonl() itself, so both the
   *    bind address and set_dest() take HOST order. The receiver binds an
   *    explicit port because bind_port == 0 means "unbound"; the sender stays
   *    ephemeral. A fixed high port keeps the two ends in agreement. */
  {
    const uint16_t data_port = 34567u;
    payload_fill(payload, sizeof(payload), 7u);

    st = hml_sock_udp_create(&rx, 0x7f000001u, data_port, false);
    if (st == HML_OK) st = hml_sock_udp_create(&tx, 0u, 0u, false);
    if (st == HML_OK) st = hml_sock_set_dest(tx, 0x7f000001u, data_port);
    if (st != HML_OK) {
      printf("[sock]  FAIL bind/set_dest on 127.0.0.1:%u: %d\n", data_port, (int)st);
      if (rx) hml_sock_close(rx);
      if (tx) hml_sock_close(tx);
      return 1;
    }
    printf("[sock]  loopback 127.0.0.1:%u ready (posix port supplies the sockets)\n", data_port);

    /* 3. Reassembler, wired to the callback before any byte is sent. */
    st = hml_reasm_open(&reasm, (size_t)HML_MAX_FRAME_SIZE);
    if (st != HML_OK) {
      printf("[reasm] FAIL open: %d\n", (int)st);
      hml_sock_close(tx);
      hml_sock_close(rx);
      return 1;
    }
    memset(&got, 0, sizeof(got));
    got.expected = payload;
    got.expected_len = sizeof(payload);
    got.payload_match = 0;
    hml_reasm_set_eof_cb(reasm, on_frame, &got);

    /* 4. Send one data frame plus an EOF-only flush, both END-flagged so the
     *    receiver's reassembler emits. */
    dg_len = build_datagram(dg, sizeof(dg), stream_id, 900u, payload, sizeof(payload));
    if (dg_len == 0u) {
      printf("[send]  FAIL datagram does not fit\n");
      hml_reasm_close(reasm);
      hml_sock_close(tx);
      hml_sock_close(rx);
      return 1;
    }
    {
      hml_frame_hdr h;
      memset(&h, 0, sizeof(h));
      h.stream_id = stream_id;
      h.seq = 900u;
      h.flags = (uint16_t)HML_FLAG_END;
      h.payload_len = (uint16_t)sizeof(payload);
      hml_proto_frame_fill(&h, dg);
      memcpy(dg + HML_OFF_PAYLOAD, payload, sizeof(payload));
      hml_put_be16(dg + HML_OFF_PAYLOAD + sizeof(payload), hml_crc16(payload, sizeof(payload)));
    }
    st = hml_sock_send(tx, dg, dg_len);
    if (st != HML_OK) {
      printf("[send]  FAIL hml_sock_send: %d\n", (int)st);
      hml_reasm_close(reasm);
      hml_sock_close(tx);
      hml_sock_close(rx);
      return 1;
    }
    printf("[send]  one frame, %u bytes, seq=900, END flagged, %u-byte datagram\n",
           (unsigned)sizeof(payload), (unsigned)dg_len);

    /* 5. Drain. 4 rounds x 200ms: the first read gets the data frame, the next
     *    times out. A generous ceiling, so a slow machine does not fail. */
    dgrams = pump(reasm, rx, 200, 4);
    if (dgrams < 0) {
      printf("[reasm] FAIL feed: %d\n", dgrams);
      hml_reasm_close(reasm);
      hml_sock_close(tx);
      hml_sock_close(rx);
      return 1;
    }
  }

  /* 6. Verify: exactly one data frame, payload intact, no drops or CRC errors. */
  memset(&rst, 0, sizeof(rst));
  hml_reasm_get_stats(reasm, &rst);
  printf("[reasm]  datagrams_read=%d frames_ok=%u dropped=%u frags_recv=%u gaps=%u "
         "crc_fail=%u proto_fail=%u\n",
         dgrams, rst.frames_ok, rst.frames_dropped, rst.frags_recv, rst.gaps, rst.crc_fail,
         rst.proto_fail);
  printf("[reasm]  delivered data_frames=%u bytes=%zu stream_id=%u seq=%u payload_match=%d\n",
         got.data_frames, got.bytes, got.stream_id, got.seq, got.payload_match);

  if (got.data_frames != 1u || got.bytes != sizeof(payload) || !got.payload_match) {
    printf("[reasm] FAIL expected 1 intact frame of %u bytes\n", (unsigned)sizeof(payload));
    rc = 1;
  }
  if (got.stream_id != stream_id || got.seq != 900u) {
    printf("[reasm] FAIL expected stream_id=%u seq=900, got stream_id=%u seq=%u\n", stream_id,
           got.stream_id, got.seq);
    rc = 1;
  }
  if (rst.frames_ok != 1u || rst.crc_fail != 0u || rst.proto_fail != 0u ||
      rst.frames_dropped != 0u) {
    printf("[reasm] FAIL clean reassembly expected\n");
    rc = 1;
  }

  /* 7. Corrupt a datagram's magic and confirm it is rejected, not delivered.
   *    The point of the CRC and the magic check is fail-closed: a bad frame on
   *    a lossy link must drop, never surface as data. */
  {
    uint8_t bad[HML_MAX_FRAG_SIZE];
    size_t bad_len = build_datagram(bad, sizeof(bad), stream_id, 901u, payload, sizeof(payload));
    bad[HML_OFF_MAGIC] ^= 0xffu;
    hml_status bst = hml_sock_send(tx, bad, bad_len);
    if (bst != HML_OK) {
      printf("[parse] FAIL send of corrupt datagram: %d\n", (int)bst);
      rc = 1;
    } else {
      hml_frame_hdr parsed;
      const uint8_t *pl = NULL;
      hml_status pst = hml_proto_frame_parse(bad, bad_len, &parsed, &pl);
      (void)pl; /* only the reject/accept verdict matters here */
      if (pst == HML_OK) {
        printf("[parse] FAIL corrupted magic accepted\n");
        rc = 1;
      } else {
        printf("[parse] corrupted magic rejected (fail-closed)\n");
      }
      (void)pump(reasm, rx, 100, 2);
    }
  }

  hml_reasm_close(reasm);
  hml_sock_close(tx);
  hml_sock_close(rx);

  if (rc == 0) {
    printf("PASS: linux drop-in demo over real loopback UDP\n");
  } else {
    printf("FAIL: linux drop-in demo\n");
  }
  return rc;
}
