#ifndef HML_TRANSPORT_H
#define HML_TRANSPORT_H

#include "hml_protocol.h"

/*
 * Pluggable transports via a vtable (UDP T0, RoCEv2 socket-craft T1a, others later).
 *
 * The core framer calls send_frag() once per logical fragment. The transport owns
 * datagram shaping: the UDP transport re-serializes the HML header, the RoCEv2
 * transport wraps the payload in BTH/RETH/ICRC instead. flush() forces any buffered
 * output out; for UDP it is a no-op. The transport must be thread-affine to the
 * stream that owns it (v1: single-threaded per stream).
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hml_xport_ops hml_xport_ops;

typedef struct hml_xport {
  const hml_xport_ops *ops;
  void *impl;             /* transport private data (allocated by open) */
} hml_xport_t;

typedef struct hml_xport_ops {
  const char *name;       /* "udp", "rocev2" */
  hml_status (*open)(hml_xport_t *x, const hml_dest_cfg *dest, uint16_t stream_id);
  hml_status (*send_frag)(hml_xport_t *x, const hml_frame_hdr *hdr,
                          const uint8_t *payload, size_t payload_len);
  hml_status (*flush)(hml_xport_t *x);
  void (*close)(hml_xport_t *x);
} hml_xport_ops;

/* Registry. Register once at init; lookup by name. */
HML_API hml_status hml_transport_register(const hml_xport_ops *ops);
HML_API const hml_xport_ops *hml_transport_lookup(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* HML_TRANSPORT_H */