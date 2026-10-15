#ifndef HML_REASM_H
#define HML_REASM_H

#include "hml_latency.h"
#include "hml_protocol.h"

/*
 * Frame reassembly (host-side receive, T0 and RoCEv2 demuxed fragments). Holds
 * a single frame window per (stream_id) in v1; gap counters exposed for metrics.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hml_reasm_ctx hml_reasm_ctx_t;
typedef struct hml_reasm_stats {
  uint32_t frames_ok;
  uint32_t frames_dropped;
  uint32_t frags_recv;
  uint32_t gaps;        /* missing fragment detected */
  uint32_t crc_fail;
  uint32_t proto_fail;
} hml_reasm_stats;

typedef void (*hml_reasm_eof_cb)(void *ctx, uint16_t stream_id, uint32_t frame_seq,
                                 const uint8_t *frame, size_t frame_bytes,
                                 const hml_frame_hdr *final_hdr);

HML_API hml_status hml_reasm_open(hml_reasm_ctx_t **out, size_t max_frame_bytes);
HML_API size_t hml_reasm_ctx_size(void);
HML_API hml_status hml_reasm_open_buffers(hml_reasm_ctx_t **out, void *ctx_mem, size_t ctx_mem_size,
                                         uint8_t *staging_buf, uint8_t *gather_buf,
                                         size_t capacity);
HML_API void hml_reasm_close(hml_reasm_ctx_t *r);

HML_API hml_status hml_reasm_feed_datagram(hml_reasm_ctx_t *r, const uint8_t *dg, size_t len);
HML_API void hml_reasm_set_eof_cb(hml_reasm_ctx_t *r, hml_reasm_eof_cb cb, void *ctx);
HML_API void hml_reasm_reset(hml_reasm_ctx_t *r); /* reset current in-flight frame */
HML_API void hml_reasm_get_stats(const hml_reasm_ctx_t *r, hml_reasm_stats *st);

/* End-of-frame latency histogram (see hml_latency.h for the clock-domain caveat). */
HML_API void hml_reasm_get_latency(const hml_reasm_ctx_t *r, hml_latency *out);

#ifdef __cplusplus
}
#endif

#endif /* HML_REASM_H */