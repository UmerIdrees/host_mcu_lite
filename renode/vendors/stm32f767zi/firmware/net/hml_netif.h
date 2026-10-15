/*
 * hml_netif.h -- lwIP netif configuration for the emulated STM32F767 MAC.
 *
 * See hml_netif.c for why the netif exists, how ARP is resolved, and what this
 * file does and does not claim.
 */
#ifndef HML_NETIF_H
#define HML_NETIF_H

#include <stdbool.h>
#include <stdint.h>

#include "hml/hml_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1500 payload + 14 Ethernet header + 4 FCS is the largest frame the driver's
 * 13-bit Buffer1Size field can express with room to spare. */
#define HML_NETIF_MTU 1500u

/* A transmit bound, not a retry budget. hml_eth_send fails rather than spinning
 * forever, and the netif turns that into an error the caller can report. */
#define HML_NETIF_TX_TIMEOUT_MS 2000

/* Upper bound on how many frames one hml_netif_poll() call will move into lwIP.
 * The example sends one datagram and receives one ARP reply, so this is slack, not
 * a working limit; it exists so a driver fault surfaces as a bounded loop rather
 * than as a run that hangs. */
#define HML_NETIF_MAX_POLL_FRAMES 8u

/* Host byte order, as lwIP's ip4_addr_set_u32 expects. */
typedef struct {
  /* Six bytes, most significant first: the same order the Renode platform's
   * `sysbus.ethernet MAC "..."` and the network tester display. */
  uint8_t mac_address[6];
  uint32_t ip_address;
  uint32_t netmask;
  uint32_t gateway;
} hml_netif_cfg_t;

/*
 * Reset the MAC and register the netif with lwIP.
 *
 * Returns false, having registered nothing, if the MAC does not come up or if
 * the address is unset. The caller is expected to treat that as a failed run
 * rather than continuing with a netif that cannot transmit.
 */
HML_API bool hml_netif_init(const hml_netif_cfg_t *cfg);

/*
 * Move every frame the driver has received into lwIP.
 *
 * This is the NO_SYS equivalent of a receive thread: call it from whatever loop
 * drives the application. lwIP's ARP cache, its UDP timeouts and its pbuf
 * recycling all advance as a side effect, so a caller that never polls it will
 * not resolve ARP and will not see its datagram leave.
 */
HML_API void hml_netif_poll(void);

/* True once hml_netif_init has succeeded. */
HML_API bool hml_netif_up(void);

#ifdef __cplusplus
}
#endif

#endif /* HML_NETIF_H */
