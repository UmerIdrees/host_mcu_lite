/*
 * hml_eth_synopsys.c -- see hml_eth_synopsys.h for scope and honesty rules.
 *
 * ---------------------------------------------------------------------------
 * THE TWO AUTHORITATIVE TABLES
 * ---------------------------------------------------------------------------
 * Both layouts below were extracted from the Renode 1.17.0 binary this firmware
 * runs against. The extraction was mechanical, from the [Offset] and [Width]
 * attributes on the descriptor structs in Infrastructure.dll:
 *
 *   ikdasm Infrastructure.dll
 *   .class ... SynopsysEthernetMAC/STM32FNormalTxDescriptor
 *   .class ... SynopsysEthernetMAC/STM32FNormalRxDescriptor
 *
 * The attribute blobs are little-endian int32 tuples of the form
 * (marker, bitOffset, _, _, wordIndex). `wordIndex` is which of the descriptor's
 * four 32-bit words the field lives in. That is what the "_wN" suffix on each
 * bit position below means. A field's absence from a word is a deliberate
 * statement, not an oversight.
 *
 * STM32FNormalTxDescriptor -- 4 words, LeastSignificantByteFirst
 *   word 0  Owned                  bit 31   width 1
 *   word 0  InterruptOnCompletion  bit 30   width 1
 *   word 0  LastSegment            bit 29   width 1
 *   word 0  FirstSegment           bit 28   width 1
 *   word 0  DisableCRC             bit 27   width 1
 *   word 0  Buffer1Size            bit  0   width 13
 *   word 1  Buffer1Address         (whole word, 32 bits)
 *   word 3  Buffer2Address         (whole word)   [single-buffer TX never uses it]
 *
 * STM32FNormalRxDescriptor -- 4 words, LeastSignificantByteFirst
 *   word 0  Owned                  bit 31   width 1
 *   word 0  FrameLength            bit 16   width 14
 *   word 0  ErrorSummary           bit 15   width 1
 *   word 0  DescriptorError        bit 14   width 1
 *   word 1  DisableInterruptOnCompletion bit 31 width 1
 *   word 1  EndOfRing              bit 15   width 1
 *   word 1  FirstDescriptor        bit  9   width 1
 *   word 1  LastDescriptor         bit  8   width 1
 *   word 1  Buffer1Size            bit  0   width 13
 *   word 2  Buffer1Address         (whole word, 32 bits)
 *
 * THE MODEL SAYS THREE THINGS THAT MATTER, AND ALL THREE ARE OBEYED HERE
 *   1. The transmit path reads the frame LENGTH from Buffer1Size, not from any
 *      other field. Buffer1Address supplies the bytes. Those two are the whole
 *      contract for a single-buffer transmit.
 *   2. The next transmit descriptor is (current + 4 words). The stride is fixed.
 *   3. The model's receive path fills FrameLength itself and then reads the bytes
 *      out through Buffer1Address, so RX descriptors must point at real writable
 *      SRAM that the CPU will not touch.
 *
 * ONE FIELD IS DELIBERATELY NOT SET ON TRANSMIT
 *   Buffer2Size shares word 0 with DisableCRC, DisablePad, EndOfRing,
 *   SecondAddressChained and FirstSegment in the model's own attribute table, so
 *   those fields' declared positions are not simultaneously satisfiable. This
 *   driver never uses the second buffer: SecondAddressChained stays clear, so
 *   Buffer2Address and Buffer2Size are never read, and the three flags this
 *   driver does need (DisableCRC, FirstSegment, LastSegment) have unambiguous
 *   single-bit positions. The ambiguity is unreachable code, not a latent bug.
 */
#include "hml_eth_synopsys.h"

#include "board_config.h"

/* ------------------------------------------------------------------------- */
/* MMIO. Offsets are the model's own register names, in ascending order.        */
/* ------------------------------------------------------------------------- */
#define MAC_BASE HML_VENDOR_ETH_ADDR

#define REG_MACCR (MAC_BASE + 0x0000u) /* configuration control */
#define REG_MACFFR (MAC_BASE + 0x0004u) /* frame filter */
#define REG_MACMIIAR (MAC_BASE + 0x0008u) /* MII arbitration */
#define REG_MACMIIDR (MAC_BASE + 0x0010u) /* MII data */
#define REG_DMABMR (MAC_BASE + 0x1000u) /* bus mode */
#define REG_DMATPDR (MAC_BASE + 0x1004u) /* TX poll demand */
#define REG_DMARPDR (MAC_BASE + 0x1008u) /* RX poll demand */
#define REG_DMARDLAR (MAC_BASE + 0x100Cu) /* RX descriptor list address */
#define REG_DMATDLAR (MAC_BASE + 0x1010u) /* TX descriptor list address */
#define REG_DMASR (MAC_BASE + 0x1014u) /* status */
#define REG_DMAOMR (MAC_BASE + 0x1018u) /* operation mode */
#define REG_DMAIER (MAC_BASE + 0x101Cu) /* interrupt enable */
#define REG_MACA0HR (MAC_BASE + 0x0040u) /* MAC address 0, high */
#define REG_MACA0LR (MAC_BASE + 0x0044u) /* MAC address 0, low */

/* MACCR */
#define MACCR_RE (1u << 2)  /* receive enable */
#define MACCR_TE (1u << 3)  /* transmit enable */
#define MACCR_PS (1u << 15) /* port select */

/* MACFFR: accept everything addressed to us, and pass broadcast and multicast. */
#define MACFFR_PM (1u << 0) /* promiscuous: on, so the capture is never filtered */
#define MACFFR_PDA (1u << 5)
#define MACFFR_BM (1u << 3)
#define MACFFR_HM (1u << 4)

/* DMABMR: software reset, and a descriptor skip length of 0.
 *
 * The descriptor skip length is bits 2-6 in this map, NOT the bits 9-11 the
 * generic Synopsys manual shows: the register layout here is the STM32F one, and
 * the value written is 0 either way, so only the field position was wrong. */
#define DMABMR_SR (1u << 0)
#define DMABMR_DSL_MASK (7u << 2)
#define DMABMR_DSL_0 (0u << 2)

/* DMASR: every status bit is write-one-to-clear, so a plain read is the only
 * thing that leaves them set. Transmit status is bit 0, not bit 13 (bit 13 is
 * DMASR.FBES). Unused by this driver, which polls descriptors rather than IRQs. */
#define DMASR_TS (1u << 0) /* transmit status */

/* DMAOMR: receive and transmit start, so both halves of the engine are armed.
 *
 * THESE BIT POSITIONS WERE THE BUG THAT MADE THE NETWORK TEST SEND NOTHING.
 * They were written as 0/1/12/13 -- the numbering of the generic Synopsys EMAC
 * manual -- but this peripheral is the STM32F layout, where Renode's model
 * implements:
 *     bit  1  SR  start/stop receive
 *     bit  2  OSF operate on second frame
 *     bit 13  ST  start/stop transmission
 *     bit 20  FTF flush transmit FIFO
 * with bits 0 and 12-19 reserved. Setting bit 1 in the belief that it was "ST"
 * did enable the RECEIVE path, which is why RX looked plausible, while the real
 * ST at bit 13 was never set: the model logged "TX DMA processing requested, but
 * DMAOMR Start/stop transmission is disabled" and returned without reading a
 * single descriptor. No error, no frame, and the demo simply retried until its
 * budget ran out. The two numbering schemes differ by 12, so nothing in the
 * image could notice: the failure mode was silence, not a fault.
 */
#define DMAOMR_SR (1u << 1)
#define DMAOMR_OSF (1u << 2)
#define DMAOMR_ST (1u << 13)
#define DMAOMR_FTF (1u << 20) /* flush transmit FIFO */

/* ------------------------------------------------------------------------- */
/* Descriptor and buffer storage.                                               */
/* ------------------------------------------------------------------------- */
/* The rings and the frames they point at must be in SRAM the MAC's DMA can reach
 * and the CPU does not scribble on. They are aligned to a cache line and to 4
 * bytes; the descriptors are word-indexed and must be 4-byte aligned, and a 16-byte
 * struct keeps the 4-word stride exact. */
typedef struct {
  uint32_t desc[4];
} hml_eth_desc_t;

_Static_assert(sizeof(hml_eth_desc_t) == HML_ETH_DESC_BYTES,
               "descriptor must be exactly 4 words: the model's stride is fixed");

/* The RX frames land in their own buffers rather than in the ring, so a received
 * frame is never overwritten by the descriptor the DMA is filling.
 *
 * One buffer PER DESCRIPTOR, not a smaller pool shared between them. Sharing
 * looks like a saving and is not: two armed descriptors naming the same buffer
 * means the DMA may be writing into the buffer whose contents the CPU is copying
 * out, and the corruption is intermittent by construction. 4 * 1536 is 6 KiB of
 * bss on a part with 512 KiB, so the saving was never worth the aliasing. */
#define HML_ETH_FRAME_SLOTS HML_ETH_RX_RING
#define HML_ETH_FRAME_STRIDE 1536u

static hml_eth_desc_t hml_eth_tx_ring[HML_ETH_TX_RING];
static hml_eth_desc_t hml_eth_rx_ring[HML_ETH_RX_RING];
static uint8_t hml_eth_tx_buf[HML_ETH_MAX_FRAME] __attribute__((aligned(32)));
static uint8_t hml_eth_rx_buf[HML_ETH_FRAME_SLOTS][HML_ETH_FRAME_STRIDE]
    __attribute__((aligned(32)));

/* Which buffer each receive descriptor currently points at. Kept beside the ring
 * rather than derived from an index modulo the slot count: two descriptors can
 * legitimately name the same buffer slot at different times, and re-deriving the
 * mapping is how a received frame ends up being read out of the wrong one. */
static uint8_t *hml_eth_rx_owner[HML_ETH_RX_RING];

static bool hml_eth_initialised;
static uint32_t hml_eth_tx_done;
static uint32_t hml_eth_tx_arp_done;
static uint32_t hml_eth_rx_done;

/* Index of the descriptor the DMA is currently pointed at in each ring. */
static uint32_t hml_eth_tx_next;
static uint32_t hml_eth_rx_next;

/* ------------------------------------------------------------------------- */
/* MMIO helpers. Volatile on both sides: these are device registers, not memory. */
/* ------------------------------------------------------------------------- */
static void hml_eth_wr(uint32_t reg, uint32_t value) {
  *(volatile uint32_t *)reg = value;
}

static uint32_t hml_eth_rd(uint32_t reg) {
  return *(volatile uint32_t *)reg;
}

/* SysTick, and the one millisecond it is reprogrammed for by systick_init(). Only
 * COUNTFLAG is used: TICKINT is deliberately left clear in this image, so there is
 * no SysTick_Handler and no interrupt to wait on. */
#define HML_ETH_SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define HML_ETH_SYST_COUNTFLAG (1u << 16)

/* A millisecond clock local to this driver, for the bounded waits below.
 *
 * It exists because hml_clock_us() cannot be the deadline source here, and using it
 * anyway silently disabled every timeout in this file. hml_clock_us() is derived
 * from xTaskGetTickCount(), and the tick only moves inside the FreeRTOS stub's
 * wait_ticks(), which is reached from vTaskDelay(), taskYIELD() and the scheduler.
 * A driver poll calls none of those, so the tick is frozen for the whole spin and a
 * deadline compared against it is unreachable: the loop can only leave when the
 * awaited condition becomes true. Every "bounded" wait in this driver was
 * therefore an infinite loop in exactly the case the timeout exists for -- a DMA
 * that never clears OWN, or a descriptor that never fills.
 *
 * The bug was invisible in Renode because the model completes each descriptor
 * synchronously, so no wait here has ever expired. It would surface the first time
 * a wait had to time out, which is the opposite of when a timeout is useful.
 *
 * So the waits count COUNTFLAG edges themselves. Hardware sets COUNTFLAG when the
 * counter wraps and reading SYST_CSR clears it, so the loop below both observes and
 * consumes each edge, exactly as the stub's wait_ticks() does. */
static uint32_t hml_eth_elapsed_ms(uint32_t *edges) {
  while ((HML_ETH_SYST_CSR & HML_ETH_SYST_COUNTFLAG) != 0u) {
    (*edges)++;
  }
  return *edges;
}

/* A bounded spin. The timeout exists so a wedged DMA reports a failure instead of
 * hanging the image: a run that never reaches its DONE line is indistinguishable
 * from a run that was never started, and the harness treats those differently. */
static bool hml_eth_wait_until(volatile uint32_t *reg, uint32_t mask, bool want_set,
                               int timeout_ms) {
  uint32_t edges = 0u;
  for (;;) {
    const bool set = (hml_eth_rd((uint32_t)(uintptr_t)reg) & mask) != 0u;
    if (set == want_set) {
      return true;
    }
    if (hml_eth_elapsed_ms(&edges) >= (uint32_t)timeout_ms) {
      return false;
    }
  }
}

/* ------------------------------------------------------------------------- */
/* Descriptor field accessors.                                                 */
/*                                                                            */
/* These are the only place in the driver that knows a bit position, which is    */
/* what keeps the extracted layout above honest: if a field moves in a future    */
/* Renode, this block is the only thing that has to move with it.               */
/* ------------------------------------------------------------------------- */
static void hml_eth_tx_desc_init(hml_eth_desc_t *d, const void *buf, uint32_t len,
                                bool end_of_ring) {
  d->desc[0] = 0u;
  /* The frame LENGTH does not go here. In the STM32F normal-mode transmit
   * descriptor word 0 is status and control only (OWN, FS, LS, DisableCRC,
   * EndOfRing, and the checksum-insertion mode), and the length is Buffer1Size
   * in word 1 bits 0:12. Writing the length into word 0 was wrong in a way that
   * still "worked": the model read the low 13 bits of word 1 as the length and
   * word 2 as the buffer address, so a 42-byte ARP request went out as a
   * "6336 byte frame from address 0x0" -- the low 13 bits of the real buffer
   * address, read as a length, with the address word read as zero. It produced a
   * plausible frame count on the wire and a completely bogus frame. */
  /* DisableCRC: let the MAC build the FCS. lwIP 2.2.1 has no checksum offload
   * path, so the MAC is the only thing that appends the trailer. */
  d->desc[0] |= (1u << 27);
  /* FirstSegment and LastSegment: this driver always hands over one complete
   * frame in one buffer, so both are set unconditionally rather than tracked. */
  d->desc[0] |= (1u << 28);
  d->desc[0] |= (1u << 29);
  /* EndOfRing, word 0 bit 21: the marker that tells the DMA engine where the list
   * stops. It is set on the last descriptor and nowhere else, so the ring is a
   * ring rather than an unbounded forward walk into whatever follows it in
   * memory. This driver waits for completion rather than queueing ahead, so a
   * missing EOR would not show up in this image's behaviour -- which is exactly
   * why it is set explicitly instead of "because nothing crashes". */
  if (end_of_ring) {
    d->desc[0] |= (1u << 21);
  }
  /* Buffer1Size, word 1 bits 0:12, is the field the model reads the transmit
   * length from. A frame longer than the field can express is a programming
   * error, not something to silently truncate. */
  d->desc[1] = (len & 0x1FFFu);
  d->desc[2] = (uint32_t)(uintptr_t)buf;
  d->desc[3] = 0u; /* Buffer2Address; unused because the chain flag is clear */
}

static bool hml_eth_tx_desc_owned(const hml_eth_desc_t *d) {
  return (d->desc[0] & (1u << 31)) != 0u;
}

static void hml_eth_rx_desc_init(hml_eth_desc_t *d, void *buf, bool end_of_ring) {
  d->desc[0] = 0u;
  /* Owned, word 0 bit 31: the buffer belongs to the DMA until it fills it. */
  d->desc[0] |= (1u << 31);
  /* Word 0 bit 16 width 14 is FrameLength, and it is the DMA's to write, not
   * ours. Leaving it zero is what tells hml_eth_recv this descriptor is still
   * empty, so it must not be pre-set here. */
  d->desc[1] = (HML_ETH_FRAME_STRIDE & 0x1FFFu); /* Buffer1Size, word 1 bit 0 */
  if (end_of_ring) {
    d->desc[1] |= (1u << 15); /* EndOfRing, word 1 bit 15 */
  }
  d->desc[2] = (uint32_t)(uintptr_t)buf; /* Buffer1Address, whole word 2 */
  d->desc[3] = 0u;                      /* Buffer2Address, unused */
}

static bool hml_eth_rx_desc_owned(const hml_eth_desc_t *d) {
  return (d->desc[0] & (1u << 31)) != 0u;
}

static uint32_t hml_eth_rx_desc_len(const hml_eth_desc_t *d) {
  return (d->desc[0] >> 16) & 0x3FFFu; /* FrameLength, word 0 bit 16 width 14 */
}

/* ------------------------------------------------------------------------- */
/* Public API.                                                                 */
/* ------------------------------------------------------------------------- */
hml_eth_status_t hml_eth_init(const uint8_t mac_address[6]) {
  if (mac_address == NULL) {
    return HML_ETH_ERR_NULL;
  }
  if ((mac_address[0] & 1u) != 0u) {
    /* A multicast or broadcast source address is never legal for our own MAC. */
    return HML_ETH_ERR_ARG;
  }

  /* Start from a known state: the DMA block software reset is bit 0 of DMABMR
   * and self-clears. Nothing else is touched until it has. */
  hml_eth_wr(REG_DMABMR, DMABMR_SR);
  if (!hml_eth_wait_until((volatile uint32_t *)(uintptr_t)REG_DMABMR, DMABMR_SR, false, 1000)) {
    return HML_ETH_ERR_TIMEOUT;
  }
  /* Descriptor skip length 0, i.e. 4 words, which is the stride the model uses. */
  hml_eth_wr(REG_DMABMR, DMABMR_DSL_0);

  /* Build both rings before either is announced to the DMA. */
  hml_eth_tx_next = 0u;
  hml_eth_rx_next = 0u;
  hml_eth_tx_done = 0u;
  hml_eth_tx_arp_done = 0u;
  hml_eth_rx_done = 0u;

  for (uint32_t i = 0u; i < HML_ETH_RX_RING; i++) {
    const bool last = (i + 1u == HML_ETH_RX_RING);
    hml_eth_rx_owner[i] = hml_eth_rx_buf[i % HML_ETH_FRAME_SLOTS];
    hml_eth_rx_desc_init(&hml_eth_rx_ring[i], hml_eth_rx_owner[i], last);
  }
  /* The ring's descriptors are handed to the DMA in address order, and the
   * model's EndOfRing flag is what makes it wrap rather than run off the end. */
  for (uint32_t i = 0u; i < HML_ETH_TX_RING; i++) {
    hml_eth_tx_ring[i].desc[0] = 0u; /* not owned: the ring is idle */
    hml_eth_tx_ring[i].desc[1] = 0u;
    hml_eth_tx_ring[i].desc[2] = 0u;
    hml_eth_tx_ring[i].desc[3] = 0u;
  }

  hml_eth_wr(REG_DMARDLAR, (uint32_t)(uintptr_t)&hml_eth_rx_ring[0]);
  hml_eth_wr(REG_DMATDLAR, (uint32_t)(uintptr_t)&hml_eth_tx_ring[0]);

  /* MAC address 0: six bytes, most significant first, split across the low
   * register's 31 bits and the high register's upper 16. */
  hml_eth_wr(REG_MACA0LR, ((uint32_t)mac_address[0] << 0) | ((uint32_t)mac_address[1] << 8) |
                               ((uint32_t)mac_address[2] << 16) |
                               ((uint32_t)mac_address[3] << 24));
  hml_eth_wr(REG_MACA0HR, ((uint32_t)mac_address[4] << 0) | ((uint32_t)mac_address[5] << 8));

  /* Accept everything addressed to this MAC. The frame filter is left permissive
   * on purpose: the example asserts on what the MAC emits, and a filter that
   * silently dropped an address would make a passing capture meaningless.
   *
   * Promiscuous mode is what a real part would want here. Renode's model treats
   * MACFFR bit 0 as an unimplemented tag, so the write is a no-op there and the
   * model logs it; nothing in this test depends on it, because every frame the
   * test injects is addressed to this MAC to begin with. */
  hml_eth_wr(REG_MACFFR, MACFFR_PM);
  hml_eth_wr(REG_MACCR, MACCR_PS | MACCR_TE | MACCR_RE);

  /* Start both halves of the engine and flush the transmit FIFO in ONE write.
   *
   * The two separate writes this used to be ("flush", then "start") is a real bug,
   * and a quiet one: a register write REPLACES the whole register rather than
   * merging into it, so the second write silently cleared the start bits the
   * first had just set. The model then refused every transmit poll with "TX DMA
   * processing requested, but DMAOMR Start/stop transmission is disabled" and
   * every receive poll with the matching receive complaint, while the image
   * reported nothing but "no datagram". One write, all the bits.
   *
   * In this map ST is bit 13 and SR is bit 1, and the model runs the transmit
   * path the moment either is set, so both are set in the same write rather than
   * in two, for the same reason. */
  hml_eth_wr(REG_DMAOMR, DMAOMR_SR | DMAOMR_ST | DMAOMR_FTF);
  hml_eth_wr(REG_DMAIER, 0u); /* polled: no interrupt is wanted or taken */

  hml_eth_wr(REG_DMARPDR, 1u); /* ask the DMA to look at the receive ring */

  hml_eth_initialised = true;
  return HML_ETH_OK;
}

hml_eth_status_t hml_eth_send(const void *frame, size_t len, int timeout_ms) {
  if (!hml_eth_initialised) {
    return HML_ETH_ERR_ARG;
  }
  if (frame == NULL) {
    return HML_ETH_ERR_NULL;
  }
  /* A minimum Ethernet frame is 60 bytes excluding the FCS. Anything shorter is
   * malformed, and the MAC would pad it anyway, so send what the caller built. */
  if (len < 14u) {
    return HML_ETH_ERR_ARG;
  }
  if (len > HML_ETH_MAX_FRAME) {
    return HML_ETH_ERR_TOO_LONG;
  }

  /* Wait for a free descriptor. With a ring this shallow it is always immediate,
   * but the loop is what makes a full ring a bounded failure rather than a
   * silent overwrite of a frame the DMA has not finished with. */
  {
    uint32_t edges = 0u;
    while (hml_eth_tx_desc_owned(&hml_eth_tx_ring[hml_eth_tx_next])) {
      if (hml_eth_elapsed_ms(&edges) >= (uint32_t)timeout_ms) {
        return HML_ETH_ERR_TIMEOUT;
      }
    }
  }

  uint8_t *const dst = hml_eth_tx_buf;
  const uint8_t *const src = (const uint8_t *)frame;
  for (size_t i = 0u; i < len; i++) {
    dst[i] = src[i];
  }

  hml_eth_tx_desc_init(&hml_eth_tx_ring[hml_eth_tx_next], dst, (uint32_t)len,
                       hml_eth_tx_next + 1u == HML_ETH_TX_RING);

  /* Only now hand the descriptor over. Setting OWN is the door: until this
   * write the DMA cannot see the buffer, so there is no window in which it
   * could transmit a half-copied frame. */
  hml_eth_tx_ring[hml_eth_tx_next].desc[0] |= (1u << 31);
  __asm__ volatile("" ::: "memory");
  hml_eth_wr(REG_DMATPDR, 1u);

  /* The DMA clears OWN when it is finished with the descriptor. That clear is the
   * only completion signal this driver trusts, because it is the same signal the
   * model actually acts on. */
  if (!hml_eth_wait_until(&hml_eth_tx_ring[hml_eth_tx_next].desc[0], (1u << 31), false,
                          timeout_ms)) {
    return HML_ETH_ERR_TIMEOUT;
  }

  hml_eth_tx_next = (hml_eth_tx_next + 1u) % HML_ETH_TX_RING;
  hml_eth_tx_done++;
  /* Counted from the header the caller handed over, not from the descriptor, so
   * this stays a statement about the bytes that went on the wire. An Ethernet
   * header is 14 bytes and the type sits at offset 12; a frame too short to have
   * one is not ARP, and neither is a frame whose type field is anything else. */
  if (len >= 14u && ((const uint8_t *)frame)[12] == 0x08u &&
      ((const uint8_t *)frame)[13] == 0x06u) {
    hml_eth_tx_arp_done++;
  }
  return HML_ETH_OK;
}

hml_eth_status_t hml_eth_recv(void *out, size_t cap, size_t *out_len, int timeout_ms) {
  if (!hml_eth_initialised) {
    return HML_ETH_ERR_ARG;
  }
  if (out == NULL || out_len == NULL) {
    return HML_ETH_ERR_NULL;
  }

  uint32_t edges = 0u;
  for (;;) {
    hml_eth_desc_t *d = &hml_eth_rx_ring[hml_eth_rx_next];
    /* The DMA clears OWN when it has filled the descriptor, and it fills
     * FrameLength at the same time. Testing both means an empty descriptor and a
     * filled one are never confused, which matters because a zero-length frame is
     * not something the emulated wire can produce. */
    if (!hml_eth_rx_desc_owned(d)) {
      hml_eth_wr(REG_DMARPDR, 1u);
      const uint32_t len = hml_eth_rx_desc_len(d);
      if (len != 0u) {
        hml_eth_status_t result = HML_ETH_OK;

        if (len > cap) {
          result = HML_ETH_ERR_TOO_LONG;
        } else {
          const uint8_t *const src = hml_eth_rx_owner[hml_eth_rx_next];
          for (uint32_t i = 0u; i < len; i++) {
            ((uint8_t *)out)[i] = src[i];
          }
          *out_len = (size_t)len;
          hml_eth_rx_done++;
        }

        /* Re-arm THIS descriptor, not the one after it. The descriptor that was
         * just consumed is the one the DMA must refill: it is the only entry in
         * the ring still marked CPU-owned, and re-initialising its successor
         * instead would leave it permanently un-armed, silently shrinking the
         * ring by one every received frame until nothing receives at all. */
        hml_eth_rx_owner[hml_eth_rx_next] = hml_eth_rx_buf[hml_eth_rx_next];
        hml_eth_rx_desc_init(&hml_eth_rx_ring[hml_eth_rx_next], hml_eth_rx_owner[hml_eth_rx_next],
                             hml_eth_rx_next + 1u == HML_ETH_RX_RING);
        hml_eth_rx_next = (hml_eth_rx_next + 1u) % HML_ETH_RX_RING;
        hml_eth_wr(REG_DMARPDR, 1u);
        return result;
      }
    }
    if (hml_eth_elapsed_ms(&edges) >= (uint32_t)timeout_ms) {
      return HML_ETH_ERR_TIMEOUT;
    }
  }
}

uint32_t hml_eth_tx_frames(void) {
  return hml_eth_tx_done;
}

uint32_t hml_eth_tx_arp_frames(void) {
  return hml_eth_tx_arp_done;
}

uint32_t hml_eth_rx_frames(void) {
  return hml_eth_rx_done;
}

bool hml_eth_ready(void) {
  return hml_eth_initialised;
}
