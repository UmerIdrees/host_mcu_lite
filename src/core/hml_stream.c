#include "hml/hml_stream.h"

#include <stdlib.h>

/*
 * Framer state. One malloc for the whole object, nothing else in the send path.
 *
 * Fragmentation is caller's job: every hml_stream_send_frag() emits exactly one
 * fragment. The stream only tracks the per-frame bookkeeping the wire needs:
 *   - next_seq        : per-fragment sequence, seeded from cfg.seq_start
 *   - frag_idx        : 0-based index of the next fragment, reset on first==true
 *   - frame_bytes     : running payload bytes of the frame being emitted
 * frag_cnt is never signalled (0 == "not signalled"), so a receiver must rely on
 * HML_FLAG_END to close a frame. frag_idx is still emitted correctly so that
 * reassembly, gap accounting and out-of-order tolerance work.
 *
 * Ownership: hml_stream_open() adopts the already-open hml_xport_t*; closing the
 * stream closes the transport (ops->close) and frees the stream.
 */
struct hml_stream {
  hml_xport_t *x;
  const hml_xport_ops *ops;
  hml_stream_cfg cfg;
  uint32_t next_seq;
  uint32_t frag_idx;
  size_t frame_bytes;
  hml_eof_cb cb;
  void *cb_ctx;
};

hml_status hml_stream_open(hml_stream_t **out, const hml_stream_cfg *cfg, hml_xport_t *xport) {
  if (!out || !cfg || !xport || !xport->ops) return HML_ERR_ARG;
  *out = NULL;
  if (!xport->ops->send_frag || !xport->ops->close) return HML_ERR_ARG;

  size_t max_frame = cfg->max_frame_bytes;
  if (max_frame == 0) max_frame = (size_t)HML_MAX_FRAME_SIZE;
  if (max_frame > (size_t)HML_MAX_FRAME_SIZE) max_frame = (size_t)HML_MAX_FRAME_SIZE;

  hml_stream_t *s = (hml_stream_t *)calloc(1, sizeof(*s));
  if (!s) return HML_ERR_NOMEM;

  s->x = xport;
  s->ops = xport->ops;
  s->cfg = *cfg;
  s->cfg.max_frame_bytes = max_frame;
  s->next_seq = cfg->seq_start;
  *out = s;
  return HML_OK;
}

hml_status hml_stream_close(hml_stream_t *s) {
  if (!s) return HML_ERR_ARG;
  if (s->ops && s->ops->close) s->ops->close(s->x);
  free(s);
  return HML_OK;
}

hml_status hml_stream_send_frag(hml_stream_t *s, const uint8_t *data, size_t len,
                                bool first, bool last, uint64_t ts_us) {
  if (!s) return HML_ERR_ARG;
  if (!data && len > 0) return HML_ERR_ARG;
  if (len > (size_t)HML_MAX_FRAG_PAYLOAD) return HML_ERR_TOOLARGE;

  size_t base = first ? 0u : s->frame_bytes;
  if (len > s->cfg.max_frame_bytes || base > s->cfg.max_frame_bytes - len) return HML_ERR_TOOLARGE;

  hml_frame_hdr h;
  h.stream_id = s->cfg.stream_id;
  h.seq = s->next_seq;
  h.frag_idx = (uint16_t)(first ? 0u : s->frag_idx);
  h.frag_cnt = 0; /* not signalled in v1 */
  h.ts_us = ts_us;
  h.flags = (uint8_t)((first ? HML_FLAG_START : 0u) | (last ? HML_FLAG_END : 0u));
  h.payload_len = (uint16_t)len;

  hml_status st = s->ops->send_frag(s->x, &h, data, len);
  if (st != HML_OK) return st; /* counters are not advanced on a failed send */

  s->next_seq++;
  s->frag_idx = first ? 1u : (uint32_t)h.frag_idx + 1u;
  s->frame_bytes = last ? 0u : base + len;
  return HML_OK;
}

hml_status hml_stream_flush_eof(hml_stream_t *s, uint64_t ts_us) {
  if (!s) return HML_ERR_ARG;

  hml_frame_hdr h;
  h.stream_id = s->cfg.stream_id;
  h.seq = s->next_seq;
  h.frag_idx = 0;
  h.frag_cnt = 0;
  h.ts_us = ts_us;
  h.flags = (uint8_t)HML_FLAG_END; /* EOF-only flush: no payload, no fragment count */
  h.payload_len = 0;

  hml_status st = s->ops->send_frag(s->x, &h, NULL, 0);
  if (st != HML_OK) return st;

  s->next_seq++;
  s->frame_bytes = 0;
  s->frag_idx = 0;
  return HML_OK;
}

hml_status hml_stream_set_eof_cb(hml_stream_t *s, hml_eof_cb cb, void *ctx) {
  if (!s) return HML_ERR_ARG;
  s->cb = cb;
  s->cb_ctx = ctx;
  return HML_OK;
}

uint32_t hml_stream_next_seq(const hml_stream_t *s) { return s ? s->next_seq : 0u; }
