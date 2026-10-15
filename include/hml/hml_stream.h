#ifndef HML_STREAM_H
#define HML_STREAM_H

#include "hml_protocol.h"
#include "hml_transport.h"

/*
 * Sensor-agnostic data stream (framer). Streams fragments to a single destination
 * transport. One stream = one stream_id and one in-flight frame reassembly context
 * (single-frame reassembly window in v1). EOF is defined by HML_FLAG_END.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hml_stream hml_stream_t;
typedef struct hml_stream_cfg {
  uint16_t stream_id;
  size_t   max_frame_bytes; /* clamp; <= HML_MAX_FRAME_SIZE */
  uint32_t seq_start;       /* initial seq for fragments emitted by this stream */
  uint32_t mtu_effective;   /* informational; >= HML_MAX_FRAG_PAYLOAD+overhead */
} hml_stream_cfg;

typedef void (*hml_eof_cb)(void *ctx, uint16_t stream_id, uint32_t frame_seq,
                           const uint8_t *frame, size_t frame_bytes,
                           const hml_frame_hdr *final_hdr);

HML_API hml_status hml_stream_open(hml_stream_t **out, const hml_stream_cfg *cfg,
                                   hml_xport_t *xport);
HML_API hml_status hml_stream_close(hml_stream_t *s);

HML_API hml_status hml_stream_send_frag(hml_stream_t *s, const uint8_t *data, size_t len,
                                        bool first, bool last, uint64_t ts_us);
HML_API hml_status hml_stream_flush_eof(hml_stream_t *s, uint64_t ts_us); /* zero-payload EOF */

HML_API hml_status hml_stream_set_eof_cb(hml_stream_t *s, hml_eof_cb cb, void *ctx);

/* Sequence number to use for the next fragment sent by this stream. */
HML_API uint32_t hml_stream_next_seq(const hml_stream_t *s);

#ifdef __cplusplus
}
#endif

#endif /* HML_STREAM_H */