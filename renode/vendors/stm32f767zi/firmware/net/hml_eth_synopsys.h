/*
 * hml_eth_synopsys.h -- bare-metal driver for the Synopsys (DesignWare) Ethernet
 * MAC as Renode models it on the STM32F767.
 *
 * WHAT THIS FILE IS
 *   The only part of the network path that is specific to the emulated chip. It
 *   owns the descriptor rings and turns a caller-supplied Ethernet frame into a
 *   frame on the emulated PHY. It has no notion of IP, UDP or HML; see
 *   hml_netif.c for that.
 *
 * WHY IT IS SAFE TO TRUST
 *   Every register offset and every descriptor bit position here was read out of
 *   the Renode build that this firmware actually runs against, not out of a data
 *   sheet and not from memory:
 *     - register offsets/names came from the model's own register collection
 *       (DoubleWordRegister dump) and are listed in the MMIO block below;
 *     - the descriptor layouts came from the [Offset]/[Width] attributes on
 *       SynopsysEthernetMAC.STM32FNormalTxDescriptor and
 *       SynopsysEthernetMAC.STM32FNormalRxDescriptor inside
 *       Infrastructure.dll.
 *   The two layouts are reproduced verbatim in hml_eth_synopsys.c next to the
 *   code that uses them. If a future Renode release changes them, these tables
 *   are the single place to change, and the robot test's byte assertions are
 *   what would notice.
 *
 * WHERE THE AUTHORITY IS
 *   Renode 1.17.0, portable install at
 *   /home/vboxuser/hml_toolchain_cache/renode_1.17.0-portable
 *   Verified against Infrastructure.dll with ikdasm. The model picks the
 *   "STM32F" descriptor variant because the platform does not set `version:`, and
 *   the model's default for this peripheral is the STM32F layout. That is why
 *   the bit positions below are the STM32F ones and not the BeagleV ones.
 *
 * SCOPE / HONEST LIMITS
 *   - Transmit and receive only. No multicast filter, no promiscuous mode, no
 *     flow control, no MDIO/MII management, no checksum offload. lwIP computes
 *     every checksum in software (lwIP 2.2.1 has no offload path at all), so
 *     DisableCRC is asserted on transmit and the MAC appends the FCS.
 *   - Polled. No interrupt is enabled or taken. The example polls.
 *   - The frame buffer is static SRAM; nothing here allocates.
 */
#ifndef HML_ETH_SYNOPSYS_H
#define HML_ETH_SYNOPSYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hml/hml_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One descriptor is 4 words (16 bytes) in BOTH rings. The model's transmit path
 * hard-codes this stride -- it constructs the next descriptor as
 * (current + 4 words) -- so 4 is not a tuning parameter. */
#define HML_ETH_DESC_WORDS 4u
#define HML_ETH_DESC_BYTES (HML_ETH_DESC_WORDS * 4u)

/* Ring depths. Deep enough that lwIP never has to block on a full ring for the
 * datagram the example sends, shallow enough to keep the SRAM cost visible. */
#define HML_ETH_TX_RING 4u
#define HML_ETH_RX_RING 4u

/* Largest frame this driver will hand to or take from the MAC. The ring's
 * Buffer1Size field is only 13 bits wide (see the RX layout), so 8191 is the
 * real ceiling; keep the default under it with room for the 4-byte FCS. */
#define HML_ETH_MAX_FRAME 1518u

/* Status codes. Zero is success, so a caller can test `== 0`. */
typedef enum {
  HML_ETH_OK = 0,
  HML_ETH_ERR_ARG = -1,
  HML_ETH_ERR_NULL = -2,
  HML_ETH_ERR_TOO_LONG = -3,
  HML_ETH_ERR_TIMEOUT = -4
} hml_eth_status_t;

/*
 * Bring the MAC and DMA up.
 *
 * Programs, in order: a software reset of the DMA block, the descriptor-skip
 * length, the receive and transmit descriptor list addresses, the MAC's own
 * address, then transmit and receive enable in MACCR. The PHY is a passive
 * EthernetPhysicalLayer in this platform and needs no software-visible setup
 * for the MAC to move frames.
 *
 * mac_address is 6 bytes, most-significant first, which is the byte order lwIP
 * and the network tester both display.
 */
HML_API hml_eth_status_t hml_eth_init(const uint8_t mac_address[6]);

/*
 * Hand one complete Ethernet frame to the MAC and wait for the DMA to consume it.
 *
 * `frame` is a full Ethernet frame: destination MAC, source MAC, EtherType, and
 * payload, but WITHOUT the 4-byte FCS -- the MAC appends that. `len` is the
 * length of that frame, and it is what lands in the descriptor's Buffer1Size
 * field; the model reads the transmit length from exactly that field.
 *
 * Blocks until the descriptor's OWN bit has been cleared by the DMA, or until
 * `timeout_ms` has expired. A timeout is not a silent success: it is reported
 * so the caller can fail the run rather than claim a frame went out.
 */
HML_API hml_eth_status_t hml_eth_send(const void *frame, size_t len, int timeout_ms);

/*
 * Fetch at most one received frame, if one is waiting.
 *
 * Returns HML_ETH_OK with *out_len set to the frame length, or HML_ETH_ERR_TIMEOUT
 * if the ring is empty (a poll, not an error). `cap` bounds the copy; a frame
 * longer than `cap` is dropped and reported as HML_ETH_ERR_TOO_LONG so the caller
 * resynchronises the ring deliberately instead of reading a torn frame.
 */
HML_API hml_eth_status_t hml_eth_recv(void *out, size_t cap, size_t *out_len,
                                      int timeout_ms);

/* Number of frames the DMA has handed to the PHY since init. Used by the example
 * to report that the wire really moved, rather than that a register was poked. */
HML_API uint32_t hml_eth_tx_frames(void);

/* Subset of hml_eth_tx_frames() whose Ethernet type is ARP (0x0806), counted by
 * inspecting the header the driver was handed. The example needs the split
 * because the frames that prove a datagram reached the wire are not the frames
 * that got it there: lwIP sends an ARP request to resolve the peer, and that
 * request is a real frame on a real wire too. Counting "any frame left" as
 * success therefore reports success the moment address resolution starts, and
 * counting "two frames left" is worse, because lwIP retransmits the request on a
 * timer and two requests are not a datagram. */
HML_API uint32_t hml_eth_tx_arp_frames(void);

/* Number of frames the DMA has taken off the ring since init. */
HML_API uint32_t hml_eth_rx_frames(void);

/* True once hml_eth_init has succeeded. */
HML_API bool hml_eth_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* HML_ETH_SYNOPSYS_H */
