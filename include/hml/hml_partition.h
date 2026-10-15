#ifndef HML_PARTITION_H
#define HML_PARTITION_H

/*
 * hml_partition.h -- the link contract between hml and the platform below it.
 *
 * hml is partitioned into independently linkable components:
 *
 *   hml_protocol    codecs + CRCs          portable, no libc beyond C
 *   hml_core        framer, reassembly, latency, transport registry
 *                                            portable, depends on hml_protocol
 *   hml_control     discovery beacon + control-plane codec/server
 *                                            needs a port for sockets
 *   hml_udp         T0 UDP data plane      needs a port
 *   hml_rocev2      experimental RoCEv2   needs a port
 *   hml_port_<x>    hml_clock_us + hml_sock_*   YOU supply this
 *
 * The portable arms (hml_protocol, hml_core) call exactly two families of
 * platform symbol: hml_clock_us() and the five hml_sock_*() calls declared in
 * hml_sock.h. Nothing else. This header makes that contract machine-checkable
 * instead of a comment: see "Link-time presence check" below.
 *
 * Everything here is a header. There is no code, no allocation, no libc
 * dependency beyond <stdint.h>/<stddef.h>/<stdbool.h>.
 */

#include "hml_export.h"
#include "hml_sock.h"
#include "hml_types.h"
/* HML_HAS_POSIX_PORT below is derived from HML_PORT_POSIX, which hml_port.h is
 * the only thing that defines. Without this include the value would depend on
 * whether some *other* header happened to pull hml_port.h in first, so the same
 * translation unit could report 0 or 1 purely by include order. */
#include "hml_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The complete port contract, as an X-macro list carrying
 *   <return type>, <symbol name>, <parameter list>.
 *
 * Use it three ways:
 *   1. HML_PORT_DECLS      -> re-declare the prototypes (a stub file).
 *   2. hml_port_contract_t  -> a struct of correctly-typed function pointers.
 *   3. HML_PORT_FN_LIST     -> a static table, whose mere existence forces the
 *                              linker to resolve every symbol (see below).
 *
 * The parameter lists below are the ones in hml_sock.h verbatim. If you ever
 * change a port signature, change it here in the same commit: the table in
 * your port will stop compiling, which is the point.
 */
#define HML_PORT_FN_LIST(X)                                                  \
  X(uint64_t, hml_clock_us, (void))                                          \
  X(hml_status, hml_sock_udp_create,                                         \
    (hml_sock_t **out, uint32_t bind_ip, uint16_t bind_port, bool broadcast)) \
  X(hml_status, hml_sock_set_dest, (hml_sock_t *s, uint32_t ip, uint16_t port)) \
  X(hml_status, hml_sock_send, (hml_sock_t *s, const void *buf, size_t n))    \
  X(hml_status, hml_sock_recv,                                               \
    (hml_sock_t *s, void *buf, size_t cap, size_t *n, int timeout_ms))       \
  X(void, hml_sock_close, (hml_sock_t *s))

/* 1. Prototype re-declaration. Identical to the hml_sock.h declarations, so
 *    including both headers is legal C11 (a compatible redeclaration). */
#define HML_PORT_DECL(ret, name, args) ret name args;
HML_PORT_FN_LIST(HML_PORT_DECL)
#undef HML_PORT_DECL

/* 2. One struct, one member per required symbol, each with its real type. */
typedef struct hml_port_contract {
#define HML_PORT_MEMBER(ret, name, args) ret (*name) args;
  HML_PORT_FN_LIST(HML_PORT_MEMBER)
#undef HML_PORT_MEMBER
} hml_port_contract;

/*
 * 3. Link-time presence check.
 *
 * A firmware that cannot compile a port reference must not be able to link
 * against hml_core and discover the omission at the first datagram. Declare
 * this in exactly one translation unit of your port layer:
 *
 *     const hml_port_contract hml_port_contract_check = {
 *         HML_PORT_FN_LIST(HML_PORT_ENTRY)
 *     };
 *     #undef HML_PORT_ENTRY
 *
 * Every entry is a real function address, so a missing symbol is a link error
 * naming the symbol, not a runtime surprise. Cost: one data object of six
 * pointers, which the linker may discard if unreferenced.
 */
#define HML_PORT_ENTRY(ret, name, args) .name = name,

/*
 * Which partition a translation unit is being built against. These are set by
 * hml's own build; a consumer only reads them. They are informational: nothing
 * in the library branches on them.
 */
#if defined(HML_PARTITION_SHARED)
#  define HML_LINKAGE_NAME "shared"
#else
#  define HML_LINKAGE_NAME "static"
#endif

/* 1 when this build ships the POSIX port (i.e. hml_port.h auto-tagged POSIX). */
#if defined(HML_PORT_POSIX)
#  define HML_HAS_POSIX_PORT 1
#else
#  define HML_HAS_POSIX_PORT 0
#endif

/*
 * Memory the platform owes hml. hml_core/hml_protocol call these four and
 * nothing else from the C library; on a freestanding target they are your
 * freestanding-runtime hooks (newlib nano, picolibc, or your own arena).
 *   memset   frame/header zeroing and buffer clears
 *   memcpy   payload copies in the framer and the reassembly gather
 *   malloc   one allocation per hml_stream_t / hml_reasm_ctx_t / transport
 *   free     the matching release
 * No printf, no strtol, no time(), no stdio, no locale, no threads.
 */
#define HML_LIBC_REQUIREMENTS "memset memcpy malloc free"

#ifdef __cplusplus
}
#endif

#endif /* HML_PARTITION_H */
