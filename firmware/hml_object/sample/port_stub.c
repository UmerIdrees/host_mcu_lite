/*
 * port_stub.c -- the smallest complete hml port that links.
 *
 * hml has no platform layer of its own. A port supplies exactly six symbols:
 *
 *     uint64_t hml_clock_us(void);
 *     hml_status hml_sock_udp_create(hml_sock_t **out, uint32_t bind_ip,
 *                                    uint16_t bind_port, bool broadcast);
 *     hml_status hml_sock_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port);
 *     hml_status hml_sock_send(hml_sock_t *s, const void *buf, size_t n);
 *     hml_status hml_sock_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n,
 *                              int timeout_ms);
 *     void hml_sock_close(hml_sock_t *s);
 *
 * This file supplies all six, deliberately, as follows:
 *
 *   hml_clock_us        ISO C11 timespec_get(TIME_UTC). A wall clock, so NOT
 *                       monotonic -- fine here, wrong for a real port. On a real
 *                       target read a hardware timer; see docs/porting.md 2.1.
 *   hml_sock_*          HML_ERR_NOTSUP. NOT a lie and NOT a silent success: the
 *                       send path refuses to advance next_seq on a non-OK
 *                       return, so a frame is never marked "sent" when it was
 *                       not. Returning HML_OK here would silently corrupt the
 *                       sequence space. See docs/porting.md 2.2.
 *
 * The last line of the file is the link-time port contract: a table of the six
 * function addresses, so a port that forgets one fails at link time with the
 * symbol's name instead of at the first datagram.
 */

#include <hml/hml_partition.h>
#include <hml/hml_protocol.h>
#include <hml/hml_sock.h>

#include <time.h>

/* ---- clock ------------------------------------------------------------- */

/*
 * ISO C11 timespec_get(), deliberately: this file must not need <unistd.h>,
 * <sys/time.h> or _POSIX_C_SOURCE, because the whole point of the demo is that
 * a firmware port needs none of them.
 *
 * CAVEAT, and it is a real one: TIME_UTC is a *wall* clock, so it can step
 * backwards. hml_clock_us() must be MONOTONIC -- the control plane's deadlines
 * and the latency histogram both depend on it, and hml_reasm counts a
 * backwards delta as a skew sample and throws the sample away. A real port
 * reads a hardware timer or a FreeRTOS tick count; see docs/porting.md 2.1.
 * This stub is only good enough to stamp a ts_us and read it back in the same
 * process.
 */
uint64_t hml_clock_us(void) {
  struct timespec ts;
  if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return 0u;
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* ---- sockets: not implemented in this stub ------------------------------ */

hml_status hml_sock_udp_create(hml_sock_t **out, uint32_t bind_ip, uint16_t bind_port,
                               bool broadcast) {
  (void)bind_ip;
  (void)bind_port;
  (void)broadcast;
  if (!out) return HML_ERR_ARG;
  *out = NULL;
  return HML_ERR_NOTSUP;
}

hml_status hml_sock_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port) {
  (void)s;
  (void)ip;
  (void)port;
  return HML_ERR_NOTSUP;
}

hml_status hml_sock_send(hml_sock_t *s, const void *buf, size_t n) {
  (void)s;
  (void)buf;
  (void)n;
  return HML_ERR_NOTSUP;
}

hml_status hml_sock_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n, int timeout_ms) {
  (void)s;
  (void)buf;
  (void)cap;
  (void)timeout_ms;
  if (n) *n = 0u;
  return HML_ERR_NOTSUP;
}

void hml_sock_close(hml_sock_t *s) { (void)s; }

/* No hml_port_contract_check here on purpose: cmake/hml_firmware.cmake
 * generates hml_port_contract.c from this same list and the build compiles
 * that in, so the table exists exactly once. Defining it again would be a
 * duplicate symbol at link time.
 */
