/*
 * board_config.h -- ti_tm4c (TM4C1294-style) board constants for the
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
 *   TM4C1294-style: 1 MiB of flash mapped at address 0, 256 KiB of SRAM at
 *   0x20000000, and UART0 at 0x4000C000 on NVIC line 5. This is the ONLY row in
 *   the matrix whose flash starts at address 0, and that matters for the reset
 *   vector: ti_tm4c.resc spells out `cpu VectorTableOffset 0x00000000` literally,
 *   and the self-check asserts it equals the FLASH ORIGIN in the linker script.
 *   A vector table fetched from address 0 because a monitor variable was
 *   misparsed is exactly the silent failure the base platform's .resc warns
 *   about, so it is worth spelling out here too.
 *
 * THE FPU
 *   arch is cortex-m4f, because that is what the part is and what Renode is asked
 *   to model. The firmware is nevertheless compiled soft float
 *   (-mfloat-abi=soft, see firmware/vendor.env): a demo that issued FP
 *   instructions would need CPACR CP10/CP11 set and lazy stacking configured, and
 *   none of that exists in this harness. Nothing here needs the FPU, so the code
 *   stays integer-only and the mismatch between the declared CPU and the compiled
 *   ISA is deliberate. Renode having an FPU in the model and the image never
 *   touching it is the whole story.
 *
 * STATUS -- READ BEFORE QUOTING ANY OF IT
 *   This header, the linker script that consumes it, the .resc that models it
 *   and the robot test that asserts on it HAVE NEVER BEEN EXECUTED. The machine
 *   this was written on has no Renode, no mono, no dotnet, no arm-none-eabi-gcc
 *   and no network access to any of them. No copy of the TM4C1294 data sheet was
 *   available to check the IRQ line against. Nothing in this repository may claim
 *   a green ti_tm4c run.
 *
 * WHY THERE IS NO SDRAM WINDOW HERE, unlike the base platform
 *   hml_reasm_open(&r, 0) takes the library default of 1 MiB and allocates TWO
 *   buffers of that size: 2 MiB of heap, which this part does not have. So this
 *   board models no SDRAM and the linker script puts the heap in internal SRAM,
 *   sized for the frame cap below. The demo prints the cap and the heap size on
 *   the UART every boot and prints HML-MCU-DEMO-WARN then HML-MCU-DEMO-FAIL if
 *   the allocation fails.
 */
#ifndef HML_VENDOR_BOARD_CONFIG_H
#define HML_VENDOR_BOARD_CONFIG_H

#define HML_VENDOR_NAME "ti_tm4c"
#define HML_VENDOR_ARCH "cortex-m4f"

/* The artefact build_vendor.sh produces and the .robot loads. */
#define HML_VENDOR_ELF_NAME "ti_tm4c_hml_mcu_demo.elf"

/* ---- memory map. Must match firmware/platform/gcc_ti_tm4c_hml_mcu.ld and
 * ---- ti_tm4c.resc byte for byte. */
#define HML_VENDOR_FLASH_ADDR 0x00000000u /* the only row in the matrix mapped at zero */
#define HML_VENDOR_FLASH_BYTES 0x100000u  /* 1 MiB */
#define HML_VENDOR_SRAM_ADDR 0x20000000u
#define HML_VENDOR_SRAM_BYTES 0x40000u /* 256 KiB */

/* ---- the console. UART0. */
#define HML_VENDOR_UART_ADDR 0x4000C000u
#define HML_VENDOR_UART_IRQ 5 /* UART0 on the TM4C interrupt table. See below. */
#define HML_VENDOR_UART_NAME "UART0"

/*
 * THE INTERRUPT NUMBER IS DECORATION.
 *   The demo enables no interrupt and handles no exception. The wire-up in
 *   ti_tm4c.resc exists so the platform description is complete and so a future
 *   interrupt-driven console has somewhere to go. If 5 were wrong, no run would
 *   fail: that is exactly why it is recorded in renode/boards/index and
 *   cross-checked, so it cannot quietly become stale.
 */

/* ---- reassembly sizing. */
#define HML_VENDOR_FRAME_BYTES 65536u /* the default --frame-bytes for this board */
#define HML_VENDOR_REASM_CTX_BYTES 0x2800u /* sizeof(hml_reasm_ctx)=9488 rounded up, plus 3 headers */
#define HML_VENDOR_HEAP_BYTES 141312u /* == 2 * FRAME_BYTES + REASM_CTX_BYTES: the two buffers
                                       * plus the reassembler's context. 2*65536+10240=141312,
                                       * fits in 256 KiB after an 8 KiB stack. */
#define HML_VENDOR_HEAP_ADDR 0x20000000u /* internal SRAM: this board models no SDRAM */
#define HML_VENDOR_STACK_BYTES 8192u    /* the linker script's .stack, at the top of SRAM */

#endif /* HML_VENDOR_BOARD_CONFIG_H */
