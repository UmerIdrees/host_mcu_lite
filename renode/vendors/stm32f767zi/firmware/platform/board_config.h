/*
 * board_config.h -- stm32f767zi (STM32F767ZI Nucleo) board constants for the
 * holoscan_mcu_lite Renode network dataplane example.
 *
 * WHAT THIS HEADER IS
 *   The single per-vendor C view of the memory map and the console UART. The
 *   linker script, the .resc and renode/boards/index all have to agree with it,
 *   and renode/scripts/selfcheck_vendors.sh is what makes them agree: it parses
 *   all of them and fails if any one number here differs. Edit it and the
 *   <vendor>_renode_selfcheck CTest test goes red.
 *
 * WHY THIS BOARD IS DIFFERENT FROM EVERY OTHER ROW IN renode/boards/index
 *   This is the first and only row with a NETWORK PERIPHERAL. The other four
 *   vendor machines deliberately have none (see the "no network" paragraph in
 *   each of their .resc files): their images run the codec, CRC and reassembly
 *   code and print a result, and no datagram ever leaves them. That is a
 *   deliberate limit of those tests, not a claim that the part has no MAC.
 *
 *   The STM32F767ZI is a different situation. Renode models its MAC as
 *   Network.SynopsysEthernetMAC (platforms/cpus/stm32f777.repl) and the Nucleo
 *   board wires a Network.EthernetPhysicalLayer to it. So a firmware image on
 *   this board can drive the real MAC DMA, and a real Ethernet/IP/UDP frame
 *   reaches the emulated wire, where the example asserts on its bytes.
 *
 *   The MAC base and the numbers below are transcribed from Renode's own
 *   platform description, NOT from a data sheet, because Renode's description is
 *   what this image actually has to match. Where the two would disagree, this
 *   row describes the EMULATED machine. See the STATUS block.
 *
 * THE MEMORY NUMBERS ARE DELIBERATELY SMALLER THAN THE REPL
 *   Renode's stm32f777.repl declares sram at 0x20000000 with size 0x10000000
 *   (256 MiB) and flash at 0x08000000 with size 0x200000 (2 MiB). The SRAM
 *   figure is a Renode placeholder, not an STM32F767ZI feature: the real part
 *   has 512 KiB of general SRAM (RM0385) plus separate DTCM/ITCM/AXI-SRAM
 *   banks. This row models a conservative 512 KiB of SRAM at 0x20000000 and
 *   2 MiB of flash, which is a subset of what Renode provides, so the image
 *   can only touch memory the emulated machine definitely has.
 *
 * STATUS -- READ BEFORE QUOTING ANY OF IT
 *   This board is NEW. It was added to demonstrate the library's UDP/RoCE
 *   TRANSPORT path, which none of the other rows exercise. Only the numbers
 *   confirmed by an actual Renode run may be described as verified, and the
 *   claims belong to the example's own README and robot test, not to this
 *   header. An emulator is not an MCU, and a frame seen on an emulated PHY is
 *   not a frame on a wire.
 */
#ifndef HML_VENDOR_BOARD_CONFIG_H
#define HML_VENDOR_BOARD_CONFIG_H

#define HML_VENDOR_NAME "stm32f767zi"
#define HML_VENDOR_ARCH "cortex-m7"

/* The artefact build_vendor.sh produces and the .robot loads. */
#define HML_VENDOR_ELF_NAME "stm32f767zi_hml_mcu_demo.elf"

/* ---- memory map. Must match firmware/platform/gcc_stm32f767zi_hml_mcu.ld and
 * ---- stm32f767zi.resc byte for byte. */
#define HML_VENDOR_FLASH_ADDR 0x08000000u
#define HML_VENDOR_FLASH_BYTES 0x200000u /* 2 MiB: what Renode's repl declares */
#define HML_VENDOR_SRAM_ADDR 0x20000000u
#define HML_VENDOR_SRAM_BYTES 0x80000u /* 512 KiB: a conservative subset of
                                        * Renode's 0x10000000 placeholder, and
                                        * close to the real part's SRAM total */

/* ---- the console. USART3, whose model on this part is UART.STM32F7_USART. */
#define HML_VENDOR_UART_ADDR 0x40004800u
#define HML_VENDOR_UART_IRQ 39 /* usart3 IRQ -> nvic@39 in platforms/cpus/stm32f777.repl.
                                * NOT exercised: the demo takes no interrupt, see
                                * startup_stm32f767zi.c. It is pinned here and
                                * cross-checked so it cannot quietly go stale. */
#define HML_VENDOR_UART_NAME "USART3"

/* ---- the network peripheral. THIS IS THE POINT OF THIS ROW.
 *
 * ethernet: Network.SynopsysEthernetMAC @ sysbus 0x40028000
 *     -> nvic@61
 * phy: Network.EthernetPhysicalLayer @ ethernet 0
 *
 * transcribed from platforms/cpus/stm32f777.repl and
 * platforms/boards/nucleo_f767zi.repl. The base address is what the MAC shim
 * writes; the IRQ is decoration, exactly as the console's is, because the
 * example polls the TX descriptor ring rather than taking the interrupt. */
#define HML_VENDOR_ETH_ADDR 0x40028000u
#define HML_VENDOR_ETH_IRQ 61 /* ethernet -> nvic@61 */
#define HML_VENDOR_ETH_NAME "EMAC"

/*
 * The re-asm sizing arithmetic below is the same invariant every row obeys, and
 * selfcheck_vendors.sh proves it by parsing this header, the linker script and
 * the index row: heap_bytes == 2 * frame_bytes + REASM_CTX_BYTES. hml_reasm_open()
 * allocates TWO frame buffers AND its own reassembler context.
 *
 * THIS BOARD RUNS A DIFFERENT DEMO BODY, so the frame cap is sized for the
 * network path instead: the dataplane example needs a frame it can put on the
 * wire, and lwIP plus its descriptor rings need room, so 64 KiB of frame with
 * the heap in SRAM is the budget. The invariant is unchanged.
 */
#define HML_VENDOR_FRAME_BYTES 65536u
#define HML_VENDOR_REASM_CTX_BYTES 0x2800u /* sizeof(hml_reasm_ctx)=9488, rounded up to
                                             * 1 KiB with room for 3 newlib chunk headers */
#define HML_VENDOR_HEAP_BYTES 141312u /* == 2 * FRAME_BYTES + REASM_CTX_BYTES */
#define HML_VENDOR_HEAP_ADDR 0x20000000u
#define HML_VENDOR_STACK_BYTES 16384u /* 16 KiB, not 8: this row also runs lwIP, whose
                                        * pbuf and TCP/IP state are stack-hungry */

#endif /* HML_VENDOR_BOARD_CONFIG_H */
