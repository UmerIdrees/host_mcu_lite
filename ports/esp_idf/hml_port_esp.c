/*
 * hml_port_esp.c -- ESP-IDF platform port for Host MCU Lite (hml).
 *
 * Same frozen contract as every other port: hml_clock_us() plus the whole
 * hml_sock API (hml_sock_udp_create / set_dest / send / recv / close). ESP-IDF
 * already runs lwIP, so the data path is the same raw-lwIP-PCB shape as the
 * FreeRTOS port, with a FreeRTOS queue bridging lwIP's tcpip thread into the
 * polling hml_sock_recv() model.
 *
 * IDF COMPONENTS THE REAL PATH NEEDS (real path == HML_PORT_HAVE_ESP_IDF)
 * -----------------------------------------------------------------------
 *   esp_timer  : esp_timer_get_time() for the microsecond clock
 *                (free, always compiled in; no CONFIG_LWIP* dependency)
 *   lwip       : lwip/udp.h, lwip/pbuf.h, lwip/ip_addr.h, lwip/err.h
 *   freertos   : FreeRTOS.h + queue.h (xQueueCreate/xQueueSend/xQueueReceive),
 *                vQueueDelete
 *   In CMakeLists.txt of the real firmware add:
 *     idf_component_register(SRCS "hml_port_esp.c" INCLUDE_DIRS "include"
 *                            REQUIRES esp_timer lwip freertos)
 *   NOTE: this file is also compiled by the host CMake build as a stub, where
 *   NONE of the headers above are included.
 *
 * CONFIG KNOBS THAT AFFECT THIS PORT
 * ----------------------------------
 *   CONFIG_LWIP_UDP                        must be 1 (UDP raw PCBs)
 *   CONFIG_LWIP_UDP_RAW                    alternative: bind a raw socket instead
 *   CONFIG_LWIP_MAX_UDP_PBUF? (not a real knob) - buffer budget is instead:
 *   CONFIG_LWIP_TCPIP_RECVMBOX_SIZE         must fit the app's RX rate
 *   CONFIG_LWIP_TCPIP_RECVMBOX_SIZE / CONFIG_LWIP_TCPIP_MBOX_SIZE
 *   CONFIG_LWIP_SO_RCVBUF                   per-socket receive buffer (lwIP
 *                                          honours SO_RCVBUF when not using
 *                                          pbuf malloc pools)
 *   CONFIG_LWIP_TCPIP_CORE_LOCKING_INPUT    optional; without it udp_recv()
 *                                           callbacks already run on the tcpip
 *                                           task, which is where the queue push
 *                                           happens
 *   CONFIG_FREERTOS_HZ                     sets the tick; the real path does not
 *                                          depend on it (esp_timer, not ticks)
 *   CONFIG_SPIRAM / heap size              the receive queue holds up to 8 pbuf
 *                                          pointers + 1 control buffer
 *   Data plane on ESP is "control-first": the sensor-bridge control plane and
 *   discovery are what this port is validated for. T1a/T2 are not claimed.
 *
 * CLOCK SOURCE IN THE STUB (documented, per the port contract)
 * ------------------------------------------------------------
 *   C11 timespec_get(TIME_UTC) when __STDC_VERSION__ >= 201112L, clamped to
 *   (previous + 1) so the value never repeats or goes backwards; otherwise a
 *   static counter incremented once per call. Not thread-safe by design.
 *
 * ONE PORT API PER BINARY (duplicate-symbol rule)
 * ------------------------------------------------
 * hml_clock_us() / hml_sock_*() also live in ports/posix, so every public
 * definition here is guarded with `#if !defined(HML_PORT_POSIX)`. On a host
 * build this file contributes no public symbol (the API-surface test links all
 * three port libraries together); on a real ESP-IDF build HML_PORT_POSIX is
 * undefined and this file owns the public API. The prefixed
 * `hml_port_esp_*` entry points are always compiled and always report this
 * port's behaviour. The public API is never renamed.
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
/* real path: ESP-IDF (esp_timer + lwip + freertos)                          */
/* ------------------------------------------------------------------------- */
#if defined(HML_PORT_HAVE_ESP_IDF)

#include <stdlib.h>

#include "esp_timer.h"  /* esp_timer_get_time() */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

typedef struct esp_rx_item {
  struct pbuf *p;
  u16_t len;
} esp_rx_item;

struct hml_sock {
  struct udp_pcb *pcb;
  QueueHandle_t rxq;
  ip4_addr_t dest_ip;
  u16_t dest_port;
};

static void esp_rx_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
                      u16_t port) {
  hml_sock_t *s = (hml_sock_t *)arg;
  (void)pcb; (void)addr; (void)port;
  if (s == NULL || s->rxq == NULL) {
    if (p != NULL) pbuf_free(p);
    return;
  }
  esp_rx_item item;
  item.p = p;
  item.len = (u16_t)p->tot_len;
  if (xQueueSend(s->rxq, &item, 0) != pdTRUE) pbuf_free(p); /* drop when full */
}

static TickType_t esp_ticks(int timeout_ms) {
  if (timeout_ms < 0) return portMAX_DELAY;
  if (timeout_ms == 0) return (TickType_t)0;
  TickType_t t = pdMS_TO_TICKS((uint32_t)timeout_ms);
  return (t == (TickType_t)0) ? (TickType_t)1 : t;
}

static hml_status esp_udp_create(hml_sock_t **out, uint32_t bind_ip, uint16_t bind_port,
                                 bool broadcast) {
  if (out == NULL) return HML_ERR_ARG;
  *out = NULL;

  hml_sock_t *s = (hml_sock_t *)calloc(1, sizeof(*s));
  if (s == NULL) return HML_ERR_NOMEM;
  s->rxq = xQueueCreate(8, sizeof(esp_rx_item));
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
  (void)broadcast;
#endif

  if (bind_port != 0) {
    ip4_addr_t a;
    a.addr = htonl(bind_ip);
    if (udp_bind(s->pcb, &a, bind_port) != ERR_OK) {
      udp_remove(s->pcb);
      vQueueDelete(s->rxq);
      free(s);
      return HML_ERR_IO;
    }
  }
  udp_recv(s->pcb, esp_rx_cb, s);
  *out = s;
  return HML_OK;
}

static hml_status esp_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port) {
  if (s == NULL) return HML_ERR_ARG;
  if (s->pcb == NULL) return HML_ERR_STATE;
  s->dest_ip.addr = htonl(ip);
  s->dest_port = port;
  return HML_OK;
}

static hml_status esp_send(hml_sock_t *s, const void *buf, size_t n) {
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
  ip_addr_t dip;
  memcpy(&dip, &s->dest_ip, sizeof(dip));
  /* udp_send() takes its own reference on success and frees on failure, so the
   * caller always releases exactly one reference. */
  err_t e = udp_send(s->pcb, p, s->dest_port, &dip, s->dest_port);
  pbuf_free(p);
  return (e == ERR_OK) ? HML_OK : HML_ERR_IO;
}

static hml_status esp_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n, int timeout_ms) {
  if (s == NULL || buf == NULL || n == NULL) return HML_ERR_ARG;
  *n = 0;
  if (s->pcb == NULL) return HML_ERR_STATE;

  esp_rx_item item;
  if (xQueueReceive(s->rxq, &item, esp_ticks(timeout_ms)) != pdTRUE)
    return (timeout_ms == 0) ? HML_ERR_AGAIN : HML_ERR_TIMEOUT;

  size_t total = (size_t)item.len;
  size_t copy = (total < cap) ? total : cap;
  if (copy > 0) pbuf_copy_partial(item.p, buf, (u16_t)copy, 0);
  pbuf_free(item.p);
  *n = copy;
  return HML_OK;
}

static void esp_close(hml_sock_t *s) {
  if (s == NULL) return;
  if (s->pcb != NULL) udp_remove(s->pcb);
  if (s->rxq != NULL) vQueueDelete(s->rxq);
  free(s);
}

static uint64_t esp_clock_us(void) {
  /* esp_timer counts a monotonic int64 microseconds since boot. */
  return (uint64_t)(esp_timer_get_time() / 1000ll);
}

#endif /* HML_PORT_HAVE_ESP_IDF */

/* ------------------------------------------------------------------------- */
/* stub path: no ESP-IDF, no lwIP                                           */
/* ------------------------------------------------------------------------- */

static uint64_t esp_stub_clock_us(void) {
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  static uint64_t last; /* single-threaded stub */
  struct timespec ts;
  if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return ++last;
  uint64_t v = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
  if (v <= last) v = last + 1;
  last = v;
  return v;
#else
  static uint64_t counter;
  return ++counter;
#endif
}

static hml_status esp_stub_create(hml_sock_t **out, uint32_t bind_ip, uint16_t bind_port,
                                  bool broadcast) {
  (void)bind_ip; (void)bind_port; (void)broadcast;
  if (out == NULL) return HML_ERR_ARG;
  *out = NULL;
  return HML_ERR_NOTSUP;
}

static hml_status esp_stub_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port) {
  (void)ip; (void)port;
  if (s == NULL) return HML_ERR_ARG;
  return HML_ERR_NOTSUP;
}

static hml_status esp_stub_send(hml_sock_t *s, const void *buf, size_t n) {
  (void)buf; (void)n;
  if (s == NULL || (buf == NULL && n != 0)) return HML_ERR_ARG;
  return HML_ERR_NOTSUP;
}

static hml_status esp_stub_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n, int timeout_ms) {
  (void)buf; (void)cap; (void)timeout_ms;
  if (s == NULL || buf == NULL || n == NULL) return HML_ERR_ARG;
  if (n != NULL) *n = 0;
  return HML_ERR_NOTSUP;
}

static void esp_stub_close(hml_sock_t *s) { (void)s; }

/* ------------------------------------------------------------------------- */
/* port-private surface: always compiled, always this port's behaviour       */
/* ------------------------------------------------------------------------- */

hml_status hml_port_esp_sock_udp_create(hml_sock_t **out, uint32_t bind_ip, uint16_t bind_port,
                                        bool broadcast) {
#if defined(HML_PORT_HAVE_ESP_IDF)
  return esp_udp_create(out, bind_ip, bind_port, broadcast);
#else
  return esp_stub_create(out, bind_ip, bind_port, broadcast);
#endif
}

hml_status hml_port_esp_sock_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port) {
#if defined(HML_PORT_HAVE_ESP_IDF)
  return esp_set_dest(s, ip, port);
#else
  return esp_stub_set_dest(s, ip, port);
#endif
}

hml_status hml_port_esp_sock_send(hml_sock_t *s, const void *buf, size_t n) {
#if defined(HML_PORT_HAVE_ESP_IDF)
  return esp_send(s, buf, n);
#else
  return esp_stub_send(s, buf, n);
#endif
}

hml_status hml_port_esp_sock_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n,
                                  int timeout_ms) {
#if defined(HML_PORT_HAVE_ESP_IDF)
  return esp_recv(s, buf, cap, n, timeout_ms);
#else
  return esp_stub_recv(s, buf, cap, n, timeout_ms);
#endif
}

void hml_port_esp_sock_close(hml_sock_t *s) {
#if defined(HML_PORT_HAVE_ESP_IDF)
  esp_close(s);
#else
  esp_stub_close(s);
#endif
}

uint64_t hml_port_esp_clock_us(void) {
#if defined(HML_PORT_HAVE_ESP_IDF)
  return esp_clock_us();
#else
  return esp_stub_clock_us();
#endif
}

/* ------------------------------------------------------------------------- */
/* public frozen API -- owned by this file only on an ESP-IDF target          */
/* ------------------------------------------------------------------------- */
#if !defined(HML_PORT_POSIX)

uint64_t hml_clock_us(void) { return hml_port_esp_clock_us(); }

hml_status hml_sock_udp_create(hml_sock_t **out, uint32_t bind_ip, uint16_t bind_port,
                               bool broadcast) {
  return hml_port_esp_sock_udp_create(out, bind_ip, bind_port, broadcast);
}

hml_status hml_sock_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port) {
  return hml_port_esp_sock_set_dest(s, ip, port);
}

hml_status hml_sock_send(hml_sock_t *s, const void *buf, size_t n) {
  return hml_port_esp_sock_send(s, buf, n);
}

hml_status hml_sock_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n, int timeout_ms) {
  return hml_port_esp_sock_recv(s, buf, cap, n, timeout_ms);
}

void hml_sock_close(hml_sock_t *s) { hml_port_esp_sock_close(s); }

#endif /* !HML_PORT_POSIX */
