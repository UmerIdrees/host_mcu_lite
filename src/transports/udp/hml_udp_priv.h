#ifndef HML_UDP_PRIV_H
#define HML_UDP_PRIV_H

#include "hml/hml_port.h"
#include "hml/hml_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The "udp" hml_xport_ops vtable. */
const hml_xport_ops *hml_udp_ops(void);

/* Register "udp" in the hml_transport registry. Idempotent. */
hml_status hml_udp_register(void);

/*
 * hml_dest_cfg.ipv4 is network byte order while hml_sock_* takes a host-order
 * address (it applies htonl() itself). This is the portable conversion.
 */
static inline uint32_t hml_ipv4_ne_to_host(uint32_t ip_ne) {
  const uint16_t probe = 1;
  if (*(const uint8_t *)&probe == 1u) return ip_ne; /* little endian */
  return hml_bswap32(ip_ne);
}

#ifdef __cplusplus
}
#endif

#endif /* HML_UDP_PRIV_H */
