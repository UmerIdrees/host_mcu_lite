/*
 * hml_sock_lwip.h -- the lwIP-backed implementation of include/hml/hml_sock.h.
 *
 * The declarations in hml_sock.h are the contract and are not repeated here; this
 * header only carries the compile-time sizing the implementation needs. See
 * hml_sock_lwip.c for why these values are what they are.
 */
#ifndef HML_SOCK_LWIP_H
#define HML_SOCK_LWIP_H

#include "hml/hml_types.h"
#include "hml/hml_sock.h"

#ifdef __cplusplus
extern "C" {
#endif

/* hml_sock.h says one hml_sock_t handles one socket, and hml_udp.c holds exactly
 * one at a time. Two slots leaves room for a host-side receiver in the same image
 * without making the pool unbounded. */
#ifndef HML_SOCK_MAX_SOCKETS
#define HML_SOCK_MAX_SOCKETS 2u
#endif

/* The largest datagram this port will accept. 2048 is comfortably above the
 * library's HML_MAX_FRAG_SIZE, so no HML fragment is ever dropped for size, and a
 * fixed slot means the receive path never allocates. */
#ifndef HML_SOCK_RX_MAX
#define HML_SOCK_RX_MAX 2048u
#endif

/* Upper bound on how many times hml_sock_recv spins waiting for a datagram. Under
 * NO_SYS there is no other context to yield to, so an unbounded wait would be an
 * infinite loop with no way out. The bound turns "no datagram arrived" into a
 * reported HML_ERR_AGAIN instead of a hung run. */
#ifndef HML_SOCK_RX_MAX_SPINS
#define HML_SOCK_RX_MAX_SPINS 200000u
#endif

#ifdef __cplusplus
}
#endif

#endif /* HML_SOCK_LWIP_H */
