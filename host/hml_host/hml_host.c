#include "hml_host.h"

#include "hml/hml_protocol.h"
#include "hml/hml_sock.h"
#include "hml_synth_priv.h" /* hml_fnv1a32 (hml_synth is linked into this target) */

#include <stdlib.h>
#include <string.h>

/*
 * Path A CPU receiver: bind the T0 data port, feed every datagram to the
 * reassembler and, on each delivered frame, hash the assembled bytes so a test
 * harness can compare against the sender's hash without shipping the payload.
 */
struct hml_host {
  hml_sock_t *sock;
  hml_reasm_ctx_t *reasm;
  uint8_t *buf; /* HML_MAX_FRAG_SIZE */
  uint64_t frames;
  uint64_t bytes;
  uint64_t eof; /* EOF callbacks == frames_ok + EOF-only flushes */
  uint64_t last_hash;
  uint32_t last_seq;
  size_t last_bytes;
  hml_host_frame_cb cb;
  void *cb_ctx;
  hml_host_data_cb data_cb;
  void *data_cb_ctx;
};

static void host_on_frame(void *ctx, uint16_t stream_id, uint32_t frame_seq, const uint8_t *frame,
                          size_t frame_bytes, const hml_frame_hdr *final_hdr) {
  hml_host_t *h = (hml_host_t *)ctx;
  (void)stream_id;
  (void)final_hdr;
  h->eof++;
  h->last_seq = frame_seq;
  h->last_bytes = frame_bytes;
  h->last_hash = (uint64_t)hml_fnv1a32(frame, frame_bytes);
  if (frame_bytes > 0) {
    h->frames++;
    h->bytes += (uint64_t)frame_bytes;
  }
  if (h->data_cb) h->data_cb(h->data_cb_ctx, stream_id, frame_seq, frame, frame_bytes);
  if (h->cb) h->cb(h->cb_ctx, stream_id, frame_seq, frame_bytes, (uint32_t)h->last_hash);
}

hml_status hml_host_open(hml_host_t **out, uint16_t port, size_t max_frame) {
  if (!out) return HML_ERR_ARG;
  *out = NULL;
  if (port == 0) port = (uint16_t)HML_DATA_PORT;

  hml_host_t *h = (hml_host_t *)calloc(1, sizeof(*h));
  if (!h) return HML_ERR_NOMEM;
  h->buf = (uint8_t *)malloc((size_t)HML_MAX_FRAG_SIZE);
  if (!h->buf) {
    free(h);
    return HML_ERR_NOMEM;
  }

  hml_status st = hml_sock_udp_create(&h->sock, 0u, port, false);
  if (st == HML_OK) st = hml_reasm_open(&h->reasm, max_frame);
  if (st != HML_OK) {
    hml_sock_close(h->sock);
    free(h->buf);
    free(h);
    return st;
  }
  hml_reasm_set_eof_cb(h->reasm, host_on_frame, h);
  *out = h;
  return HML_OK;
}

hml_status hml_host_poll(hml_host_t *h, int timeout_ms) {
  if (!h) return HML_ERR_ARG;

  size_t n = 0;
  hml_status st = hml_sock_recv(h->sock, h->buf, (size_t)HML_MAX_FRAG_SIZE, &n, timeout_ms);
  if (st != HML_OK) {
    if (st == HML_ERR_TIMEOUT) return HML_ERR_TIMEOUT;
    if (st == HML_ERR_AGAIN) return HML_OK; /* interrupted or nothing left to read */
    return st;
  }
  (void)hml_reasm_feed_datagram(h->reasm, h->buf, n);

  /* drain whatever else is already queued (never blocks) */
  for (;;) {
    n = 0;
    st = hml_sock_recv(h->sock, h->buf, (size_t)HML_MAX_FRAG_SIZE, &n, 0);
    if (st != HML_OK) break;
    (void)hml_reasm_feed_datagram(h->reasm, h->buf, n);
  }
  return HML_OK;
}

void hml_host_stats(const hml_host_t *h, hml_reasm_stats *st, uint64_t *frames, uint64_t *bytes,
                    uint64_t *eof, uint64_t *last_hash) {
  if (!h) {
    if (st) memset(st, 0, sizeof(*st));
    if (frames) *frames = 0;
    if (bytes) *bytes = 0;
    if (eof) *eof = 0;
    if (last_hash) *last_hash = 0;
    return;
  }
  hml_reasm_get_stats(h->reasm, st);
  if (frames) *frames = h->frames;
  if (bytes) *bytes = h->bytes;
  if (eof) *eof = h->eof;
  if (last_hash) *last_hash = h->last_hash;
}

hml_status hml_host_set_frame_cb(hml_host_t *h, hml_host_frame_cb cb, void *ctx) {
  if (!h) return HML_ERR_ARG;
  h->cb = cb;
  h->cb_ctx = ctx;
  return HML_OK;
}

hml_status hml_host_set_data_cb(hml_host_t *h, hml_host_data_cb cb, void *ctx) {
  if (!h) return HML_ERR_ARG;
  h->data_cb = cb;
  h->data_cb_ctx = ctx;
  return HML_OK;
}

uint32_t hml_host_last_seq(const hml_host_t *h) { return h ? h->last_seq : 0u; }
size_t hml_host_last_bytes(const hml_host_t *h) { return h ? h->last_bytes : 0u; }

hml_status hml_host_latency(const hml_host_t *h, hml_latency *out) {
  if (!out) return HML_ERR_ARG;
  memset(out, 0, sizeof(*out));
  if (!h) return HML_ERR_ARG;
  hml_reasm_get_latency(h->reasm, out);
  return HML_OK;
}

bool hml_host_latency_ok(const hml_host_t *h, uint64_t budget_us, uint64_t *p99_out) {
  if (p99_out) *p99_out = 0;
  if (!h) return false;
  hml_latency lat;
  hml_reasm_get_latency(h->reasm, &lat);
  if (lat.n == 0u || lat.skew_samples != 0u) return false;
  uint64_t p99 = 0;
  if (hml_latency_percentile(&lat, 99, &p99) != HML_OK) return false;
  if (p99_out) *p99_out = p99;
  return p99 <= budget_us;
}

void hml_host_close(hml_host_t *h) {
  if (!h) return;
  hml_reasm_close(h->reasm);
  hml_sock_close(h->sock);
  free(h->buf);
  free(h);
}
