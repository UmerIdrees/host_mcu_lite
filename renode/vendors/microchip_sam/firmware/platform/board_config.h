/*
 * board_config.h -- microchip_sam (SAME70-style) board constants for the
 * holoscan_mcu_lite Renode multi-vendor harness.
 *
 * WHAT THIS HEADER IS
 *   The single per-vendor C view of the memory map and the console UART. The
 *   linker script, the .resc and renode/boards/index all have to agree with it,
 *   and renode/scripts/selfcheck_vendors.sh is what makes them agree: it parses
 *   all of them and fails if any one number here differs. Edit it and the CTest
 *   test goes red.
 *
 * WHAT THE NUMBERS ARE
 *   SAME70-style: 2 MiB of flash at 0x00400000, 384 KiB of SRAM at 0x20400000,
 *   and UART0 at 0x400E0800 on NVIC line 35. Note that NEITHER window is at a
 *   canonical ARM address: the flash is above 0x00400000 rather than at
 *   0x08000000, and the SRAM is at 0x20400000 rather than 0x20000000. The flash
 *   address is a bootROM-and-bias-mapping consequence of the part; the SRAM
 *   address is where this row models internal SRAM. Both are spelled out because
 *   a copy-paste from a more familiar STM32-style map would silently produce an
 *   image that links and then goes nowhere.
 *
 * THIS IS THE ONLY CORTEX-M7 IN THE MATRIX
 *   ARMv7-M: the same reset-from-the-first-two-vector-words behaviour as every
 *   other row here, so nothing about the startup shape changes. What DOES change
 *   is what is NOT enabled, and that is deliberate. See THE CORTEX-M7 below and
 *   the note in startup_microchip_sam.c.
 *
 * STATUS -- READ BEFORE QUOTING ANY OF IT
 *   This header, the linker script that consumes it, the .resc that models it
 *   and the robot test that asserts on it HAVE NEVER BEEN EXECUTED. The machine
 *   this was written on has no Renode, no mono, no dotnet, no arm-none-eabi-gcc
 *   and no network access to any of them. No copy of the SAME70 data sheet was
 *   available to check the interrupt line against. Nothing in this repository may
 *   claim a green microchip_sam run.
 *
 * WHY THERE IS NO SDRAM WINDOW HERE, unlike the base platform
 *   hml_reasm_open(&r, 0) takes the library default of 1 MiB and allocates TWO
 *   buffers of that size: 2 MiB of heap. This board models the part's internal
 *   SRAM only, so the heap lives there, sized for the frame cap below -- 256 KiB
 *   out of 384 KiB. The demo prints the cap and the heap size on the UART every
 *   boot and prints HML-MCU-DEMO-WARN then HML-MCU-DEMO-FAIL if the allocation
 *   fails. A real SAME70 board with external SDRAM behind an FMC could hold the
 *   full default; this platform does not model that, so it does not pretend to.
 */
#ifndef HML_VENDOR_BOARD_CONFIG_H
#define HML_VENDOR_BOARD_CONFIG_H

#define HML_VENDOR_NAME "microchip_sam"
#define HML_VENDOR_ARCH "cortex-m7"

/* The artefact build_vendor.sh produces and the .robot loads. */
#define HML_VENDOR_ELF_NAME "microchip_sam_hml_mcu_demo.elf"

/* ---- memory map. Must match firmware/platform/gcc_microchip_sam_hml_mcu.ld
 * ---- and microchip_sam.resc byte for byte. */
#define HML_VENDOR_FLASH_ADDR 0x00400000u /* not 0x08000000: see the header note */
#define HML_VENDOR_FLASH_BYTES 0x200000u  /* 2 MiB */
#define HML_VENDOR_SRAM_ADDR 0x20400000u  /* not 0x20000000: see the header note */
#define HML_VENDOR_SRAM_BYTES 0x60000u    /* 384 KiB */

/* ---- the console. UART0. */
#define HML_VENDOR_UART_ADDR 0x400E0800u
#define HML_VENDOR_UART_IRQ 35 /* UART0 on the Harmony+ interrupt table. See below. */
#define HML_VENDOR_UART_NAME "UART0"

/*
 * THE INTERRUPT NUMBER IS DECORATION, AND A GUESS AT THAT.
 *   35 is the line recorded for UART0 in the Harmony+ interrupt table as
 *   understood here; no data sheet was available to confirm it. The demo enables
 *   no interrupt and handles no exception, so no run would fail either way. It is
 *   recorded in renode/boards/index and cross-checked precisely so that a guess
 *   like this is visible and cannot quietly become stale.
 */

/* ---- reassembly sizing. The biggest frame cap in the matrix, on the biggest
 * ---- part in the matrix. Still not the library default: 2 MiB of heap would not
 * ---- fit in 384 KiB of SRAM. */
#define HML_VENDOR_FRAME_BYTES 131072u
#define HML_VENDOR_REASM_CTX_BYTES 0x2800u /* sizeof(hml_reasm_ctx)=9488 rounded up, plus 3 headers */
#define HML_VENDOR_HEAP_BYTES 272384u /* == 2 * FRAME_BYTES + REASM_CTX_BYTES: the two buffers
                                       * plus the reassembler's context. 2*131072+10240=272384,
                                       * fits in 384 KiB after an 8 KiB stack. */
#define HML_VENDOR_HEAP_ADDR 0x20400000u /* internal SRAM: this board models no SDRAM */
#define HML_VENDOR_STACK_BYTES 8192u     /* the linker script's .stack, at the top of SRAM */

#endif /* HML_VENDOR_BOARD_CONFIG_H */
