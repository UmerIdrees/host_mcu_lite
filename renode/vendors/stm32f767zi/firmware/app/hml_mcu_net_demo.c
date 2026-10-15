/*
 * hml_mcu_net_demo.c -- the STM32F767ZI network dataplane example.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS IMAGE DEMONSTRATES, IN ONE PARAGRAPH
 * ---------------------------------------------------------------------------
 * It brings up Renode's model of the STM32F767's real Ethernet MAC, registers it
 * with lwIP 2.2.1 through a thin netif, opens the library's own UDP transport
 * through the hml_sock_t port, and sends one HML frame as a real UDP datagram. The
 * datagram is built by lwIP's IP and UDP code and handed to the MAC's descriptor
 * ring, so a frame that appears on the emulated PHY was produced by this
 * library's transport path and this firmware, not by a test harness.
 *
 * ---------------------------------------------------------------------------
 * WHY IT RE-SENDS, AND WHY THAT IS CORRECT RATHER THAN A RETRY HACK
 * ---------------------------------------------------------------------------
 * The emulated PHY is a bare Network.EthernetPhysicalLayer with no peer. The first
 * hml_sock_send() therefore makes lwIP emit an ARP REQUEST and refuse the datagram
 * with ERR_RTE, which hml_sock_lwip.c maps to HML_ERR_AGAIN. Nothing is wrong, and
 * the run is not stuck: lwIP will emit the datagram as soon as it learns the peer's
 * MAC address. The robot test supplies that address by injecting a real ARP reply
 * into the MAC's receive ring.
 *
 * So the loop below is: send, pump the stack, look at what the MAC has actually
 * transmitted. It repeats until the MAC has carried a frame whose EtherType is IP
 * (0x0800), or until a bounded number of passes has gone by. The bound is the
 * important part -- an image that cannot resolve ARP fails with a reported
 * reason instead of hanging, because a run that never prints DONE is
 * indistinguishable from a run that never started.
 *
 * ---------------------------------------------------------------------------
 * WHAT THE OUTPUT MEANS
 * ---------------------------------------------------------------------------
 * Each line is `key=value` on one line so the robot test can assert on it without
 * parsing prose. `result=PASS` is only printed after the MAC reports having
 * transmitted a frame that is not an ARP request, which is the driver's own count
 * of frames the DMA handed to the PHY -- not an assumption that a send returned
 * OK.
 */
#include "hml_netif.h"
#include "hml_eth_synopsys.h"
#include "hml_sock_lwip.h"

#include "board_config.h"
#include "FreeRTOS.h" /* the shared stub: only vTaskDelay and pdMS_TO_TICKS, see the note below */

#include "hml/hml_protocol.h"
#include "hml/hml_transport.h"

/* hml_udp_register() is declared in the transport's own private header,
 * src/transports/udp/hml_udp_priv.h, not in a public one -- that is the shipping
 * registration entry point and this is the only thing that needs it. */
#include "hml_udp_priv.h"

#include <stdint.h>
#include <string.h>

/*
 * The addressing this image uses. These four numbers are not arbitrary: they are
 * the same values the network robot test injects its ARP reply for, and changing
 * one side without the other is a guaranteed no-datagram run. The mask and gateway
 * are on the same /24.
 *
 * The MAC address MUST match `sysbus.ethernet MAC "02:00:00:00:00:01"` in
 * stm32f767zi.resc. The two are independent settings on the same peripheral: the
 * platform's sets the address Renode stamps on frames the model emits, this one
 * sets MACA0LR/MACA0HR in the firmware. If they disagree the peer will not accept
 * the frame, so the network robot test asserts they agree.
 */
#define DEMO_MAC0 0x02u
#define DEMO_MAC1 0x00u
#define DEMO_MAC2 0x00u
#define DEMO_MAC3 0x00u
#define DEMO_MAC4 0x00u
#define DEMO_MAC5 0x01u

/* 10.0.0.1/24 in host byte order. hml_sock.h takes addresses in host order, and
 * hml_sock_lwip.c owns the swap into lwIP's network order. */
#define DEMO_IP_HOST 0x0A000001u /* 10.0.0.1  */
#define DEMO_NETMASK_HOST 0xFFFFFF00u
#define DEMO_GW_HOST 0x0A000001u
#define DEMO_PEER_HOST 0x0A000002u /* 10.0.0.2: the address the test answers for */
#define DEMO_PEER_PORT 5000u

/* Bounded in three independent ways, because each one catches a different failure:
 *   DEMO_MAX_PASSES        a hard iteration cap, so a wedged stack cannot hang the
 *                          image and a run that never reaches DONE is
 *                          distinguishable from one that was never started;
 *   DEMO_BUDGET_MS         the emulated-time cap, which is what actually
 *                          synchronises this firmware with the peer outside the
 *                          emulator (see the long comment at the retry loop);
 *   DEMO_POLL_SPINS        the receive pump depth per pass.
 * The two time figures are not redundant: passes bounds the work, milliseconds
 * bounds the wait, and a firmware that only bounded passes would race the peer. */
#define DEMO_MAX_PASSES 64u
#define DEMO_POLL_SPINS 200u
#define DEMO_BUDGET_MS 10000u
#define DEMO_PASS_DELAY_MS 20u

/* EtherType of an ARP frame, versus IP. Used to tell "the MAC carried my ARP
 * request" from "the MAC carried my datagram", which is the distinction the whole
 * run turns on: a frame count of one proves the DMA works, a frame whose
 * EtherType is IP proves the transport path works. The network robot test asserts
 * the second one independently, on the wire, by EtherType. */
#define ETHERTYPE_ARP 0x0806u
#define ETHERTYPE_IPV4 0x0800u

/* Provided by this vendor's startup file, which owns the SysTick-derived tick. */
uint64_t hml_clock_us(void);
uint64_t hml_clock_ms(void);  /* hml_clock_us in ms; both come from the startup file */


/* The console belongs to the board's startup file, which owns the UART's register
 * map (UART.STM32_UART, and the reasoning for that substitution lives there). This
 * body prints through those three functions rather than writing the data register
 * itself, so there is exactly one place in the image that knows the console map. */
void hml_console_puts(void *ctx, const char *line);
void hml_console_kv(const char *key, uint32_t value);
void hml_console_hex32(const char *key, uint32_t value);

/* Called by main() in startup_stm32f767zi.c when HML_VENDOR_APP_NETWORK is 1. */
int hml_mcu_net_demo_run(void) {
  hml_console_puts(NULL, "HML-MCU-NET hello");

  const hml_netif_cfg_t cfg = {
      {DEMO_MAC0, DEMO_MAC1, DEMO_MAC2, DEMO_MAC3, DEMO_MAC4, DEMO_MAC5},
      DEMO_IP_HOST,
      DEMO_NETMASK_HOST,
      DEMO_GW_HOST,
  };

  /* lwIP is initialised inside hml_netif_init(), before any netif is added: the
   * stack has to have its memp pools before netif_add() can use them. */
  if (!hml_netif_init(&cfg)) {
    hml_console_puts(NULL, "HML-MCU-NET fatal=netif-init-failed");
    hml_console_puts(NULL, "HML-MCU-NET result=FAIL");
    hml_console_puts(NULL, "HML-MCU-NET DONE");
    return 1;
  }
  hml_console_puts(NULL, "HML-MCU-NET netif=up");
  hml_console_hex32("HML-MCU-NET eth-base=", HML_VENDOR_ETH_ADDR);

  /* The library's own transport, through this port. hml_udp_register() is the
   * shipping registration function in src/transports/udp/hml_udp.c, so the bytes
   * that reach the wire are shaped by library code, not by anything in this file. */
  if (hml_udp_register() != HML_OK) {
    hml_console_puts(NULL, "HML-MCU-NET fatal=udp-register-failed");
    hml_console_puts(NULL, "HML-MCU-NET result=FAIL");
    hml_console_puts(NULL, "HML-MCU-NET DONE");
    return 1;
  }
  const hml_xport_ops *udp = hml_transport_lookup("udp");
  if (udp == NULL) {
    hml_console_puts(NULL, "HML-MCU-NET fatal=udp-transport-missing");
    hml_console_puts(NULL, "HML-MCU-NET result=FAIL");
    hml_console_puts(NULL, "HML-MCU-NET DONE");
    return 1;
  }

  hml_xport_t xport;
  memset(&xport, 0, sizeof(xport));
  hml_dest_cfg dest;
  memset(&dest, 0, sizeof(dest));
  dest.kind = HML_XPORT_UDP;
  /* hml_proto_cfg_fill/hml_udp expect the address in NETWORK byte order; the port
   * converts to host order itself. 10.0.0.2, the address the network robot test
   * answers ARP for. */
  dest.ipv4 = ((uint32_t)10u << 24) | ((uint32_t)0u << 16) | ((uint32_t)0u << 8) | 2u;
  dest.udp_port = DEMO_PEER_PORT;

  if (udp->open(&xport, &dest, 1u) != HML_OK) {
    hml_console_puts(NULL, "HML-MCU-NET fatal=xport-open-failed");
    hml_console_puts(NULL, "HML-MCU-NET result=FAIL");
    hml_console_puts(NULL, "HML-MCU-NET DONE");
    return 1;
  }

  /* A small, fully deterministic payload. The robot test asserts on the HML magic
   * inside the datagram, so the content here is deliberately fixed rather than
   * derived from a clock or a counter: a payload that varied would make the
   * expected bytes in the test vary too. */
  uint8_t payload[16];
  for (unsigned i = 0u; i < sizeof(payload); i++) {
    payload[i] = (uint8_t)(0xA0u + i);
  }

  hml_frame_hdr hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.stream_id = 1u;
  hdr.seq = 1u;
  hdr.frag_idx = 0u;
  hdr.frag_cnt = 1u;
  hdr.ts_us = 1000u;
  hdr.payload_len = (uint16_t)sizeof(payload);

  /* The emulated-time budget for the whole retry loop.
   *
   * This is the one piece of synchronisation in the run, and it exists because of
   * a race that is otherwise invisible. The peer is the robot test on the other
   * side of the emulation, and the ARP reply it injects only happens AFTER the
   * firmware has emitted its first ARP request. A loop that spins with no delay
   * would burn all DEMO_MAX_PASSES in a few hundred microseconds of wall clock --
   * long before a Python process outside the emulator had reacted to the frame --
   * and the run would fail with "no datagram" for a reason that has nothing to do
   * with the driver, the netif or the transport.
   *
   * So the loop is bounded in EMULATED time as well as in iterations: it exits
   * early once DEMO_BUDGET_MS of the board's own tick have gone by. The robot test
   * additionally freezes the CPU around the injection, but a test-side pause is
   * not something the firmware may depend on, and this budget is what makes the
   * firmware correct on its own terms.
   *
   * DEMO_POLL_SPINS is 200 and DEMO_PASS_DELAY_TICKS is 20 (portTICK_PERIOD_MS is
   * 1), so each pass costs about 20 ms of emulated time and the budget is about
   * 10 s: 500 possible attempts, against a maximum of 64. */
  const uint64_t start_ms = hml_clock_ms();
  const uint64_t deadline_ms = start_ms + DEMO_BUDGET_MS;
  uint32_t passes = 0u;
  hml_status st = HML_ERR_AGAIN;
  const uint32_t frames_after_open = hml_eth_tx_frames();
  const uint32_t arp_after_open = hml_eth_tx_arp_frames();
  bool out_of_time = false;

  for (; passes < DEMO_MAX_PASSES; passes++) {
    st = udp->send_frag(&xport, &hdr, payload, sizeof(payload));

    /* Pump the stack. This is what drains the MAC's receive ring into lwIP, which
     * is how an injected ARP reply becomes a cache entry, and it is also what
     * advances lwIP's timers on a NO_SYS target. */
    for (unsigned i = 0u; i < DEMO_POLL_SPINS; i++) {
      hml_netif_poll();
    }

    /* Frames the DMA actually handed to the PHY, split by whether they were ARP.
     * The emulated receive path cannot manufacture one, so a rise here means a
     * real frame left the machine -- but the ARP request lwIP sends to resolve
     * the peer is such a frame, and it leaves *after* the baseline above, inside
     * the first send_frag(). Counting it as the datagram made this loop stop on
     * pass one, before lwIP had even been given a chance to be answered. The
     * datapath frame is the one that is not ARP. */
    const uint32_t sent = hml_eth_tx_frames() - frames_after_open;
    const uint32_t arp_sent = hml_eth_tx_arp_frames() - arp_after_open;
    const uint32_t data_sent = (sent > arp_sent) ? (sent - arp_sent) : 0u;
    if (data_sent > 0u && st == HML_OK) {
      break;
    }

    if (hml_clock_ms() >= deadline_ms) {
      out_of_time = true;
      break;
    }

    /* Wait before the next attempt. See the comment above: this is what keeps the
     * loop from finishing before the peer can answer. */
    vTaskDelay(pdMS_TO_TICKS(DEMO_PASS_DELAY_MS));
  }

  (void)udp->flush(&xport);

  const uint32_t eth_tx = hml_eth_tx_frames();
  const uint32_t eth_rx = hml_eth_rx_frames();

  hml_console_kv("passes", passes);
  hml_console_kv("eth-tx", eth_tx);
  hml_console_kv("eth-rx", eth_rx);
  hml_console_kv("send-status", (uint32_t)st);
  hml_console_kv("elapsed-ms", (uint32_t)(hml_clock_ms() - start_ms));
  /* Printed unconditionally, and asserted by the robot test, because a timebase
   * that never advanced would make every timing number above meaningless -- and
   * because a constant-zero sys_now is a real defect in a stack that caches ARP
   * entries, not a cosmetic one. */
  hml_console_kv("systick-live", (hml_clock_ms() > start_ms || passes == 0u) ? 1u : 0u);
  if (out_of_time) {
    hml_console_puts(NULL, "HML-MCU-NET-WARN gave-up=emulated-time-budget-exhausted");
  }

  /* The pass/fail rule, stated precisely:
   *   the transport's last send returned HML_OK, AND
   *   the MAC's own transmit counter rose, AND
   *   at least one frame left besides the ARP request that resolution needed.
   *
   * A send that returned HML_OK with an unmoved transmit counter is treated as a
   * FAILURE, because "the stack accepted it" is not "it went out" and conflating
   * the two is the exact bug this example exists to avoid. */
  const bool sent_ok = (st == HML_OK) && (eth_tx > frames_after_open);
  /* At least one non-ARP frame left, counted from the driver's own ARP tally
   * rather than assumed to be "two frames". "Two frames" was a proxy for "one
   * request plus one datagram" and it is wrong twice over: it cannot tell a
   * datagram from a second ARP request, which lwIP sends on a timer when the
   * peer has not answered yet, and it is computed from a number this run prints
   * as arp-frames, so a failure to resolve and a failure to transmit reported the
   * same counter. */
  const uint32_t arp_total = hml_eth_tx_arp_frames() - arp_after_open;
  const uint32_t data_total = (eth_tx - frames_after_open) > arp_total
                                  ? (eth_tx - frames_after_open) - arp_total
                                  : 0u;
  const bool datagram_out = (data_total >= 1u);

  hml_console_kv("arp-frames", arp_total);
  hml_console_kv("data-frames", data_total);
  hml_console_kv("expected-ethertype-arp", ETHERTYPE_ARP);
  hml_console_kv("expected-ethertype-ipv4", ETHERTYPE_IPV4);
  hml_console_puts(NULL, sent_ok && datagram_out ? "HML-MCU-NET result=PASS"
                                                 : "HML-MCU-NET result=FAIL");

  (void)udp->close(&xport);
  hml_console_puts(NULL, "HML-MCU-NET DONE");
  return (sent_ok && datagram_out) ? 0 : 1;
}
