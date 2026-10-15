#ifndef HML_SOCK_H
#define HML_SOCK_H

#include "hml_types.h"

/*
 * Minimal UDP socket abstraction provided by each port (ports/<port>).
 * IPv4 only for v1. One hml_sock_t handles one socket.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hml_sock hml_sock_t;

/* Create an unbound or bound (bind_port != 0) UDP socket. broadcast enables SO_BROADCAST. */
HML_API hml_status hml_sock_udp_create(hml_sock_t **out, uint32_t bind_ip, uint16_t bind_port,
                                       bool broadcast);

/* Set default destination for hml_sock_send. */
HML_API hml_status hml_sock_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port);

/* Send one datagram to the default destination. */
HML_API hml_status hml_sock_send(hml_sock_t *s, const void *buf, size_t n);

/* Receive one datagram. timeout_ms < 0 means block; 0 means poll (AGAIN if empty);
 * > 0 is a bounded wait. Returns HML_OK with n>0, HML_ERR_AGAIN (poll/timeout), or IO error. */
HML_API hml_status hml_sock_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n, int timeout_ms);

HML_API void hml_sock_close(hml_sock_t *s);

#ifdef __cplusplus
}
#endif

#endif /* HML_SOCK_H */