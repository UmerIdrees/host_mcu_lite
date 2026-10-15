/* M1: framer over a recording fake transport. */
#include "hml/hml_protocol.h"
#include "hml/hml_stream.h"
#include "hml_test_util.h"

#include <stdlib.h>
#include <string.h>

#define MAX_REC 64
#define CHUNK 1000

typedef struct {
  uint32_t n;
  hml_frame_hdr hdr[MAX_REC];
  uint8_t payload[MAX_REC][CHUNK];
  size_t len[MAX_REC];
  uint32_t opens;
  uint32_t closes;
  uint32_t flushes;
  hml_status send_rc[MAX_REC]; /* forced return for record i */
  uint32_t force_at;          /* index at which send_rc applies (0 == never) */
} rec;

static rec g;

static hml_status fake_open(hml_xport_t *x, const hml_dest_cfg *dest, uint16_t stream_id) {
  (void)dest;
  (void)stream_id;
  if (!x) return HML_ERR_ARG;
  g.opens++;
  x->impl = &g;
  return HML_OK;
}

static hml_status fake_send(hml_xport_t *x, const hml_frame_hdr *h, const uint8_t *payload,
                            size_t payload_len) {
  (void)x;
  if (g.n >= MAX_REC) return HML_ERR_NOMEM;
  uint32_t i = g.n;
  g.hdr[i] = *h;
  g.len[i] = payload_len;
  if (payload_len && payload) memcpy(g.payload[i], payload, payload_len);
  g.n++;
  if (g.force_at && i + 1 == g.force_at) return g.send_rc[i];
  return HML_OK;
}

static hml_status fake_flush(hml_xport_t *x) {
  (void)x;
  g.flushes++;
  return HML_OK;
}

static void fake_close(hml_xport_t *x) {
  (void)x;
  g.closes++;
}

static const hml_xport_ops g_ops = {"fake", fake_open, fake_send, fake_flush, fake_close};

int main(void) {
  hml_xport_t x;
  hml_stream_cfg cfg;
  hml_stream_t *s = NULL;
  uint8_t buf[CHUNK];
  memset(&x, 0, sizeof(x));
  memset(buf, 0xA5, sizeof(buf));
  x.ops = &g_ops;

  CHECK(hml_transport_lookup("udp") == NULL);
  CHECK(hml_transport_lookup(NULL) == NULL);
  CHECK(hml_transport_lookup("") == NULL);
  CHECK_STATUS(hml_transport_register(NULL), HML_ERR_ARG);
  CHECK_STATUS(hml_transport_register(&g_ops), HML_OK);
  CHECK_STATUS(hml_transport_register(&g_ops), HML_ERR_BUSY); /* duplicate name */
  CHECK(hml_transport_lookup("fake") == &g_ops);
  CHECK(hml_transport_lookup("nope") == NULL);

  cfg.stream_id = 9;
  cfg.max_frame_bytes = 2500; /* exactly 2 full chunks + one 500-byte tail */
  cfg.seq_start = 100;
  cfg.mtu_effective = 1500;
  CHECK_STATUS(hml_stream_open(&s, &cfg, &x), HML_OK);
  /* the stream adopts an already-open transport: it never calls ops->open itself */
  CHECK_EQ_U32(g.opens, 0u);
  CHECK_EQ_U32(hml_stream_next_seq(s), 100u);
  CHECK(hml_stream_next_seq(NULL) == 0u);

  /* ---- one frame in three fragments ---- */
  CHECK_STATUS(hml_stream_send_frag(s, buf, CHUNK, true, false, 11), HML_OK);
  CHECK_STATUS(hml_stream_send_frag(s, buf, CHUNK, false, false, 12), HML_OK);
  CHECK_STATUS(hml_stream_send_frag(s, buf, CHUNK - 500, false, true, 13), HML_OK);
  CHECK_EQ_U32(g.n, 3u);
  CHECK_EQ_U32(hml_stream_next_seq(s), 103u);
  CHECK_EQ_U32(g.hdr[0].seq, 100u);
  CHECK_EQ_U32(g.hdr[1].seq, 101u);
  CHECK_EQ_U32(g.hdr[2].seq, 102u);
  CHECK_EQ_U32(g.hdr[0].frag_idx, 0u);
  CHECK_EQ_U32(g.hdr[1].frag_idx, 1u);
  CHECK_EQ_U32(g.hdr[2].frag_idx, 2u);
  CHECK_EQ_U32(g.hdr[0].flags, HML_FLAG_START);
  CHECK_EQ_U32(g.hdr[1].flags, 0u);
  CHECK_EQ_U32(g.hdr[2].flags, HML_FLAG_END);
  CHECK_EQ_U32(g.hdr[0].frag_cnt, 0u); /* not signalled in v1 */
  CHECK_EQ_U32(g.hdr[0].stream_id, 9u);
  CHECK_EQ_U32(g.hdr[0].payload_len, CHUNK);
  CHECK_EQ_U32(g.hdr[2].payload_len, CHUNK - 500);
  CHECK_EQ_U32(g.hdr[0].ts_us, 11u);
  CHECK_EQ_U32(g.hdr[2].ts_us, 13u);
  CHECK_EQ_SIZE(g.len[2], (size_t)(CHUNK - 500));

  /* the frame counter restarted, so the next frame's index is 0 again */
  g.n = 0;
  CHECK_STATUS(hml_stream_send_frag(s, buf, 10, true, true, 14), HML_OK);
  CHECK_EQ_U32(g.hdr[0].frag_idx, 0u);
  CHECK_EQ_U32(g.hdr[0].flags, HML_FLAG_START | HML_FLAG_END);
  CHECK_EQ_U32(g.hdr[0].seq, 103u);
  g.n = 0;

  /* ---- argument validation ---- */
  CHECK_STATUS(hml_stream_send_frag(NULL, buf, 4, true, true, 0), HML_ERR_ARG);
  CHECK_STATUS(hml_stream_send_frag(s, NULL, 4, true, true, 0), HML_ERR_ARG);
  CHECK_STATUS(hml_stream_send_frag(s, NULL, 0, true, true, 0), HML_OK); /* zero payload ok */
  g.n = 0;

  /* ---- oversized chunk -> HML_ERR_TOOLARGE, nothing sent, no state change ---- */
  uint32_t seq_before = hml_stream_next_seq(s);
  static uint8_t big[HML_MAX_FRAG_PAYLOAD + 16];
  CHECK_STATUS(hml_stream_send_frag(s, big, sizeof(big), true, true, 0), HML_ERR_TOOLARGE);
  CHECK_EQ_U32(g.n, 0u);
  CHECK_EQ_U32(hml_stream_next_seq(s), seq_before);
  CHECK_STATUS(hml_stream_send_frag(s, big, HML_MAX_FRAG_PAYLOAD + 1, true, true, 0),
               HML_ERR_TOOLARGE);

  /* ---- max_frame_bytes enforcement across a frame (2500 bytes budget) ---- */
  CHECK_STATUS(hml_stream_send_frag(s, buf, CHUNK, true, false, 0), HML_OK);
  CHECK_STATUS(hml_stream_send_frag(s, buf, CHUNK, false, false, 0), HML_OK);
  g.n = 0;
  CHECK_STATUS(hml_stream_send_frag(s, buf, CHUNK, false, false, 0), HML_ERR_TOOLARGE);
  CHECK_EQ_U32(g.n, 0u); /* would exceed the 2500-byte frame budget */
  CHECK_STATUS(hml_stream_send_frag(s, buf, CHUNK - 500, false, true, 0), HML_OK);
  CHECK_EQ_U32(g.n, 1u);
  g.n = 0;

  /* a fresh frame resets the budget */
  CHECK_STATUS(hml_stream_send_frag(s, buf, CHUNK, true, true, 0), HML_OK);
  CHECK_EQ_U32(g.n, 1u);
  g.n = 0;

  /* ---- failed send does not consume a sequence number ---- */
  g.n = 0;
  g.force_at = 1;
  g.send_rc[0] = HML_ERR_AGAIN;
  seq_before = hml_stream_next_seq(s);
  CHECK_STATUS(hml_stream_send_frag(s, buf, 8, true, false, 0), HML_ERR_AGAIN);
  CHECK_EQ_U32(hml_stream_next_seq(s), seq_before);
  CHECK_STATUS(hml_stream_send_frag(s, buf, 8, true, false, 0), HML_OK);
  CHECK_EQ_U32(hml_stream_next_seq(s), seq_before + 1u);
  CHECK_EQ_U32(g.hdr[1].seq, seq_before); /* retried with the same seq */
  g.force_at = 0;
  g.n = 0;

  /* ---- flush_eof: zero payload, END only ---- */
  seq_before = hml_stream_next_seq(s);
  CHECK_STATUS(hml_stream_flush_eof(s, 77), HML_OK);
  CHECK_EQ_U32(g.n, 1u);
  CHECK_EQ_U32(g.len[0], 0u);
  CHECK_EQ_U32(g.hdr[0].payload_len, 0u);
  CHECK_EQ_U32(g.hdr[0].flags, HML_FLAG_END);
  CHECK_EQ_U32(g.hdr[0].seq, seq_before);
  CHECK_EQ_U32(g.hdr[0].frag_idx, 0u);
  CHECK_EQ_U32(g.hdr[0].frag_cnt, 0u);
  CHECK_EQ_U32(g.hdr[0].ts_us, 77u);
  CHECK_EQ_U32(hml_stream_next_seq(s), seq_before + 1u);
  g.n = 0;
  CHECK_STATUS(hml_stream_flush_eof(NULL, 0), HML_ERR_ARG);

  /* eof cb is storable */
  CHECK_STATUS(hml_stream_set_eof_cb(s, NULL, NULL), HML_OK);
  CHECK_STATUS(hml_stream_set_eof_cb(NULL, NULL, NULL), HML_ERR_ARG);

  /* ---- open validation ---- */
  CHECK_STATUS(hml_stream_open(NULL, &cfg, &x), HML_ERR_ARG);
  CHECK_STATUS(hml_stream_open(&s, NULL, &x), HML_ERR_ARG);
  CHECK_STATUS(hml_stream_open(&s, &cfg, NULL), HML_ERR_ARG);
  {
    hml_xport_t bad;
    memset(&bad, 0, sizeof(bad));
    CHECK_STATUS(hml_stream_open(&s, &cfg, &bad), HML_ERR_ARG);
  }

  /* ---- close forwards to ops->close ---- */
  CHECK_STATUS(hml_stream_close(s), HML_OK);
  CHECK_EQ_U32(g.closes, 1u);
  CHECK_STATUS(hml_stream_close(NULL), HML_ERR_ARG);

  /* ---- max_frame_bytes == 0 means "protocol maximum"; oversized clamps ---- */
  {
    hml_stream_cfg c2;
    hml_stream_t *s2 = NULL;
    hml_xport_t x2;
    memset(&x2, 0, sizeof(x2));
    x2.ops = &g_ops;
    c2.stream_id = 1;
    c2.max_frame_bytes = 0;
    c2.seq_start = 0;
    c2.mtu_effective = 0;
    g.n = 0;
    CHECK_STATUS(hml_stream_open(&s2, &c2, &x2), HML_OK);
    CHECK_STATUS(hml_stream_send_frag(s2, buf, CHUNK, true, true, 0), HML_OK);
    CHECK_EQ_U32(g.n, 1u);
    g.n = 0;
    CHECK_STATUS(hml_stream_close(s2), HML_OK);
  }

  TEST_DONE();
}
