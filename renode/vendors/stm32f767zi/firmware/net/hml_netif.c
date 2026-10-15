/*
 * hml_netif.c -- the lwIP 2.2.1 netif for the emulated STM32F767 Ethernet MAC.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS FILE EXISTS AT ALL
 * ---------------------------------------------------------------------------
 * lwIP ships drivers for a lot of MACs, and none of them is this one: Renode's
 * Network.SynopsysEthernetMAC is a DesignWare-style MAC that lwIP has no port
 * for. Rather than hand-roll IP and UDP -- which would make the example a test of
 * code written here rather than a test of this library's transport path -- the
 * split is:
 *
 *   hml_eth_synopsys.c  descriptor rings, MMIO, the emulated PHY. Chip-specific.
 *   hml_netif.c         THIS FILE. The four callbacks lwIP needs and nothing
 *                       else. The smallest possible surface.
 *   lwIP 2.2.1          ARP, IPv4, UDP, checksums, pbufs. Proven, upstream.
 *   hml_udp.c           the HML framing this example is actually exercising.
 *
 * So the only novel code is the glue lwIP cannot avoid, and the bytes on the wire
 * are built by lwIP's own IP and UDP code.
 *
 * ---------------------------------------------------------------------------
 * THE ARP PROBLEM, AND WHAT IS DONE ABOUT IT
 * ---------------------------------------------------------------------------
 * The emulated PHY is a bare Network.EthernetPhysicalLayer with no peer, so
 * nothing answers lwIP's ARP request and `udp_send` would queue forever without
 * emitting a datagram. Three ways out were available:
 *
 *   1. Let the test inject an ARP reply. Requires a working receive path, which
 *      this file has, and it is the honest option: ARP is real, the reply is a
 *      real frame, and lwIP's cache is populated by lwIP's own etharp.c.
 *   2. Prime the ARP cache from firmware. Would need lwIP's static arp_create(),
 *      which is file-static in 2.2.1 and therefore not callable. Rejected.
 *   3. Bypass lwIP's IP layer and hand it a pre-built frame. That is a hand-rolled
 *      IP/UDP stack wearing lwIP as a disguise. Rejected.
 *
 * (1) is what the example does. The test injects the reply with the network
 * tester's SendFrame, and this file hands every received frame to lwIP untouched
 * via netif->input, so the cache entry is created by upstream code. The resulting
 * sequence of frames leaving the MAC is therefore itself part of the evidence:
 * an ARP request, then the HML/UDP datagram.
 *
 * ---------------------------------------------------------------------------
 * NO_SYS
 * ---------------------------------------------------------------------------
 * This is a bare-metal cooperative netif. There is no thread, no semaphore and
 * no mailbox: input() is called from the same context that polls the driver, and
 * netif_poll() drains the receive ring into lwIP. The single-threaded assumption
 * is the price of NO_SYS and is stated here so nobody wires this to an RTOS
 * without re-reading it.
 */
#include "lwipopts.h"

#include <string.h>

void hml_console_puts(void *ctx, const char *line);
void hml_console_hex32(const char *key, uint32_t value);

#include "hml_netif.h"

#include "hml_eth_synopsys.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "netif/ethernet.h"
#include "lwip/pbuf.h"

/* Enough for the largest frame this driver accepts, with the driver's 4-byte FCS
 * allowance left over. One buffer per pbuf keeps the DMA from having to assemble
 * across a chain, which this driver does not implement. */
#define HML_NETIF_PBUF_BUFSIZE (HML_ETH_MAX_FRAME + 4u)

static struct netif hml_netif_state;
static bool hml_netif_ready_flag;

/* ------------------------------------------------------------------------- */
/* lwIP timebase.                                                               */
/*                                                                            */
/* lwIP is compiled with -Dsys_now=hml_lwip_now_ms, so the token sys_now in     */
/* lwIP's own headers is renamed to this function at compile time. That is the    */
/* only wiring needed, and it is why hml_lwip_now_ms is a hard link-time          */
/* requirement of liblwip.a: timeouts.c references it unconditionally.            */
/*                                                                            */
/* The source is the board's own tick, NOT a counter of our own. lwIP's ARP     */
/* cache ages in seconds and its UDP reassembly times out in milliseconds, so a  */
/* timebase that does not track real elapsed time would quietly break both.      */
/* hml_clock_us() comes from the vendor startup file and is SysTick-derived.     */
/* ------------------------------------------------------------------------- */
uint64_t hml_clock_us(void);

uint32_t hml_lwip_now_ms(void) {
  return (uint32_t)(hml_clock_us() / 1000ull);
}

static err_t hml_netif_linkoutput(struct netif *netif, struct pbuf *p);

/* ------------------------------------------------------------------------- */
/* hml_lwip_assert: say what lwIP asserted, in the open, before stopping.         */
/* ------------------------------------------------------------------------- */
/* lwipopts.h routes every LWIP_PLATFORM_ASSERT to hml_lwip_assert(file, line,
 * message) precisely so a bare-metal port does not fail silently. liblwip.a only
 * ships a weak definition of it, compiled to a single `udf`, so without a strong
 * one here an lwIP assertion traps the image on an invalid instruction: the run
 * prints nothing after the last ordinary line, the CPU stops with a lockup state,
 * and the harness cannot tell an assertion failure from a wedged DMA. That is
 * exactly how the missing-netif->hwaddr fault above presented, and it cost a long
 * time to diagnose from the outside.
 *
 * The message is printed in full because it is the whole diagnosis: the file and
 * line come from lwIP's own tree, and the text is the assertion lwIP actually
 * evaluated. Then the image stops rather than returning, because a failed
 * assertion means lwIP's internal invariants are already broken and letting the
 * demo carry on would report a result for a stack that cannot be trusted. */
void hml_lwip_assert(const char *file, int line, const char *message) {
  hml_console_puts(NULL, "HML-LWIP-ASSERT file=");
  hml_console_puts(NULL, (file != NULL) ? file : "(null)");
  hml_console_hex32("HML-LWIP-ASSERT line=", (uint32_t)line);
  hml_console_puts(NULL, "HML-LWIP-ASSERT message=");
  hml_console_puts(NULL, (message != NULL) ? message : "(null)");
  hml_console_puts(NULL, "HML-MCU-NET result=FAIL");
  hml_console_puts(NULL, "HML-MCU-NET DONE");
  for (;;) {
  }
}

/* ------------------------------------------------------------------------- */
/* link_init: the four callbacks lwIP needs, and nothing else.                   */
/* ------------------------------------------------------------------------- */
static err_t hml_netif_link_init(struct netif *netif) {
  if (netif == NULL) {
    return ERR_ARG;
  }
  netif->name[0] = 'h';
  netif->name[1] = 'm';
  netif->output = etharp_output;
  netif->linkoutput = hml_netif_linkoutput;
  netif->mtu = HML_NETIF_MTU;

  /* The hardware address, and this used to be missing entirely, which is why the
   * wire stayed silent no matter what the MAC driver was doing. lwIP keeps its own
   * copy: MACA0LR/MACA0HR in the MAC tell the DMA which source address to put on
   * the wire, but etharp builds every ARP request and Ethernet header from
   * netif->hwaddr and asserts ETH_HWADDR_LEN against netif->hwaddr_len. With both
   * left at zero, the first ARP that lwIP built tripped
   *   "netif->hwaddr_len must be the same as ETH_HWADDR_LEN for etharp!"
   * in etharp.c, and the port routes LWIP_PLATFORM_ASSERT to hml_lwip_assert,
   * which liblwip.a only ships as a weak `udf` stub. So the image died on an
   * invalid instruction at netif_set_link_up() time, with no UART output at all:
   * the last line was "hello" and the run looked like a hang rather than an ARP
   * configuration fault. Programming the MAC's address registers is necessary and
   * is not sufficient. */
  const hml_netif_cfg_t *const cfg = (const hml_netif_cfg_t *)netif->state;
  if (cfg != NULL) {
    memcpy(netif->hwaddr, cfg->mac_address, ETH_HWADDR_LEN);
  }
  netif->hwaddr_len = ETH_HWADDR_LEN;
  /* ETHARP and BROADCAST are set here; UP and LINK_UP are NOT, because lwIP
   * requires netif_set_up() and netif_set_link_up() to be called deliberately and
   * the example calls them only once the MAC is confirmed up. Claiming otherwise
   * would let lwIP transmit through a netif whose DMA had never been reset.
   *
   * ETHARP is not optional and this line used to be `flags = 0u`, which was the
   * second bug that kept the wire silent: with it clear, ip4_output() refuses the
   * datagram with ERR_RTE before it ever asks for an ARP entry, so hml_sock_send
   * maps that to HML_ERR_AGAIN forever and not one ARP request is built. The
   * driver therefore looked like it was retrying a broken link when in fact lwIP
   * had refused the packet at the routing step, one layer above the driver. */
  netif->flags = NETIF_FLAG_ETHARP | NETIF_FLAG_BROADCAST;
  return ERR_OK;
}

/* ------------------------------------------------------------------------- */
/* linkoutput: lwIP hands a complete Ethernet frame to the driver.               */
/* ------------------------------------------------------------------------- */
static err_t hml_netif_linkoutput(struct netif *netif, struct pbuf *p) {
  (void)netif;

  /* A pbuf chain is possible in principle. The DMA in this driver takes one
   * contiguous buffer, so a chain has to be linearised. PBUF_POOL_SIZE is
   * configured above the largest frame, so the common case is a single pbuf and
   * this loop runs once. */
  static uint8_t linear[HML_NETIF_PBUF_BUFSIZE];
  uint16_t total = 0;
  for (struct pbuf *q = p; q != NULL; q = q->next) {
    if ((size_t)total + (size_t)q->len > sizeof(linear)) {
      return ERR_MEM;
    }
    uint8_t *const dst = linear + total;
    pbuf_copy_partial(q, dst, q->len, 0);
    total = (uint16_t)(total + q->len);
  }

  /* The timeout is a bound, not a retry count: hml_eth_send returns an error
   * rather than blocking forever, and that error is propagated so the caller
   * learns the frame did not go out. A silently dropped datagram is exactly the
   * failure this example has to be able to report. */
  if (hml_eth_send(linear, (size_t)total, HML_NETIF_TX_TIMEOUT_MS) != HML_ETH_OK) {
    return ERR_IF;
  }
  return ERR_OK;
}

/* ------------------------------------------------------------------------- */
/* input: the driver hands a received frame to lwIP.                            */
/* ------------------------------------------------------------------------- */
static void hml_netif_input(struct netif *netif, uint8_t *frame, size_t len) {
  struct pbuf *p;

  /* A fresh pbuf every frame. pbuf_alloc_from_custom is deliberately not used:
   * reusing a pool here would mean holding a pbuf across a DMA-owned buffer, and
   * the lifetime of those two things is exactly what this driver is careful about
   * in hml_eth_recv. */
  p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
  if (p == NULL) {
    /* Out of memory. Dropping is correct here and is not a silent success: lwIP
     * treats a NULL return from input as the frame being dropped, and the example
     * reports the receive count so a drop is visible in the run's output. */
    return;
  }
  pbuf_take(p, frame, (u16_t)len);

  /* Handed straight to lwIP. In particular it is NOT re-validated or re-written
   * here: ARP replies, ICMP and UDP all have to be parsed by lwIP's own code for
   * this example to mean anything. */
  if (netif->input(p, netif) != ERR_OK) {
    pbuf_free(p);
  }
}

/* ------------------------------------------------------------------------- */
/* netif_poll: drain the driver's receive ring into lwIP.                       */
/* ------------------------------------------------------------------------- */
void hml_netif_poll(void) {
  if (!hml_netif_ready_flag) {
    return;
  }
  /* Bounded on purpose. An unbounded drain would spin here forever if the DMA
   * kept handing back the same descriptor, turning a driver bug into a run that
   * never reaches its DONE line -- which the harness cannot tell apart from a run
   * that was never started. */
  for (unsigned i = 0u; i < HML_NETIF_MAX_POLL_FRAMES; i++) {
    static uint8_t frame[HML_ETH_MAX_FRAME];
    size_t len = 0u;
    /* timeout_ms 0: a non-blocking drain. Polling from the application loop is
     * what pumps this, and it is what keeps the stack's timers and its ARP cache
     * moving on a NO_SYS target. */
    const hml_eth_status_t st = hml_eth_recv(frame, sizeof(frame), &len, 0);
    if (st == HML_ETH_ERR_TIMEOUT) {
      return; /* ring empty, which is the normal case */
    }
    if (st != HML_ETH_OK) {
      return; /* a dropped oversized frame; the ring has already been resynced */
    }
    hml_netif_input(&hml_netif_state, frame, len);
  }
}

/* ------------------------------------------------------------------------- */
/* init: the four callbacks plus flags, and the IP/netmask/gateway.             */
/* ------------------------------------------------------------------------- */
bool hml_netif_init(const hml_netif_cfg_t *cfg) {
  if (cfg == NULL) {
    return false;
  }

  /* lwIP is initialised here rather than in the demo body, because it has to
   * happen before netif_add() and the two are one operation from the caller's
   * point of view: "give me a working interface on this MAC". lwIP's
   * NO_SYS=1 core has no start hook, and netif_add() on a stack whose memp pools
   * do not exist yet would fail in a way that looks like a driver fault. */
  lwip_init();

  /* Bring the MAC up first. If the DMA never resets, there is no point
   * registering a netif that cannot move a frame, and a half-registered netif
   * would let the example report success with a dead link. */
  if (hml_eth_init(cfg->mac_address) != HML_ETH_OK) {
    return false;
  }

  ip4_addr_t ip, netmask, gw;
  /* IP4_ADDR_ANY is 0. If the caller left an address unset, say so rather than
   * configuring 0.0.0.0 and letting the stack transmit with it. */
  if (cfg->ip_address == 0u || cfg->netmask == 0u) {
    return false;
  }
  /* htonl, and this is not cosmetic. ip4_addr_set_u32() is a plain memcpy of the
   * uint32, and lwIP's ip4_addr_t.addr is in NETWORK byte order, so handing it a
   * host-order address configures 10.0.0.1 as 1.0.0.10 on the wire. The symptom is
   * not a visible fault: the MAC still transmits, the peer still answers ARP, and
   * the only symptom is that every reply is addressed to the wrong host and the
   * stack never accepts it. The netif's own address is used to match incoming
   * frames, so a wrong value here is invisible until a real peer is involved. */
  ip4_addr_set_u32(&ip, htonl(cfg->ip_address));
  ip4_addr_set_u32(&netmask, htonl(cfg->netmask));
  ip4_addr_set_u32(&gw, htonl(cfg->gateway));

  /* netif->state carries the caller's config, which is the documented way to hand
   * a driver its own state into netif_add()'s init callback. hml_netif_link_init()
   * reads it once, to copy the hardware address out, and never dereferences it
   * again: the cfg is a local in the demo, so leaving a pointer to it in a netif
   * that outlives the demo's frame would be a dangling pointer waiting to be read
   * by something unrelated. */
  if (netif_add(&hml_netif_state, &ip, &netmask, &gw, (void *)(uintptr_t)cfg,
                hml_netif_link_init, ethernet_input) == NULL) {
    return false;
  }
  /* Nothing needs the caller's config again, and the demo's frame is about to go
   * away, so drop the pointer before that happens rather than leaving it set. */
  hml_netif_state.state = NULL;

  /* The link is only marked up once the DMA has actually been reset and the
   * descriptor rings are live. netif_set_up() is what lets lwIP's ARP and UDP
   * output paths run at all, and netif_set_link_up() is what tells it the carrier
   * is present. Both happen after hml_eth_init() returned OK, so a netif that
   * reports up is a netif whose DMA came out of reset -- not merely one that was
   * registered. */
  netif_set_up(&hml_netif_state);
  netif_set_link_up(&hml_netif_state);

  /* Deliberately NOT clearing the flags here. A second `flags = 0u` used to sit
   * just below these two calls and undid them: it removed NETIF_FLAG_UP and
   * NETIF_FLAG_LINK_UP again, so the netif reported up over the console while
   * lwIP considered it down and dropped every packet. See the flag assignment in
   * hml_netif_link_init() for the same mistake on the other side.
   *
   * No MPOOL or hardware-checksum-offload flags: lwIP 2.2.1 has no checksum
   * offload path, and this driver copies rather than handing over pbufs, so
   * advertising either would make lwIP skip work that is never done. */
  hml_netif_state.mtu = HML_NETIF_MTU;
  hml_netif_ready_flag = true;
  return true;
}

bool hml_netif_up(void) {
  return hml_netif_ready_flag;
}
