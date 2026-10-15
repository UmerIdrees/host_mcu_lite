/*
 * board_config.h -- stm32f4 (STM32F407-style) board constants for the
 * holoscan_mcu_lite Renode multi-vendor harness.
 *
 * WHAT THIS HEADER IS
 *   The single per-vendor C view of the memory map and the console UART. The
 *   linker script, the .resc and renode/boards/index all have to agree with it,
 *   and renode/scripts/selfcheck_vendors.sh is what makes them agree: it parses
 *   all of them and fails if any one number here differs. That is the whole
 *   point of having the file. Edit it and the CTest test goes red.
 *
 * WHAT THE NUMBERS ARE
 *   STM32F407VG-style: 1 MiB of flash at 0x08000000, 128 KiB of SRAM at
 *   0x20000000, USART2 at 0x40004400 with NVIC line 37 (USART2_IRQn in
 *   RM0090). Note that this is NOT the base platform in
 *   renode/holoscan_mcu_lite.resc, which models 2 MiB of flash, 512 KiB of SRAM
 *   and a 4 MiB SDRAM window; this row is the smaller, no-SDRAM part.
 *
 * STATUS -- READ BEFORE QUOTING ANY OF IT
 *   This header, the linker script that consumes it, the .resc that models it
 *   and the robot test that asserts on it HAVE NEVER BEEN EXECUTED. The machine
 *   this was written on has no Renode, no mono, no dotnet, no arm-none-eabi-gcc
 *   and no network access to any of them. The values are transcribed from the
 *   part's memory map as understood here and have not been checked against a
 *   copy of RM0090; where one of them could not be established with confidence
 *   it says so. Nothing in this repository may claim a green stm32f4 run.
 *
 * WHY THERE IS NO SDRAM WINDOW HERE, unlike the base platform
 *   hml_reasm_open(&r, 0) takes the library default of 1 MiB and allocates TWO
 *   buffers of that size: 2 MiB of heap, which no STM32F4 has anywhere. So this
 *   board does not model the SDRAM window at all, and the linker script puts the
 *   heap in plain internal SRAM sized for the frame cap below. The demo prints
 *   the cap and the resulting heap size on the UART on every boot, and prints
 *   HML-MCU-DEMO-WARN followed by HML-MCU-DEMO-FAIL if the allocation fails,
 *   which is exactly what would happen on real silicon too.
 */
#ifndef HML_VENDOR_BOARD_CONFIG_H
#define HML_VENDOR_BOARD_CONFIG_H

#define HML_VENDOR_NAME "stm32f4"
#define HML_VENDOR_ARCH "cortex-m4"

/* The artefact build_vendor.sh produces and the .robot loads. */
#define HML_VENDOR_ELF_NAME "stm32f4_hml_mcu_demo.elf"

/* ---- memory map. Must match firmware/platform/gcc_stm32f4_hml_mcu.ld and
 * ---- stm32f4.resc byte for byte. */
#define HML_VENDOR_FLASH_ADDR 0x08000000u
#define HML_VENDOR_FLASH_BYTES 0x100000u /* 1 MiB */
#define HML_VENDOR_SRAM_ADDR 0x20000000u
#define HML_VENDOR_SRAM_BYTES 0x20000u /* 128 KiB */

/* ---- the console. USART2, the second UART of the part. */
#define HML_VENDOR_UART_ADDR 0x40004400u
#define HML_VENDOR_UART_IRQ 37 /* USART2_IRQn, RM0090. NOT exercised: see below. */
#define HML_VENDOR_UART_NAME "USART2"

/*
 * THE INTERRUPT NUMBER IS DECORATION.
 *   The demo enables no interrupt and handles no exception. The wire-up in
 *   stm32f4.resc exists so the platform description is complete and so a future
 *   interrupt-driven console has somewhere to go. If 37 were wrong, no run
 *   would fail: that is precisely why it is pinned in renode/boards/index and
 *   cross-checked, so it cannot quietly become stale.
 */

/* ---- reassembly sizing.
 *
 * hml_reasm_open() does NOT allocate exactly two buffers of the cap: it also
 * allocates its own reassembler context, sizeof(hml_reasm_ctx) = 9488 bytes on
 * this 32-bit target. That is why the heap is a little bigger than
 * 2 * HML_VENDOR_FRAME_BYTES. HML_VENDOR_REASM_CTX_BYTES = 0x2800 covers the
 * context plus the newlib chunk headers of the three allocations; the
 * self-check (selfcheck_vendors.sh, host_vendor_selftest.c) asserts
 * heap == 2 * FRAME_BYTES + that overhead, so the emulated boot cannot be
 * short of heap without failing loudly. */
#define HML_VENDOR_FRAME_BYTES 32768u   /* the default --frame-bytes for this board */
#define HML_VENDOR_REASM_CTX_BYTES 0x2800u /* sizeof(hml_reasm_ctx)=9488, rounded up to 1 KiB
                                             * with room for 3 newlib chunk headers */
#define HML_VENDOR_HEAP_BYTES 75776u    /* == 2 * FRAME_BYTES + REASM_CTX_BYTES: the two
                                         * reassembly buffers plus the reassembler's own
                                         * context, which the sizing above makes explicit */
#define HML_VENDOR_HEAP_ADDR 0x20000000u /* the heap is in internal SRAM: no SDRAM here */
#define HML_VENDOR_STACK_BYTES 8192u    /* the linker script's .stack, at the top of SRAM */

#endif /* HML_VENDOR_BOARD_CONFIG_H */
