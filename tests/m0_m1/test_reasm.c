/* M1: frame reassembly - ordering, gaps, duplicates, EOF-only flush, soak. */
#include "hml/hml_crc.h"
#include "hml/hml_protocol.h"
#include "hml/hml_reasm.h"
#include "hml_test_util.h"

#include <stdlib.h>

#define MAX_FRAGS 8
#define CHUNK 64

typedef struct {
  uint32_t calls;
  uint32_t last_seq;
  size_t last_bytes;
  uint8_t last[1024];
  uint8_t expected[1024];
  size_t expected_bytes;
  uint32_t seqs[4096];
  size_t sizes[4096];
  uint32_t n;
} sink;

static void on_frame(void *ctx, uint16_t stream_id, uint32_t frame_seq, const uint8_t *frame,
                     size_t frame_bytes, const hml_frame_hdr *final_hdr) {
  (void)stream_id;
  (void)final_hdr;
  sink *k = (sink *)ctx;
  if (k->n < 4096) {
    k->seqs[k->n] = frame_seq;
    k->sizes[k->n] = frame_bytes;
    k->n++;
  }
  k->calls++;
  k->last_seq = frame_seq;
  k->last_bytes = frame_bytes;
  if (frame_bytes && frame_bytes <= sizeof(k->last)) memcpy(k->last, frame, frame_bytes);
}

static void build_dg(uint8_t *dg, uint16_t stream, uint32_t seq, uint16_t idx, uint16_t cnt,
                     uint8_t flags, uint64_t ts, const uint8_t *payload, size_t len) {
  hml_frame_hdr h;
  h.stream_id = stream;
  h.seq = seq;
  h.frag_idx = idx;
  h.frag_cnt = cnt;
  h.ts_us = ts;
  h.flags = flags;
  h.payload_len = (uint16_t)len;
  hml_proto_frame_fill(&h, dg);
  if (len) memcpy(dg + HML_OFF_PAYLOAD, payload, len);
  hml_put_be16(dg + HML_OFF_PAYLOAD + len, hml_crc16(payload, len));
}

/* feed one fragment; payload is chunk index i (0,1,2...) so order is verifiable */
static void feed_frag(hml_reasm_ctx_t *r, uint16_t stream, uint32_t seq, uint16_t idx, uint8_t flags,
                      uint8_t val, size_t len) {
  uint8_t pay[CHUNK];
  uint8_t dg[HML_MAX_FRAG_SIZE];
  memset(pay, val, len);
  build_dg(dg, stream, seq, idx, 0, flags, 0, pay, len);
  hml_status st = hml_reasm_feed_datagram(r, dg, HML_FRAME_HDR_SIZE + len + HML_TRAIL_CRC_SIZE);
  if (st != HML_OK) printf("FAIL feed status %d\n", (int)st), g_fails++;
}

int main(void) {
  hml_reasm_stats st;
  sink k;
  hml_reasm_ctx_t *r = NULL;
  uint8_t dg[HML_MAX_FRAG_SIZE];

  memset(&k, 0, sizeof(k));
  CHECK_STATUS(hml_reasm_open(&r, 1024), HML_OK);
  hml_reasm_set_eof_cb(r, on_frame, &k);

  /* ---- single-packet frame ---- */
  {
    uint8_t pay[100];
    for (size_t i = 0; i < sizeof(pay); i++) pay[i] = (uint8_t)i;
    build_dg(dg, 7, 1000, 0, 0, (uint8_t)(HML_FLAG_START | HML_FLAG_END), 5, pay, sizeof(pay));
    CHECK_STATUS(hml_reasm_feed_datagram(r, dg, HML_FRAME_HDR_SIZE + sizeof(pay) + 2), HML_OK);
    hml_reasm_get_stats(r, &st);
    CHECK_EQ_U32(k.calls, 1u);
    CHECK_EQ_U32(k.last_seq, 1000u);
    CHECK_EQ_SIZE(k.last_bytes, sizeof(pay));
    CHECK(memcmp(k.last, pay, sizeof(pay)) == 0);
    CHECK_EQ_U32(st.frames_ok, 1u);
    CHECK_EQ_U32(st.frags_recv, 1u);
    CHECK_EQ_U32(st.frames_dropped, 0u);
    CHECK_EQ_U32(st.gaps, 0u);
  }

  /* ---- multi-fragment in order (3 frags: 64,64,32 -> 160 bytes) ---- */
  {
    k.n = 0;
    uint8_t want[3 * CHUNK];
    memset(want, 0, CHUNK);
    memset(want + CHUNK, 1, CHUNK);
    memset(want + 2 * CHUNK, 2, CHUNK - 32);
    feed_frag(r, 7, 2000, 0, HML_FLAG_START, 0, CHUNK);
    feed_frag(r, 7, 2001, 1, 0, 1, CHUNK);
    feed_frag(r, 7, 2002, 2, HML_FLAG_END, 2, 32);
    hml_reasm_get_stats(r, &st);
    CHECK_EQ_U32(k.calls, 2u);
    CHECK_EQ_U32(k.last_seq, 2000u); /* frame seq == seq of the first fragment */
    CHECK_EQ_SIZE(k.last_bytes, 2 * CHUNK + (CHUNK - 32));
    CHECK(memcmp(k.last, want, 2 * CHUNK + (CHUNK - 32)) == 0);
    CHECK_EQ_U32(st.frames_ok, 2u);
    CHECK_EQ_U32(st.frags_recv, 4u);
    CHECK_EQ_U32(st.gaps, 0u);
  }

  /* ---- REVERSED delivery order: END arrives first, stragglers follow ---- */
  {
    k.n = 0;
    uint8_t want[3 * CHUNK];
    memset(want, 0, CHUNK);
    memset(want + CHUNK, 1, CHUNK);
    memset(want + 2 * CHUNK, 2, CHUNK - 32);
    feed_frag(r, 7, 3002, 2, HML_FLAG_END, 2, 32);
    feed_frag(r, 7, 3001, 1, 0, 1, CHUNK);
    feed_frag(r, 7, 3000, 0, HML_FLAG_START, 0, CHUNK); /* late first fragment */
    hml_reasm_get_stats(r, &st);
    CHECK_EQ_U32(k.calls, 3u);
    CHECK_EQ_U32(k.last_seq, 3000u); /* frame id == seq of the START fragment */
    CHECK_EQ_SIZE(k.last_bytes, 2 * CHUNK + (CHUNK - 32));
    CHECK(memcmp(k.last, want, 2 * CHUNK + (CHUNK - 32)) == 0);
    CHECK_EQ_U32(st.frames_ok, 3u);
    CHECK_EQ_U32(st.gaps, 0u);
    CHECK_EQ_U32(st.frames_dropped, 0u);
  }

  /* ---- duplicate fragment is idempotent (no double delivery / no bytes) ---- */
  {
    k.n = 0;
    uint32_t calls0 = k.calls, ok0 = st.frames_ok, recv0 = st.frags_recv, drop0 = st.frames_dropped;
    feed_frag(r, 7, 4000, 0, HML_FLAG_START, 0, CHUNK);
    feed_frag(r, 7, 4001, 1, 0, 1, CHUNK);
    feed_frag(r, 7, 4001, 1, 0, 1, CHUNK); /* duplicate of frag 1 */
    feed_frag(r, 7, 4002, 2, HML_FLAG_END, 2, 32);
    hml_reasm_get_stats(r, &st);
    CHECK_EQ_U32(k.calls, calls0 + 1u);
    CHECK_EQ_SIZE(k.last_bytes, 160u);
    CHECK_EQ_U32(st.frames_ok, ok0 + 1u);
    CHECK_EQ_U32(st.frags_recv, recv0 + 4u);
    CHECK_EQ_U32(st.frames_dropped, drop0);
  }

  /* ---- missing middle fragment -> gaps+1, frames_dropped+1, no callback ---- */
  {
    k.n = 0;
    uint32_t calls0 = k.calls, drop0 = st.frames_dropped, gap0 = st.gaps, ok0 = st.frames_ok;
    feed_frag(r, 7, 5000, 0, HML_FLAG_START, 0, CHUNK);
    feed_frag(r, 7, 5002, 2, HML_FLAG_END, 2, 32); /* frag 1 lost */
    hml_reasm_get_stats(r, &st);
    /* a straggler may still be in flight, so nothing is counted and nothing is delivered */
    CHECK_EQ_U32(k.calls, calls0);
    CHECK_EQ_U32(st.frames_dropped, drop0);
    CHECK_EQ_U32(st.gaps, gap0);
    /* the next frame proves the straggler is gone: the frame is dropped, not delivered */
    feed_frag(r, 7, 6000, 0, HML_FLAG_START, 0, CHUNK);
    hml_reasm_get_stats(r, &st);
    CHECK_EQ_U32(k.calls, calls0);
    CHECK_EQ_U32(st.frames_dropped, drop0 + 1u);
    CHECK_EQ_U32(st.gaps, gap0 + 1u);
    CHECK_EQ_U32(st.frames_ok, ok0);
    /* and the stream recovers on the very next good frame */
    uint8_t want[3 * CHUNK];
    memset(want, 0, CHUNK);
    memset(want + CHUNK, 1, CHUNK);
    memset(want + 2 * CHUNK, 2, CHUNK - 32);
    feed_frag(r, 7, 6001, 1, 0, 1, CHUNK);
    feed_frag(r, 7, 6002, 2, HML_FLAG_END, 2, 32);
    hml_reasm_get_stats(r, &st);
    CHECK_EQ_U32(k.calls, calls0 + 1u);
    CHECK(memcmp(k.last, want, 2 * CHUNK + (CHUNK - 32)) == 0);
  }

  /* ---- EOF-only flush: zero-payload END, no data frame ---- */
  {
    k.n = 0;
    uint32_t calls0 = k.calls, ok0 = st.frames_ok, drop0 = st.frames_dropped, gap0 = st.gaps;
    feed_frag(r, 7, 7000, 0, HML_FLAG_END, 0, 0);
    hml_reasm_get_stats(r, &st);
    CHECK_EQ_U32(k.calls, calls0 + 1u);
    CHECK_EQ_SIZE(k.last_bytes, 0u);
    CHECK_EQ_U32(k.last_seq, 7000u);
    CHECK_EQ_U32(st.frames_ok, ok0); /* EOF-only is not a data frame */
    CHECK_EQ_U32(st.frames_dropped, drop0);
    CHECK_EQ_U32(st.gaps, gap0);
  }

  /* ---- garbage datagram -> proto_fail ---- */
  {
    uint32_t pf0 = st.proto_fail, recv0 = st.frags_recv;
    uint8_t junk[64];
    memset(junk, 0xA5, sizeof(junk));
    CHECK(hml_reasm_feed_datagram(r, junk, sizeof(junk)) == HML_ERR_PROTO);
    CHECK(hml_reasm_feed_datagram(r, junk, 4) == HML_ERR_PROTO);
    hml_reasm_get_stats(r, &st);
    CHECK_EQ_U32(st.proto_fail, pf0 + 2u);
    CHECK_EQ_U32(st.frags_recv, recv0);
    /* bad header crc -> crc_fail */
    build_dg(dg, 7, 8000, 0, 0, HML_FLAG_START, 0, junk, 8);
    dg[HML_OFF_HDR_CRC] ^= 0xFFu;
    CHECK(hml_reasm_feed_datagram(r, dg, HML_FRAME_HDR_SIZE + 10) == HML_ERR_CRC);
    /* bad payload crc -> crc_fail */
    uint32_t cf0 = st.crc_fail;
    build_dg(dg, 7, 8001, 0, 0, HML_FLAG_START, 0, junk, 8);
    dg[HML_FRAME_HDR_SIZE + 3] ^= 0x10u;
    CHECK(hml_reasm_feed_datagram(r, dg, HML_FRAME_HDR_SIZE + 10) == HML_ERR_CRC);
    hml_reasm_get_stats(r, &st);
    CHECK_EQ_U32(st.crc_fail, cf0 + 2u);
    /* NULL args */
    CHECK(hml_reasm_feed_datagram(NULL, junk, 4) == HML_ERR_ARG);
    CHECK(hml_reasm_feed_datagram(r, NULL, 4) == HML_ERR_ARG);
  }

  /* ---- new START while a frame is in flight drops the old one ---- */
  {
    uint32_t calls0 = k.calls, drop0 = st.frames_dropped;
    feed_frag(r, 7, 9000, 0, HML_FLAG_START, 0, CHUNK);
    feed_frag(r, 7, 9001, 1, 0, 1, CHUNK); /* frame left open */
    feed_frag(r, 7, 9500, 0, HML_FLAG_START, 0, CHUNK); /* new frame starts */
    hml_reasm_get_stats(r, &st);
    CHECK_EQ_U32(k.calls, calls0);
    CHECK_EQ_U32(st.frames_dropped, drop0 + 1u);
    /* the new frame completes normally */
    feed_frag(r, 7, 9501, 1, 0, 1, CHUNK);
    feed_frag(r, 7, 9502, 2, HML_FLAG_END, 2, 32);
    CHECK_EQ_U32(k.calls, calls0 + 1u);
    CHECK_EQ_U32(k.last_seq, 9500u);
  }

  /* ---- a fragment of the next frame is never merged into the open one ---- */
  {
    k.n = 0;
    hml_reasm_reset(r);
    hml_reasm_get_stats(r, &st);
    uint32_t calls0 = k.calls, drop0 = st.frames_dropped, gap0 = st.gaps;
    feed_frag(r, 4, 20000, 0, HML_FLAG_START, 0, CHUNK);
    /* the first fragment of the *next* frame: seq = base + idx does not hold */
    feed_frag(r, 4, 20002, 1, HML_FLAG_END, 1, CHUNK);
    hml_reasm_get_stats(r, &st);
    CHECK_EQ_U32(k.calls, calls0); /* a mixed frame is never delivered */
    CHECK_EQ_U32(st.frames_dropped, drop0 + 1u);
    CHECK_EQ_U32(st.gaps, gap0); /* the open frame was never closed, so no gap yet */
    /* the new frame finishes normally once its first fragment shows up */
    feed_frag(r, 4, 20001, 0, HML_FLAG_START, 2, CHUNK);
    hml_reasm_get_stats(r, &st);
    CHECK_EQ_U32(k.calls, calls0 + 1u);
    CHECK_EQ_U32(k.last_seq, 20001u);
    CHECK_EQ_U32(k.last_bytes, 2u * CHUNK);
    CHECK_EQ_U32(st.gaps, gap0); /* nothing was lost: the frame only arrived out of order */
  }

  /* ---- explicit reset abandons the in-flight frame without a counter ---- */
  {
    uint32_t calls0 = k.calls, drop0 = st.frames_dropped, gap0 = st.gaps;
    feed_frag(r, 7, 10000, 0, HML_FLAG_START, 0, CHUNK);
    hml_reasm_reset(r);
    hml_reasm_get_stats(r, &st);
    CHECK_EQ_U32(k.calls, calls0);
    CHECK_EQ_U32(st.frames_dropped, drop0);
    CHECK_EQ_U32(st.gaps, gap0);
  }

  /* ---- two interleaved stream slots ---- */
  {
    k.n = 0;
    uint32_t calls0 = k.calls, drop0 = st.frames_dropped, gap0 = st.gaps;
    feed_frag(r, 1, 11000, 0, HML_FLAG_START, 0, CHUNK);
    feed_frag(r, 2, 12000, 0, HML_FLAG_START, 1, CHUNK);
    feed_frag(r, 1, 11001, 1, 0, 2, CHUNK);
    feed_frag(r, 2, 12001, 1, HML_FLAG_END, 3, CHUNK);
    feed_frag(r, 1, 11001, 1, HML_FLAG_END, 2, CHUNK);
    feed_frag(r, 2, 12001, 1, HML_FLAG_END, 3, CHUNK);
    feed_frag(r, 1, 11001, 1, HML_FLAG_END, 4, CHUNK); /* duplicate of a delivered frame */
    hml_reasm_get_stats(r, &st);
    CHECK_EQ_U32(k.calls, calls0 + 2u);
    CHECK_EQ_U32(st.frames_dropped, drop0);
    CHECK_EQ_U32(st.gaps, gap0);
  }

  /* ---- frame larger than max_frame_bytes is dropped, not delivered ---- */
  {
    hml_reasm_ctx_t *small = NULL;
    sink k2;
    memset(&k2, 0, sizeof(k2));
    CHECK_STATUS(hml_reasm_open(&small, 100), HML_OK);
    hml_reasm_set_eof_cb(small, on_frame, &k2);
    uint32_t drop0 = 0;
    hml_reasm_get_stats(small, &st);
    drop0 = st.frames_dropped;
    feed_frag(small, 3, 13000, 0, HML_FLAG_START, 0, CHUNK);
    feed_frag(small, 3, 13001, 1, HML_FLAG_END, 1, CHUNK); /* 128 > 100 */
    hml_reasm_get_stats(small, &st);
    CHECK_EQ_U32(k2.calls, 0u);
    CHECK_EQ_U32(st.frames_dropped, drop0 + 1u);
    hml_reasm_close(small);
  }

  /* ---- 1000 sequential single-packet frames ---- */
  {
    k.n = 0;
    hml_reasm_reset(r);
    hml_reasm_get_stats(r, &st);
    uint32_t ok0 = st.frames_ok, drop0 = st.frames_dropped, gap0 = st.gaps;
    uint8_t pay[200];
    for (uint32_t f = 0; f < 1000; f++) {
      for (size_t i = 0; i < sizeof(pay); i++) pay[i] = (uint8_t)(f + i);
      build_dg(dg, 7, 20000u + f, 0, 0, (uint8_t)(HML_FLAG_START | HML_FLAG_END), f, pay,
               sizeof(pay));
      hml_status s2 = hml_reasm_feed_datagram(r, dg, HML_FRAME_HDR_SIZE + sizeof(pay) + 2);
      if (s2 != HML_OK) {
        printf("FAIL soak feed %u\n", f);
        g_fails++;
        break;
      }
    }
    hml_reasm_get_stats(r, &st);
    CHECK_EQ_U32(st.frames_ok - ok0, 1000u);
    CHECK_EQ_U32(k.n, 1000u);
    CHECK_EQ_U32(k.last_seq, 20000u + 999u);
    CHECK_EQ_U32(st.frames_dropped, drop0);
    CHECK_EQ_U32(st.gaps, gap0);
    printf("  soak: frames_ok=%u frags_recv=%u\n", (unsigned)st.frames_ok, (unsigned)st.frags_recv);
  }

  /* ---- caller-supplied static buffers (zero-heap MCU mode) ---- */
  {
    k.n = 0;
    size_t ctx_sz = hml_reasm_ctx_size();
    CHECK(ctx_sz > 0);
    uint8_t *static_ctx = (uint8_t *)calloc(1, ctx_sz);
    CHECK(static_ctx != NULL);
    uint8_t static_staging[512];
    uint8_t static_gather[512];
    hml_reasm_ctx_t *sr = NULL;

    /* arg validation */
    CHECK_STATUS(hml_reasm_open_buffers(NULL, NULL, 0, static_staging, static_gather, 512), HML_ERR_ARG);
    CHECK_STATUS(hml_reasm_open_buffers(&sr, NULL, 0, NULL, static_gather, 512), HML_ERR_ARG);
    CHECK_STATUS(hml_reasm_open_buffers(&sr, NULL, 0, static_staging, NULL, 512), HML_ERR_ARG);
    CHECK_STATUS(hml_reasm_open_buffers(&sr, NULL, 0, static_staging, static_gather, 0), HML_ERR_ARG);
    CHECK_STATUS(hml_reasm_open_buffers(&sr, static_ctx, ctx_sz - 1, static_staging, static_gather, 512), HML_ERR_ARG);

    /* open with caller memory (zero-heap) */
    CHECK_STATUS(hml_reasm_open_buffers(&sr, static_ctx, ctx_sz, static_staging, static_gather, 512), HML_OK);
    CHECK(sr != NULL);
    hml_reasm_set_eof_cb(sr, on_frame, &k);

    /* send 2-fragment frame into the static buffer */
    uint8_t p0[64], p1[32];
    memset(p0, 0xAA, sizeof(p0));
    memset(p1, 0xBB, sizeof(p1));
    uint8_t d0[HML_MAX_FRAG_SIZE], d1[HML_MAX_FRAG_SIZE];
    build_dg(d0, 9, 500, 0, 2, (uint8_t)HML_FLAG_START, 100, p0, sizeof(p0));
    build_dg(d1, 9, 501, 1, 2, (uint8_t)HML_FLAG_END, 105, p1, sizeof(p1));

    CHECK_STATUS(hml_reasm_feed_datagram(sr, d0, HML_FRAME_HDR_SIZE + sizeof(p0) + 2), HML_OK);
    CHECK_STATUS(hml_reasm_feed_datagram(sr, d1, HML_FRAME_HDR_SIZE + sizeof(p1) + 2), HML_OK);

    hml_reasm_get_stats(sr, &st);
    CHECK_EQ_U32(st.frames_ok, 1u);
    CHECK_EQ_U32(k.last_bytes, sizeof(p0) + sizeof(p1));
    CHECK_EQ_U32(k.last[0], 0xAA);
    CHECK_EQ_U32(k.last[63], 0xAA);
    CHECK_EQ_U32(k.last[64], 0xBB);
    CHECK_EQ_U32(k.last[95], 0xBB);

    /* close does not free caller's static buffers or ctx */
    hml_reasm_close(sr);
    free(static_ctx);
    printf("  static_reasm: PASS (frames_ok=1, bytes=96)\n");
  }

  hml_reasm_close(r);
  TEST_DONE();
}
