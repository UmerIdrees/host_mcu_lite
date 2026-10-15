/*
 * hml_sock_lwip.c -- the hml_sock_t port implemented on lwIP 2.2.1.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS FILE IS THE WHOLE PORT INTERFACE
 * ---------------------------------------------------------------------------
 * include/hml/hml_sock.h is seven functions: create a UDP socket, set a default
 * destination, send, receive, close. Everything above it -- hml_udp.c and
 * hml_rocev2.c -- is transport-agnostic and was written against exactly that
 * interface. So implementing those seven functions on lwIP's udp_pcb is the
 * entire amount of port code the network path needs, and both transports can use
 * it unchanged.
 *
 * NOTE ON WHETHER THIS IS "A PORT" OR "A DEMO"
 * ports/freertos_lwip/hml_port_freertos.c is the same idea for a FreeRTOS target
 * and needs a FreeRTOS+NO_SYS bridge. This file is that bridge for bare metal:
 * no threads, no mutexes, no mailbox, because NO_SYS means there is exactly one
 * context and the application calls hml_netif_poll() from it.
 *
 * ---------------------------------------------------------------------------
 * ERROR SEMANTICS, WHICH ARE A CONTRACT AND NOT AN IMPLEMENTATION DETAIL
 * ---------------------------------------------------------------------------
 * hml_sock.h specifies: HML_OK with n>0, HML_ERR_AGAIN for an empty poll or an
 * expired timeout, or an I/O error. hml_udp.c depends on that distinction --
 * udp_send_frag() counts HML_ERR_AGAIN separately from a real failure -- so a
 * lwIP "would block" is translated to HML_ERR_AGAIN and never to HML_OK with a
 * short count, which would silently truncate a datagram.
 *
 * ---------------------------------------------------------------------------
 * CHECKSUM OFFLOAD: THERE IS NONE, ON PURPOSE
 * ---------------------------------------------------------------------------
 * lwIP 2.2.1 has no checksum offload path (ethernet.c contains no hwcsum code at
 * all), and the MAC driver sets DisableCRC on transmit so the model appends the
 * FCS. So the IP and UDP checksums are computed in software by lwIP's own
 * inet_chksum() on the way out, and verified on the way in. Nothing here enables
 * a flag that claims hardware would have done it.
 */
#include "lwipopts.h"

#include "hml_sock_lwip.h"

#include <string.h>

#include "hml_netif.h"
#include "lwip/def.h"
#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

/* hml_sock.h's addresses arrive in HOST byte order. lwIP's ip4_addr_t.addr field
 * holds the address in NETWORK byte order -- ip4_addr_set_u32() is a bare
 * assignment with no swapping, and ip4_addr_aton() reaches it through
 * lwip_htonl(). So the conversion below is this port's job, and getting it wrong
 * would produce a destination that looks plausible in a register dump and is
 * addressed to the wrong host.
 *
 * ip4_addr_t, not ip_addr_t: this build has LWIP_IPV6=0, so lwIP's ip_addr_t is a
 * typedef for ip4_addr_t and has no .type member to set. */
static ip4_addr_t hml_sock_addr_from_host(uint32_t host_order) {
  ip4_addr_t a;
  ip4_addr_set_u32(&a, lwip_htonl(host_order));
  return a;
}

/* One socket. hml_sock.h says so explicitly, and both transports hold exactly one
 * of these at a time, so a single embedded pcb is the honest representation
 * rather than a heap allocation that buys nothing. */
struct hml_sock {
  struct udp_pcb *pcb;
  uint32_t dest_ip; /* host byte order */
  uint16_t dest_port;
  uint16_t bound_port;
  /* The single-datagram receive slot. A datagram larger than this is reported as
   * HML_ERR_AGAIN after being dropped, not truncated into the caller's buffer. */
  uint8_t rx_buf[HML_SOCK_RX_MAX];
  size_t rx_len;
  bool rx_ready;
};

static struct hml_sock hml_sock_pool[HML_SOCK_MAX_SOCKETS];
static bool hml_sock_pool_used[HML_SOCK_MAX_SOCKETS];

/*
 * The lwIP receive callback.
 *
 * Called from within lwIP, on the same stack context as the input path, which is
 * why it does no allocation and no blocking: it copies into the socket's fixed
 * slot and returns. A datagram that does not fit is dropped and the slot is left
 * empty, so the caller's next poll sees an empty socket rather than a short
 * datagram that would look like a complete one.
 */
static void hml_sock_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                             const ip_addr_t *addr, u16_t port) {
  struct hml_sock *s = (struct hml_sock *)arg;
  (void)pcb;
  (void)addr;
  (void)port;

  if (s == NULL || p == NULL) {
    return;
  }
  if (p->tot_len > sizeof(s->rx_buf)) {
    /* Too big for the slot. Dropping it and leaving rx_ready clear is the honest
     * outcome: a partial copy would hand the caller a datagram with a length that
     * lies about its content. */
    s->rx_ready = false;
    s->rx_len = 0u;
  } else {
    pbuf_copy_partial(p, s->rx_buf, p->tot_len, 0);
    s->rx_len = (size_t)p->tot_len;
    s->rx_ready = true;
  }
  /* pbuf_free, NOT pbuf_free_and_recv: the chain is freed here because the bytes
   * have already been copied out, and freeing the pcb's receive queue from inside
   * its own callback is the documented way to drop a datagram. */
  pbuf_free(p);
}

hml_status hml_sock_udp_create(hml_sock_t **out, uint32_t bind_ip, uint16_t bind_port,
                               bool broadcast) {
  if (out == NULL) {
    return HML_ERR_ARG;
  }
  (void)broadcast;
  /* bind_port 0 means "any" per the header. An explicitly non-zero bind port with
   * a zero bind address would silently mean INADDR_ANY in lwIP, so it is accepted
   * as "any address, that port", which is the reading that can actually work. */
  if (bind_port != 0u && bind_ip == 0u) {
    bind_ip = 0u;
  }

  for (unsigned i = 0u; i < HML_SOCK_MAX_SOCKETS; i++) {
    if (hml_sock_pool_used[i]) {
      continue;
    }
    struct hml_sock *s = &hml_sock_pool[i];
    s->pcb = udp_new();
    if (s->pcb == NULL) {
      return HML_ERR_NOMEM;
    }
    /* A local port of 0 asks lwIP for an ephemeral source port, which is what
     * hml_udp.c's "bind 0.0.0.0:0" comment expects. */
    const ip4_addr_t bind_addr = hml_sock_addr_from_host(bind_ip);
    if (udp_bind(s->pcb, &bind_addr, bind_port) != ERR_OK) {
      udp_remove(s->pcb);
      s->pcb = NULL;
      return HML_ERR_IO;
    }
    udp_recv(s->pcb, hml_sock_recv_cb, s);

    s->dest_ip = 0u;
    s->dest_port = 0u;
    s->bound_port = bind_port;
    s->rx_len = 0u;
    s->rx_ready = false;
    hml_sock_pool_used[i] = true;
    *out = s;
    return HML_OK;
  }
  return HML_ERR_NOMEM;
}

hml_status hml_sock_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port) {
  if (s == NULL || s->pcb == NULL) {
    return HML_ERR_STATE;
  }
  if (ip == 0u || port == 0u) {
    return HML_ERR_ARG;
  }
  s->dest_ip = ip;
  s->dest_port = port;
  return HML_OK;
}

hml_status hml_sock_send(hml_sock_t *s, const void *buf, size_t n) {
  if (s == NULL || s->pcb == NULL) {
    return HML_ERR_STATE;
  }
  if (buf == NULL && n > 0u) {
    return HML_ERR_ARG;
  }
  if (n == 0u) {
    return HML_ERR_ARG;
  }
  if (n > 0xFFFFu) {
    return HML_ERR_TOOLARGE;
  }
  if (s->dest_ip == 0u) {
    return HML_ERR_STATE; /* set_dest has not been called */
  }

  struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
  if (p == NULL) {
    /* Out of pbufs is a real failure, not "try again later": on NO_SYS nothing
     * else is running that would free one. Reporting HML_ERR_AGAIN here would
     * make hml_udp.c count a lost datagram as a retryable hiccup. */
    return HML_ERR_NOMEM;
  }
  if (pbuf_take(p, buf, (u16_t)n) != ERR_OK) {
    pbuf_free(p);
    return HML_ERR_IO;
  }

  const ip4_addr_t dest = hml_sock_addr_from_host(s->dest_ip);
  /* Port is network byte order on the wire and hml_sock.h says the caller passes
   * host order, so this conversion is the port's job.
   *
   * udp_sendto(), not udp_send(): udp_send() only works on a CONNECTED pcb, and
   * this socket is never connected -- hml_udp.c sets a default destination and
   * sends repeatedly, which is udp_sendto()'s contract. */
  const err_t err = udp_sendto(s->pcb, p, &dest, lwip_htons(s->dest_port));
  pbuf_free(p); /* udp_send does not take ownership; it copied what it needed */

  if (err == ERR_OK) {
    return HML_OK;
  }
  /* ERR_MEM and ERR_RTE are lwIP's "would block" and "no route yet" answers. With
   * no peer, ERR_RTE is what an unresolved ARP entry produces, and it is a
   * retryable condition rather than a broken socket, so it maps to HML_ERR_AGAIN
   * exactly as hml_sock.h's contract requires. */
  if (err == ERR_MEM || err == ERR_RTE) {
    return HML_ERR_AGAIN;
  }
  return HML_ERR_IO;
}

hml_status hml_sock_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n, int timeout_ms) {
  if (s == NULL || s->pcb == NULL || n == NULL) {
    return HML_ERR_STATE;
  }
  if (buf == NULL && cap > 0u) {
    return HML_ERR_ARG;
  }
  *n = 0u;

  /* timeout_ms < 0 means "block". Under NO_SYS a true block would spin the CPU
   * forever with no way to make progress, because nothing else can run. The loop
   * below is the closest honest equivalent: it polls the netif (which is what
   * pumps the receive ring) until the datagram arrives or a bounded number of
   * passes has gone by. It is a spin, and it is documented as one. */
  if (timeout_ms < 0) {
    for (unsigned spin = 0u; spin < HML_SOCK_RX_MAX_SPINS; spin++) {
      hml_netif_poll();
      if (s->rx_ready) {
        break;
      }
    }
  } else {
    const unsigned budget = (unsigned)timeout_ms;
    for (unsigned i = 0u; i <= budget; i++) {
      hml_netif_poll();
      if (s->rx_ready) {
        break;
      }
    }
  }

  if (!s->rx_ready) {
    return HML_ERR_AGAIN; /* empty, or the wait expired: the header's contract */
  }

  if (s->rx_len > cap) {
    /* The caller's buffer cannot hold it. The datagram is consumed either way --
     * a partially consumed one would make the next poll return the tail and look
     * like a second, smaller datagram. */
    s->rx_ready = false;
    s->rx_len = 0u;
    return HML_ERR_AGAIN;
  }

  uint8_t *const dst = (uint8_t *)buf;
  for (size_t i = 0u; i < s->rx_len; i++) {
    dst[i] = s->rx_buf[i];
  }
  *n = s->rx_len;
  s->rx_ready = false;
  s->rx_len = 0u;
  return HML_OK;
}

void hml_sock_close(hml_sock_t *s) {
  if (s == NULL) {
    return;
  }
  if (s->pcb != NULL) {
    udp_disconnect(s->pcb);
    udp_remove(s->pcb);
    s->pcb = NULL;
  }
  for (unsigned i = 0u; i < HML_SOCK_MAX_SOCKETS; i++) {
    if (&hml_sock_pool[i] == s) {
      hml_sock_pool_used[i] = false;
      break;
    }
  }
  s->rx_ready = false;
  s->rx_len = 0u;
}
