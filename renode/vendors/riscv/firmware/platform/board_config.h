/*
 * board_config.h -- riscv (GD32VF103-style RV32IMAC) board constants for the
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
 *   GD32VF103-style: 128 KiB of flash at address 0, 32 KiB of SRAM at
 *   0x20000000, and a console UART at 0x40000000. This is the only NON-Cortex row
 *   in the matrix and the smallest part in it. There is no NVIC and no PLIC
 *   wired in riscv.resc: the image enables no interrupt and handles no
 *   exception, so a decorative interrupt controller would be a region of the
 *   emulated machine nothing ever touches. The UART line in renode/boards/index
 *   is therefore "-", and HML_VENDOR_UART_IRQ here is 0 with the same meaning
 *   (never delivered).
 *
 * FLASH AT ADDRESS ZERO, TAKEN LITERALLY
 *   Like the ti_tm4c row, this row maps flash at 0 -- but where the TM4C's
 *   Cortex-M has a vector table that MUST sit at 0, a RISC-V core has no vector
 *   table at all. Reset jumps to the address in the mtvec CSR, and the CSR is
 *   programmed by firmware, so an image that forgot mtvec would start somewhere
 *   the emulated machine guessed. startup_riscv.c therefore writes mtvec out
 *   before anything else, and riscv.resc's cpu VectorTableOffset 0x00000000
 *   exists only for the self-check, which asserts it equals the linker script's
 *   FLASH ORIGIN.
 *
 * STATUS -- READ BEFORE QUOTING ANY OF IT
 *   This header, the linker script that consumes it, the .resc that models it,
 *   the startup sequence and the robot test that asserts on it HAVE NEVER BEEN
 *   EXECUTED. The machine this was written on has no Renode, no mono, no dotnet,
 *   no riscv32 toolchain and no network access to any of them. No copy of a
 *   GD32VF103 data sheet was available. Nothing in this repository may claim a
 *   green riscv run.
 */
#ifndef HML_VENDOR_BOARD_CONFIG_H
#define HML_VENDOR_BOARD_CONFIG_H

#define HML_VENDOR_NAME "riscv"
#define HML_VENDOR_ARCH "rv32imac"

/* The artefact build_vendor.sh produces and the .robot loads. */
#define HML_VENDOR_ELF_NAME "riscv_hml_mcu_demo.elf"

/* ---- memory map. Must match firmware/platform/gcc_riscv_hml_mcu.ld and
 * ---- riscv.resc byte for byte. */
#define HML_VENDOR_FLASH_ADDR 0x00000000u /* like the ti_tm4c row, flash at 0: but this
                                           * row has no vector table, mtvec is program text */
#define HML_VENDOR_FLASH_BYTES 0x20000u   /* 128 KiB */
#define HML_VENDOR_SRAM_ADDR 0x20000000u
#define HML_VENDOR_SRAM_BYTES 0x8000u /* 32 KiB */

/* ---- the console. */
#define HML_VENDOR_UART_ADDR 0x40000000u
#define HML_VENDOR_UART_IRQ 0u /* NO NVIC and NO PLIC are wired; 0 is never delivered.
                                 * The board row in renode/boards/index spells this
                                 * "-"; the self-check treats the two spellings as
                                 * the same fact. */
#define HML_VENDOR_UART_NAME "UART0"

/* ---- reassembly sizing. The smallest cap in the matrix, on the smallest
 * ---- part in the matrix: 32 KiB of SRAM is too tight for the previous 8192-byte
 * ---- cap once hml_reasm_open's own context is counted (2*8192 + 10240 bytes of
 * ---- heap plus ~3 KiB of .bss plus the 4 KiB stack would not fit), so the cap
 * ---- is 4096 and the heap is 2*4096 + BREAK-size instead. The demo reassembles
 * ---- frames of at most 200 bytes, so a 4096-byte cap is still ample. */
#define HML_VENDOR_FRAME_BYTES 4096u
#define HML_VENDOR_REASM_CTX_BYTES 0x2800u /* sizeof(hml_reasm_ctx)=9488 rounded up, plus 3 headers */
#define HML_VENDOR_HEAP_BYTES 18432u /* == 2 * FRAME_BYTES + REASM_CTX_BYTES: 2*4096+10240=18432,
                                      * fits in 32 KiB after a 4 KiB stack and ~3 KiB of .bss */
#define HML_VENDOR_HEAP_ADDR 0x20000000u /* internal SRAM: this board models no SDRAM */
#define HML_VENDOR_STACK_BYTES 4096u     /* the linker script's .stack, at the top of SRAM */

#endif /* HML_VENDOR_BOARD_CONFIG_H */