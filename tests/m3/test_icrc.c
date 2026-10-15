/*
 * M3 / T1a -- ICRC acceptance and rejection in the host RoCE demux.
 *
 * Feeds hand-built datagrams straight into the demux: one clean packet must be
 * accepted, one payload-bit flip and one ICRC-bit flip must both be rejected
 * with icrc_err++, and the EOF metadata packet must then complete and deliver
 * the frame exactly once.
 */

#include "hml_t_m3.h"

#include <string.h>

#include "hml/hml_crc.h"
#include "hml/hml_port.h"
#include "hml/hml_protocol.h"
#include "hml/hml_sock.h"
#include "hml_roce_demux.h"
#include "hml_roce_priv.h"

#define PORT (HML_ROCE_TEST_PORT + 1u) /* 14792, separate from the other tests */
#define WIN_BASE 0x100000ull
#define WIN_LEN 4096u
#define FRAME_SIZE 32u
#define RKEY 0xDEADBEEFu
#define LOOPBACK 0x7F000001u

typedef struct rx_state {
  uint32_t calls;
  uint32_t last_seq;
  size_t last_n;
  uint32_t last_crc;
  uint8_t last[64];
} rx_state;

static void on_eof(void *ctx, uint32_t frame_seq, const uint8_t *frame, size_t n, uint32_t crc32) {
  rx_state *s = (rx_state *)ctx;
  s->calls++;
  s->last_seq = frame_seq;
  s->last_n = n;
  s->last_crc = crc32;
  size_t cp = n < sizeof(s->last) ? n : sizeof(s->last);
  memcpy(s->last, frame, cp);
}

/* Build one datagram into `dg`; returns its length, 0 on failure. */
static size_t build(uint8_t *dg, size_t cap, uint32_t psn, uint64_t va, const uint8_t *pay,
                    size_t len) {
  hml_roce_wire_cfg w;
  memset(&w, 0, sizeof(w));
  w.src_ip = LOOPBACK;
  w.dst_ip = LOOPBACK;
  w.udp_port = PORT;
  w.ttl = 64;
  w.ip_id = (uint16_t)psn;
  w.psn = psn;
  w.va = va;
  w.rkey = RKEY;
  return hml_roce_build_datagram(dg, cap, &w, pay, len);
}

static hml_status tx(hml_sock_t *s, uint32_t psn, uint64_t va, const uint8_t *pay, size_t len) {
  uint8_t dg[HML_ROCE_DG_MAX];
  size_t n = build(dg, sizeof(dg), psn, va, pay, len);
  if (n == 0) return HML_ERR_TOOLARGE;
  return hml_sock_send(s, dg, n);
}

/* Wait until the demux has accounted for `target` datagrams, or the deadline. */
static int wait_pkts(hml_roce_rx_t *rx, uint64_t target, int ms) {
  uint64_t deadline = hml_clock_us() + (uint64_t)ms * 1000ull;
  for (;;) {
    hml_roce_stats st;
    hml_roce_rx_stats(rx, &st);
    if (st.pkts >= target) return 0;
    if (hml_clock_us() >= deadline) return -1;
    (void)hml_roce_rx_poll(rx, 20);
  }
}

int main(void) {
  uint8_t frame[FRAME_SIZE];
  for (size_t i = 0; i < sizeof(frame); i++) frame[i] = (uint8_t)i;

  rx_state rxs;
  memset(&rxs, 0, sizeof(rxs));

  hml_roce_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.udp_port = PORT;
  cfg.window_base = WIN_BASE;
  cfg.window_len_bytes = WIN_LEN;
  cfg.frame_size = FRAME_SIZE;
  cfg.rkey = RKEY;
  cfg.verify_icrc = true;

  hml_roce_rx_t *rx = NULL;
  T_CHECK(hml_roce_rx_open(&rx, &cfg, on_eof, &rxs) == HML_OK, "hml_roce_rx_open failed");
  if (!rx) T_DONE("test_icrc");

  hml_sock_t *txs = NULL;
  T_CHECK(hml_sock_udp_create(&txs, 0, 0, false) == HML_OK, "sender socket failed");
  T_CHECK(hml_sock_set_dest(txs, LOOPBACK, PORT) == HML_OK, "hml_sock_set_dest failed");

  const uint32_t P0 = 0x00010000u;

  /* 1. a clean write must be accepted. */
  T_CHECK(tx(txs, P0, WIN_BASE, frame, sizeof(frame)) == HML_OK, "clean write tx failed");
  T_CHECK(wait_pkts(rx, 1, 3000) == 0, "clean write was never received");
  {
    hml_roce_stats st;
    hml_roce_rx_stats(rx, &st);
    T_CHECK(st.icrc_err == 0, "clean packet: icrc_err = %llu, want 0",
            (unsigned long long)st.icrc_err);
    T_CHECK(st.writes == 1, "clean packet: writes = %llu, want 1",
            (unsigned long long)st.writes);
    T_CHECK(st.bytes == FRAME_SIZE, "clean packet: bytes = %llu, want %u",
            (unsigned long long)st.bytes, (unsigned)FRAME_SIZE);
  }

  /* 2. flip one payload bit in flight -> ICRC must catch it. */
  {
    uint8_t dg[HML_ROCE_DG_MAX];
    size_t n = build(dg, sizeof(dg), P0 + 1, WIN_BASE, frame, sizeof(frame));
    T_CHECK(n > 0, "build failed");
    dg[56 + 7] ^= 0x01u; /* single bit flip inside the write payload */
    T_CHECK(hml_sock_send(txs, dg, n) == HML_OK, "corrupt write tx failed");
    T_CHECK(wait_pkts(rx, 2, 3000) == 0, "corrupt write was never received");
    hml_roce_stats st;
    hml_roce_rx_stats(rx, &st);
    T_CHECK(st.icrc_err == 1, "payload flip: icrc_err = %llu, want 1",
            (unsigned long long)st.icrc_err);
    T_CHECK(st.writes == 1, "payload flip: writes = %llu, want 1 (packet must be dropped)",
            (unsigned long long)st.writes);
  }

  /* 3. flip one ICRC bit -> must also be caught. */
  {
    uint8_t dg[HML_ROCE_DG_MAX];
    size_t n = build(dg, sizeof(dg), P0 + 2, WIN_BASE, frame, sizeof(frame));
    T_CHECK(n > 4, "build failed");
    dg[n - 1] ^= 0x80u; /* corrupt the top ICRC byte */
    T_CHECK(hml_sock_send(txs, dg, n) == HML_OK, "icrc-corrupt tx failed");
    T_CHECK(wait_pkts(rx, 3, 3000) == 0, "icrc-corrupt packet was never received");
    hml_roce_stats st;
    hml_roce_rx_stats(rx, &st);
    T_CHECK(st.icrc_err == 2, "icrc flip: icrc_err = %llu, want 2",
            (unsigned long long)st.icrc_err);
    T_CHECK(st.writes == 1, "icrc flip: writes = %llu, want 1 (packet must be dropped)",
            (unsigned long long)st.writes);
  }

  /* 4. the EOF metadata write completes the frame and delivers it once. */
  {
    uint8_t meta[HML_ROCE_META_LEN];
    uint32_t magic = HML_MAGIC_FRAME;
    uint16_t sid = 3, flags = HML_FLAG_START | HML_FLAG_END;
    uint32_t seq = 77, fbytes = FRAME_SIZE, crc = hml_crc32_ieee(frame, sizeof(frame));
    memcpy(meta + 0, &magic, 4);
    memcpy(meta + 4, &sid, 2);
    memcpy(meta + 6, &flags, 2);
    memcpy(meta + 8, &seq, 4);
    memcpy(meta + 12, &fbytes, 4);
    memcpy(meta + 16, &crc, 4);
    T_CHECK(tx(txs, P0 + 3, WIN_BASE + FRAME_SIZE, meta, sizeof(meta)) == HML_OK,
            "meta tx failed");
    T_CHECK(wait_pkts(rx, 4, 3000) == 0, "meta was never received");
    hml_roce_stats st;
    hml_roce_rx_stats(rx, &st);
    T_CHECK(st.eof_frames == 1, "eof_frames = %llu, want 1", (unsigned long long)st.eof_frames);
    T_CHECK(rxs.calls == 1, "callback invoked %u times, want 1", rxs.calls);
    T_CHECK(rxs.last_seq == seq, "callback seq = %u, want %u", rxs.last_seq, seq);
    T_CHECK(rxs.last_n == FRAME_SIZE, "callback n = %zu, want %u", rxs.last_n,
            (unsigned)FRAME_SIZE);
    T_CHECK(rxs.last_crc == crc, "callback crc = 0x%08X, want 0x%08X", rxs.last_crc, crc);
    T_CHECK(memcmp(rxs.last, frame, FRAME_SIZE) == 0, "delivered frame bytes differ");

    /* 5. a duplicate meta with the same frame_seq must never deliver twice. */
    T_CHECK(tx(txs, P0 + 4, WIN_BASE + FRAME_SIZE, meta, sizeof(meta)) == HML_OK,
            "duplicate meta tx failed");
    T_CHECK(wait_pkts(rx, 5, 3000) == 0, "duplicate meta was never received");
    hml_roce_rx_stats(rx, &st);
    T_CHECK(st.eof_frames == 1, "duplicate meta: eof_frames = %llu, want 1",
            (unsigned long long)st.eof_frames);
    T_CHECK(rxs.calls == 1, "duplicate meta: callback invoked %u times, want 1", rxs.calls);
  }

  /* 6. a meta whose CRC does not match the assembled window is dropped. */
  {
    uint8_t meta[HML_ROCE_META_LEN];
    uint32_t magic = HML_MAGIC_FRAME, seq = 78, fbytes = FRAME_SIZE;
    uint16_t sid = 3, flags = HML_FLAG_END;
    uint32_t bad_crc = hml_crc32_ieee(frame, sizeof(frame)) ^ 0xFFFFFFFFu;
    memcpy(meta + 0, &magic, 4);
    memcpy(meta + 4, &sid, 2);
    memcpy(meta + 6, &flags, 2);
    memcpy(meta + 8, &seq, 4);
    memcpy(meta + 12, &fbytes, 4);
    memcpy(meta + 16, &bad_crc, 4);
    T_CHECK(tx(txs, P0 + 5, WIN_BASE + FRAME_SIZE, meta, sizeof(meta)) == HML_OK,
            "bad-crc meta tx failed");
    T_CHECK(wait_pkts(rx, 6, 3000) == 0, "bad-crc meta was never received");
    hml_roce_stats st;
    hml_roce_rx_stats(rx, &st);
    T_CHECK(st.eof_frames == 1, "bad-crc meta: eof_frames = %llu, want 1",
            (unsigned long long)st.eof_frames);
    T_CHECK(st.meta_drops == 1, "bad-crc meta: meta_drops = %llu, want 1",
            (unsigned long long)st.meta_drops);
    T_CHECK(rxs.calls == 1, "bad-crc meta: callback invoked %u times, want 1", rxs.calls);
  }

  /* 7. a meta carrying the wrong magic is dropped too. */
  {
    uint8_t meta[HML_ROCE_META_LEN];
    uint32_t magic = 0xDEADBEEFu, seq = 79, fbytes = 0, crc = 0;
    uint16_t sid = 3, flags = HML_FLAG_END;
    memcpy(meta + 0, &magic, 4);
    memcpy(meta + 4, &sid, 2);
    memcpy(meta + 6, &flags, 2);
    memcpy(meta + 8, &seq, 4);
    memcpy(meta + 12, &fbytes, 4);
    memcpy(meta + 16, &crc, 4);
    T_CHECK(tx(txs, P0 + 6, WIN_BASE + FRAME_SIZE, meta, sizeof(meta)) == HML_OK,
            "bad-magic meta tx failed");
    T_CHECK(wait_pkts(rx, 7, 3000) == 0, "bad-magic meta was never received");
    hml_roce_stats st;
    hml_roce_rx_stats(rx, &st);
    T_CHECK(st.meta_drops == 2, "bad-magic meta: meta_drops = %llu, want 2",
            (unsigned long long)st.meta_drops);
    T_CHECK(rxs.calls == 1, "bad-magic meta: callback invoked %u times, want 1", rxs.calls);
  }

  {
    hml_roce_stats st;
    hml_roce_rx_stats(rx, &st);
    printf("  pkts=%llu icrc_err=%llu proto_err=%llu writes=%llu bytes=%llu eof=%llu gaps=%llu drops=%llu\n",
           (unsigned long long)st.pkts, (unsigned long long)st.icrc_err,
           (unsigned long long)st.proto_err, (unsigned long long)st.writes,
           (unsigned long long)st.bytes, (unsigned long long)st.eof_frames,
           (unsigned long long)st.psn_gaps, (unsigned long long)st.meta_drops);
  }

  hml_sock_close(txs);
  hml_roce_rx_close(rx);
  T_DONE("test_icrc");
}
