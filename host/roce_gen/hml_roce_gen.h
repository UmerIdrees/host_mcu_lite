#ifndef HML_ROCE_GEN_H
#define HML_ROCE_GEN_H

/*
 * Shared pieces of the hml_gen_roce T1a sender CLI (M3 / T1a). Kept out of
 * hml_gen_roce.c so tests/m3 can drive the exact same code the binary runs.
 *
 * Two functions, both of which the hml_gen_roce CLI and tests/m3 use:
 *   hml_roce_gen_xport_open()  open the real RoCEv2 transport, or fall back to T0
 *   hml_roce_gen_frame()        deterministic synthetic frame + fragment transmit
 *
 * Nothing here crafts packets: the RoCEv2 datagram builder and the EOF metadata
 * record stay in src/transports/rocev2, and the framing stays in src/core.
 */

#include "hml/hml_protocol.h"
#include "hml/hml_stream.h"
#include "hml/hml_transport.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hml_roce_gen_xport {
  hml_xport_t xport;        /* the live transport: rocev2, or udp after a fallback */
  const hml_xport_ops *ops; /* ops in use, never NULL after a successful open */
  bool fallback;            /* true when the T0 UDP ops replaced a failed RoCEv2 open */
  hml_status roce_status;   /* what the RoCEv2 open returned (HML_OK when it was used) */
} hml_roce_gen_xport;

/*
 * Open the T1a transport for `dest`, optionally from a pinned UDP source port.
 *
 * Returns HML_OK when `g` owns an open transport. If the RoCEv2 open fails and
 * `allow_fallback` is set, the plain T0 UDP transport is opened for the same
 * destination instead and g->fallback is true; the failure is preserved in
 * g->roce_status. With `allow_fallback` clear the RoCEv2 failure is returned
 * verbatim and nothing is left open, so a caller can never mistake a degraded
 * transport for the real one.
 */
hml_status hml_roce_gen_xport_open(hml_roce_gen_xport *g, const hml_dest_cfg *dest,
                                   uint16_t stream_id, uint16_t src_port, bool allow_fallback);

/* Closes whatever hml_roce_gen_xport_open() opened. Safe on an unopened `g`. */
void hml_roce_gen_xport_close(hml_roce_gen_xport *g);

/*
 * Fill `n` bytes of the deterministic synthetic pattern for the frame at
 * `frame_index`. Identical to what the synthetic sensor hands hml_gen, so the
 * frame hash matches a T0 run byte for byte.
 */
void hml_roce_gen_frame_fill(uint8_t *frame, size_t n, uint64_t frame_index, uint32_t seed);

/*
 * Send one whole frame through `s` as fragments of at most
 * HML_MAX_FRAG_PAYLOAD bytes (<= the 1440-byte RoCEv2 payload cap). The last
 * fragment carries HML_FLAG_END, so the RoCEv2 transport emits the end-of-frame
 * metadata write and the T0 reassembler completes the frame. `chunks_out` (may
 * be NULL) receives the number of fragments sent.
 */
hml_status hml_roce_gen_frame_send(hml_stream_t *s, const uint8_t *frame, size_t n,
                                   size_t *chunks_out);

#ifdef __cplusplus
}
#endif

#endif /* HML_ROCE_GEN_H */
