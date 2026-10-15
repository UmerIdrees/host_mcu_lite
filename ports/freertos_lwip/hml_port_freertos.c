/*
 * hml_port_freertos.c -- FreeRTOS + lwIP platform port for Host MCU Lite (hml).
 *
 * Implements the frozen port contract: hml_clock_us() and the whole hml_sock API
 * (hml_sock_udp_create / set_dest / send / recv / close) over lwIP raw UDP PCBs.
 *
 * TWO CODE PATHS
 * --------------
 * 1. Real path -- compiled only when the build system defines
 *    HML_PORT_HAVE_FREERTOS (set by a real target CMake / idf.py build after it
 *    has put FreeRTOS + lwIP on the include path):
 *      * clock   : xTaskGetTickCount() / portTICK_PERIOD_MS
 *      * sockets : lwIP raw API (udp_new / udp_bind / udp_connect / udp_send /
 *                  udp_recv) bridged into the polling hml_sock_recv() model with
 *                  a FreeRTOS queue: the lwIP tcpip thread pushes pbuf* into the
 *                  queue from the udp_recv callback, the caller dequeues with the
 *                  timeout it asked for.
 * 2. Stub path -- the default everywhere else (this repo's Linux CI box, where
 *    neither FreeRTOS nor lwIP is installed). Sockets answer HML_ERR_NOTSUP and
 *    the clock stays monotonic so control-plane timestamps still make sense.
 *    No header that may be missing is ever included on this path.
 *
 * CLOCK SOURCE IN THE STUB (documented, per the port contract)
 * ------------------------------------------------------------
 *   C11 timespec_get(TIME_UTC)  when __STDC_VERSION__ >= 201112L. The result is
 *   clamped to (previous + 1) so two back-to-back calls can never return the
 *   same value; the clock therefore never goes backwards and never repeats.
 *   Fallback (pre-C11 toolchain): a static counter incremented once per call.
 *   The clamp/counter is not thread-safe by design -- a stub is single-threaded.
 *
 * ONE PORT API PER BINARY (duplicate-symbol rule)
 * ------------------------------------------------
 * hml_clock_us() and hml_sock_*() are also defined in ports/posix, so every
 * public definition in this file is guarded with `#if !defined(HML_PORT_POSIX)`.
 * On a host build HML_PORT_POSIX is auto-detected by <hml/hml_port.h> and this
 * file contributes no public symbol, which lets the API-surface test link all
 * three port libraries into one binary. On a real FreeRTOS target
 * HML_PORT_POSIX is undefined (no __unix__/__linux__) and this file owns the
 * public API. The prefixed `hml_port_freertos_*` entry points below are always
 * compiled and always answer with *this port's* behaviour, so the same test can
 * assert the stub contract on a host. The public API is never renamed.
 */

#include "hml/hml_port.h"
#include "hml/hml_protocol.h"
#include "hml/hml_sock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* A full data datagram must fit the 1500-byte MTU budget with room for the
 * 20-byte IPv4 and 8-byte UDP headers (1458 + 28 = 1486). */
_Static_assert(HML_FRAME_HDR_SIZE + HML_MAX_FRAG_PAYLOAD + HML_TRAIL_CRC_SIZE <= 1500u,
               "HML datagram exceeds the 1500-byte MTU budget");
_Static_assert(HML_MAX_FRAG_SIZE + 20u + 8u <= 1500u,
               "HML datagram + IPv4/UDP headers exceeds the 1500-byte MTU budget");

/* ------------------------------------------------------------------------- */
/* real path: FreeRTOS + lwIP                                                */
/* ------------------------------------------------------------------------- */
#if defined(HML_PORT_HAVE_FREERTOS)

#include <stdlib.h> /* calloc/free from the target's heap (newlib / ESP-IDF) */

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

/* Queued receive item: the lwIP pbuf chain plus its total length. pbuf pointers
 * are stable across the chain, so the owning queue item may hold a raw pointer. */
typedef struct frt_rx_item {
  struct pbuf *p;
  u16_t len;
} frt_rx_item;

struct hml_sock {
  struct udp_pcb *pcb;
  QueueHandle_t rxq;
  ip4_addr_t dest_ip;
  u16_t dest_port;
  uint8_t bound;
};

static void frt_rx_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                      const ip_addr_t *addr, u16_t port) {
  hml_sock_t *s = (hml_sock_t *)arg;
  (void)pcb; (void)addr; (void)port;
  if (s == NULL || s->rxq == NULL) {
    if (p != NULL) pbuf_free(p);
    return;
  }
  frt_rx_item item;
  item.p = p;
  item.len = (u16_t)p->tot_len;
  /* No wait: the tcpip thread must not block. A full queue means "drop now" and
   * is accounted by the caller's gap/frame_gap counters. */
  if (xQueueSend(s->rxq, &item, 0) != pdTRUE) pbuf_free(p);
}

static TickType_t frt_ticks(int timeout_ms) {
  if (timeout_ms < 0) return portMAX_DELAY;
  if (timeout_ms == 0) return (TickType_t)0;
  TickType_t t = pdMS_TO_TICKS((uint32_t)timeout_ms);
  return (t == (TickType_t)0) ? (TickType_t)1 : t;
}

static hml_status frt_udp_create(hml_sock_t **out, uint32_t bind_ip, uint16_t bind_port,
                                 bool broadcast) {
  if (out == NULL) return HML_ERR_ARG;
  *out = NULL;

  hml_sock_t *s = (hml_sock_t *)calloc(1, sizeof(*s));
  if (s == NULL) return HML_ERR_NOMEM;
  s->rxq = xQueueCreate(8, sizeof(frt_rx_item)); /* 8 in-flight datagrams max */
  if (s->rxq == NULL) {
    free(s);
    return HML_ERR_NOMEM;
  }
  s->pcb = udp_new();
  if (s->pcb == NULL) {
    vQueueDelete(s->rxq);
    free(s);
    return HML_ERR_IO;
  }
#if defined(PCB_FLAG_BROADCAST)
  if (broadcast) s->pcb->flags |= PCB_FLAG_BROADCAST;
#else
  (void)broadcast; /* older lwIP: a raw pcb may always send to a broadcast addr */
#endif

  if (bind_port != 0) {
    ip4_addr_t a;
    a.addr = htonl(bind_ip); /* ip4_addr_t.addr is kept in network byte order */
    if (udp_bind(s->pcb, &a, bind_port) != ERR_OK) {
      udp_remove(s->pcb);
      vQueueDelete(s->rxq);
      free(s);
      return HML_ERR_IO;
    }
    s->bound = 1;
  }
  udp_recv(s->pcb, frt_rx_cb, s);
  *out = s;
  return HML_OK;
}

static hml_status frt_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port) {
  if (s == NULL) return HML_ERR_ARG;
  if (s->pcb == NULL) return HML_ERR_STATE;
  s->dest_ip.addr = htonl(ip);
  s->dest_port = port;
  return HML_OK;
}

static hml_status frt_send(hml_sock_t *s, const void *buf, size_t n) {
  if (s == NULL || (buf == NULL && n != 0)) return HML_ERR_ARG;
  if (s->pcb == NULL) return HML_ERR_STATE;
  if (n > 0xffffu) return HML_ERR_TOOLARGE;
  if (n == 0) return HML_OK;

  struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
  if (p == NULL) return HML_ERR_NOMEM;
  if (pbuf_take(p, buf, (u16_t)n) != ERR_OK) {
    pbuf_free(p);
    return HML_ERR_IO;
  }
  /* udp_send() takes its own reference on success and frees on failure, so the
   * caller must always release exactly one reference. */
  ip_addr_t dip;
  memcpy(&dip, &s->dest_ip, sizeof(dip));
  err_t e = udp_send(s->pcb, p, s->dest_port, &dip, s->dest_port);
  pbuf_free(p);
  return (e == ERR_OK) ? HML_OK : HML_ERR_IO;
}

static hml_status frt_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n, int timeout_ms) {
  if (s == NULL || buf == NULL || n == NULL) return HML_ERR_ARG;
  *n = 0;
  if (s->pcb == NULL) return HML_ERR_STATE;

  frt_rx_item item;
  if (xQueueReceive(s->rxq, &item, frt_ticks(timeout_ms)) != pdTRUE)
    return (timeout_ms == 0) ? HML_ERR_AGAIN : HML_ERR_TIMEOUT;

  size_t total = (size_t)item.len;
  size_t copy = (total < cap) ? total : cap;
  if (copy > 0) pbuf_copy_partial(item.p, buf, (u16_t)copy, 0);
  pbuf_free(item.p);
  *n = copy;
  return HML_OK;
}

static void frt_close(hml_sock_t *s) {
  if (s == NULL) return;
  if (s->pcb != NULL) udp_remove(s->pcb);
  if (s->rxq != NULL) vQueueDelete(s->rxq);
  free(s);
}

static uint64_t frt_clock_us(void) {
  /* Tick resolution only (typically 1 ms): monotonic but not strictly
   * increasing. The protocol timestamps a datagram once per fragment, so tick
   * granularity is acceptable on an MCU; add a sub-tick counter if a port needs
   * finer resolution. */
  TickType_t t = xTaskGetTickCount();
  return ((uint64_t)t * 1000000ull) / (uint64_t)portTICK_PERIOD_MS;
}

#endif /* HML_PORT_HAVE_FREERTOS */

/* ------------------------------------------------------------------------- */
/* stub path: no RTOS, no lwIP                                              */
/* ------------------------------------------------------------------------- */

static uint64_t frt_stub_clock_us(void) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  static uint64_t last; /* single-threaded stub */
  struct timespec ts;
  if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return ++last;
  uint64_t v = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
  if (v <= last) v = last + 1; /* never repeat, never go backwards */
  last = v;
  return v;
#else
  static uint64_t counter;
  return ++counter;
#endif
}

/* Stubs validate arguments exactly like the POSIX port (so callers see the
 * same HML_ERR_ARG contract everywhere) and then report "not on this port". */
static hml_status frt_stub_create(hml_sock_t **out, uint32_t bind_ip, uint16_t bind_port,
                                   bool broadcast) {
  (void)bind_ip; (void)bind_port; (void)broadcast;
  if (out == NULL) return HML_ERR_ARG;
  *out = NULL;
  return HML_ERR_NOTSUP;
}

static hml_status frt_stub_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port) {
  (void)ip; (void)port;
  if (s == NULL) return HML_ERR_ARG;
  return HML_ERR_NOTSUP;
}

static hml_status frt_stub_send(hml_sock_t *s, const void *buf, size_t n) {
  (void)buf; (void)n;
  if (s == NULL || (buf == NULL && n != 0)) return HML_ERR_ARG;
  return HML_ERR_NOTSUP;
}

static hml_status frt_stub_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n, int timeout_ms) {
  (void)buf; (void)cap; (void)timeout_ms;
  if (s == NULL || buf == NULL || n == NULL) return HML_ERR_ARG;
  if (n != NULL) *n = 0; /* no datagram, and buf is never written */
  return HML_ERR_NOTSUP;
}

static void frt_stub_close(hml_sock_t *s) { (void)s; /* nothing was allocated */ }

/* ------------------------------------------------------------------------- */
/* port-private surface: always compiled, always this port's behaviour        */
/* ------------------------------------------------------------------------- */

hml_status hml_port_freertos_sock_udp_create(hml_sock_t **out, uint32_t bind_ip,
                                             uint16_t bind_port, bool broadcast) {
#if defined(HML_PORT_HAVE_FREERTOS)
  return frt_udp_create(out, bind_ip, bind_port, broadcast);
#else
  return frt_stub_create(out, bind_ip, bind_port, broadcast);
#endif
}

hml_status hml_port_freertos_sock_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port) {
#if defined(HML_PORT_HAVE_FREERTOS)
  return frt_set_dest(s, ip, port);
#else
  return frt_stub_set_dest(s, ip, port);
#endif
}

hml_status hml_port_freertos_sock_send(hml_sock_t *s, const void *buf, size_t n) {
#if defined(HML_PORT_HAVE_FREERTOS)
  return frt_send(s, buf, n);
#else
  return frt_stub_send(s, buf, n);
#endif
}

hml_status hml_port_freertos_sock_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n,
                                       int timeout_ms) {
#if defined(HML_PORT_HAVE_FREERTOS)
  return frt_recv(s, buf, cap, n, timeout_ms);
#else
  return frt_stub_recv(s, buf, cap, n, timeout_ms);
#endif
}

void hml_port_freertos_sock_close(hml_sock_t *s) {
#if defined(HML_PORT_HAVE_FREERTOS)
  frt_close(s);
#else
  frt_stub_close(s);
#endif
}

uint64_t hml_port_freertos_clock_us(void) {
#if defined(HML_PORT_HAVE_FREERTOS)
  return frt_clock_us();
#else
  return frt_stub_clock_us();
#endif
}

/* ------------------------------------------------------------------------- */
/* public frozen API -- owned by this file only on a FreeRTOS target          */
/* ------------------------------------------------------------------------- */
#if !defined(HML_PORT_POSIX)

uint64_t hml_clock_us(void) { return hml_port_freertos_clock_us(); }

hml_status hml_sock_udp_create(hml_sock_t **out, uint32_t bind_ip, uint16_t bind_port,
                               bool broadcast) {
  return hml_port_freertos_sock_udp_create(out, bind_ip, bind_port, broadcast);
}

hml_status hml_sock_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port) {
  return hml_port_freertos_sock_set_dest(s, ip, port);
}

hml_status hml_sock_send(hml_sock_t *s, const void *buf, size_t n) {
  return hml_port_freertos_sock_send(s, buf, n);
}

hml_status hml_sock_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n, int timeout_ms) {
  return hml_port_freertos_sock_recv(s, buf, cap, n, timeout_ms);
}

void hml_sock_close(hml_sock_t *s) { hml_port_freertos_sock_close(s); }

#endif /* !HML_PORT_POSIX */
