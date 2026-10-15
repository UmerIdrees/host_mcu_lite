/*
 * board_config.h -- nxp_lpc (LPC4337-style) board constants for the
 * holoscan_mcu_lite Renode multi-vendor harness.
 *
 * WHAT THIS HEADER IS
 *   The single per-vendor C view of the memory map and the console UART. The
 *   linker script, the .resc and renode/boards/index all have to agree with it,
 *   and renode/scripts/selfcheck_vendors.sh is what makes them agree: it parses
 *   all of them and fails if any one number here differs. That is the whole
 *   point of having the file. Edit it and the CTest test goes red.
 *
 * WHAT THE NUMBERS ARE, AND HOW MUCH CONFIDENCE EACH ONE HAS
 *   flash  0x1A000000, 512 KiB. The LPC43xx executes code in place from the
 *          serial flash that starts at 0x1A000000. The 512 KiB figure is the
 *          part's flash size. Reasonably confident.
 *   sram   0x10000000, 136 KiB. MODELLING SIMPLIFICATION, STATED PLAINLY: the
 *          real LPC4337 splits its 136 KiB across several banks that are not one
 *          contiguous 136 KiB window (see UM10539). This harness models the one
 *          contiguous window the task specified, because a linker script cannot
 *          use a memory that is not contiguous, and because the demo's whole
 *          heap has to live in one place. If a real bring-up used the split
 *          banks, this region is the thing to fix, and the fix is in this
 *          header, the linker script and nxp_lpc.resc -- all three of which the
 *          self-check keeps in agreement.
 *   uart   0x400C1000, USART0. The LPC43xx USART0 base. Reasonably confident.
 *          Note that 0x400C0000, sometimes quoted for "the LPC UART block", is
 *          not USART0; 0x400C1000 is.
 *   irq    58. The USART0 line in the Cortex-M4 NVIC numbering the LPC43xx
 *          uses. LEAST CONFIDENT NUMBER IN THIS HEADER, and the least
 *          consequential: see below.
 *
 * STATUS -- READ BEFORE QUOTING ANY OF IT
 *   This header, the linker script that consumes it, the .resc that models it
 *   and the robot test that asserts on it HAVE NEVER BEEN EXECUTED. The machine
 *   this was written on has no Renode, no mono, no dotnet, no arm-none-eabi-gcc
 *   and no network access to any of them. No copy of UM10539 was available to
 *   check any of the above against. Nothing in this repository may claim a green
 *   nxp_lpc run.
 *
 * WHY THERE IS NO SDRAM WINDOW HERE, unlike the base platform
 *   hml_reasm_open(&r, 0) takes the library default of 1 MiB and allocates TWO
 *   buffers of that size: 2 MiB of heap, which this part does not have. So this
 *   board does not model an SDRAM window at all and the linker script puts the
 *   heap in plain internal SRAM, sized for the frame cap below. The demo prints
 *   the cap and the resulting heap size on the UART on every boot and prints
 *   HML-MCU-DEMO-WARN then HML-MCU-DEMO-FAIL if the allocation fails.
 */
#ifndef HML_VENDOR_BOARD_CONFIG_H
#define HML_VENDOR_BOARD_CONFIG_H

#define HML_VENDOR_NAME "nxp_lpc"
#define HML_VENDOR_ARCH "cortex-m4"

/* The artefact build_vendor.sh produces and the .robot loads. */
#define HML_VENDOR_ELF_NAME "nxp_lpc_hml_mcu_demo.elf"

/* ---- memory map. Must match firmware/platform/gcc_nxp_lpc_hml_mcu.ld and
 * ---- nxp_lpc.resc byte for byte. */
#define HML_VENDOR_FLASH_ADDR 0x1A000000u /* serial flash, executed in place */
#define HML_VENDOR_FLASH_BYTES 0x80000u   /* 512 KiB */
#define HML_VENDOR_SRAM_ADDR 0x10000000u  /* ONE contiguous window: see the header note */
#define HML_VENDOR_SRAM_BYTES 0x22000u    /* 136 KiB */

/* ---- the console. USART0. */
#define HML_VENDOR_UART_ADDR 0x400C1000u
#define HML_VENDOR_UART_IRQ 58 /* USART0 on the LPC43xx NVIC numbering. LEAST CONFIDENT
                                * NUMBER HERE; see the header note and below. */
#define HML_VENDOR_UART_NAME "USART0"

/*
 * THE INTERRUPT NUMBER IS DECORATION.
 *   The demo enables no interrupt and handles no exception, so nothing in a run
 *   would notice if 58 were wrong. It is recorded in renode/boards/index and
 *   cross-checked by the self-check precisely so that a guess like this cannot
 *   quietly become stale without anybody finding out.
 */

/* ---- reassembly sizing. 136 KiB of SRAM: 8 KiB of stack and roughly 5 KiB of
 * ---- .bss leave about 120 KiB, so a 32 KiB frame cap (74 KiB of heap, see the
 * ---- REASM_CTX_BYTES note in the stm32f4 board_config.h -- it is the same
 * ---- library, so the same non-zero context overhead applies) fits with room
 * ---- to spare. */
#define HML_VENDOR_FRAME_BYTES 32768u
#define HML_VENDOR_REASM_CTX_BYTES 0x2800u /* sizeof(hml_reasm_ctx)=9488 rounded up, plus 3 headers */
#define HML_VENDOR_HEAP_BYTES 75776u /* == 2 * FRAME_BYTES + REASM_CTX_BYTES: the two buffers
                                      * plus the reassembler's own context */
#define HML_VENDOR_HEAP_ADDR 0x10000000u /* internal SRAM: this part has no SDRAM here */
#define HML_VENDOR_STACK_BYTES 8192u /* the linker script's .stack, at the top of SRAM */

#endif /* HML_VENDOR_BOARD_CONFIG_H */
