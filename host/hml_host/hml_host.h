#ifndef HML_HOST_H
#define HML_HOST_H

#include "hml/hml_reasm.h"
#include "hml/hml_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hml_host hml_host_t;

/* Per-frame notification (fired from inside hml_host_poll). */
typedef void (*hml_host_frame_cb)(void *ctx, uint16_t stream_id, uint32_t frame_seq,
                                  size_t frame_bytes, uint32_t hash);

/* Raw frame data callback: delivered buffer is valid ONLY for the duration of the callback. */
typedef void (*hml_host_data_cb)(void *ctx, uint16_t stream_id, uint32_t frame_seq,
                                 const uint8_t *frame_data, size_t frame_bytes);

/* port == 0 selects HML_DATA_PORT; max_frame == 0 selects HML_MAX_FRAME_SIZE. */
hml_status hml_host_open(hml_host_t **out, uint16_t port, size_t max_frame);

/*
 * Consume every datagram that arrives within timeout_ms, then return.
 *   HML_OK          data was processed (or the wait window elapsed with traffic)
 *   HML_ERR_TIMEOUT nothing arrived within timeout_ms
 *   HML_ERR_IO      socket failure
 * timeout_ms < 0 blocks until the first datagram.
 */
hml_status hml_host_poll(hml_host_t *h, int timeout_ms);

/* All out params are optional (NULL tolerated). */
void hml_host_stats(const hml_host_t *h, hml_reasm_stats *st, uint64_t *frames, uint64_t *bytes,
                    uint64_t *eof, uint64_t *last_hash);

hml_status hml_host_set_frame_cb(hml_host_t *h, hml_host_frame_cb cb, void *ctx);
hml_status hml_host_set_data_cb(hml_host_t *h, hml_host_data_cb cb, void *ctx);

/* seq / bytes of the most recently delivered frame (0 until the first one). */
uint32_t hml_host_last_seq(const hml_host_t *h);
size_t hml_host_last_bytes(const hml_host_t *h);

/* Copy of the receiver's EOF-latency histogram (see hml_latency.h). */
hml_status hml_host_latency(const hml_host_t *h, hml_latency *out);

/*
 * Return whether p99 EOF latency is within budget_us. Always false when there
 * are no recorded samples or any skew samples were seen (a clock-domain
 * mismatch makes the number meaningless). Copies the p99 estimate out if
 * p99_out is non-NULL.
 */
bool hml_host_latency_ok(const hml_host_t *h, uint64_t budget_us, uint64_t *p99_out);

void hml_host_close(hml_host_t *h);

#ifdef __cplusplus
}
#endif

#endif /* HML_HOST_H */
