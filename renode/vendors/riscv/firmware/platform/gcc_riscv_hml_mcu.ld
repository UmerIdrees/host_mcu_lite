/*
 * gcc_riscv_hml_mcu.ld -- linker script for the riscv vendor platform
 * (GD32VF103-style RV32IMAC).
 *
 * It matches platform/board_config.h, renode/vendors/riscv/riscv.resc and the
 * riscv row of renode/boards/index. renode/scripts/selfcheck_vendors.sh parses
 * all four and fails if any number differs, so the emulated map and the linked
 * map cannot drift apart.
 *
 *   FLASH  0x00000000  128 KiB  code, rodata, the .data initialiser image
 *   SRAM   0x20000000  32 KiB   .data, .bss, stack, and the newlib heap
 *
 * RISC-V DIFFERENCES FROM THE ARM ROWS
 *   - There is no vector table. Reset jumps to the address programmed into the
 *     mtvec CSR, which startup_riscv.c sets to the .riscv_trap handler. The
 *     "vector" in this file is the trap entry point, not a table.
 *   - -msdata=none (see firmware/vendor.env) means no gp-relative addressing, so
 *     there is no .sdata/.sbss and startup_riscv.c does not touch the gp
 *     register.
 *   - Linker relaxation is OFF (-Wl,--no-relax). Relax would migrate code
 *     between sections at link time; for a 128 KiB image at address 0 the
 *     gain is nothing and the risk is a layout the self-check documents. 
 *   - The stack is 4 KiB and the heap 18 KiB: the smallest numbers in the
 *     matrix, because this is a 32 KiB part and hml_reasm_open's two buffers
 *     plus its reassembler context are what the heap exists for.
 *
 * WHY THE HEAP IS IN INTERNAL SRAM AND SMALLER THAN THE BASE PLATFORM'S
 *   hml_reasm_open(&r, 0) takes the library default of HML_MAX_FRAME_SIZE (1
 *   MiB) and allocates TWO buffers of that size, i.e. 2 MiB of heap. The base
 *   platform can afford that because it models a 4 MiB SDRAM window and puts the
 *   heap there. This board models the part's internal 32 KiB of SRAM only, so it
 *   is built with --frame-bytes 4096: 18 KiB of heap after a 4 KiB stack. Same
 *   code paths, same checks, smaller frame cap. See
 *   docs/renode_multi_vendor.md section 4.
 *
 * STATUS
 *   This script has NEVER BEEN FED TO A LINKER. The machine it was written on
 *   has no riscv32 toolchain and no Renode. It is adapted from the base
 *   platform's script (renode/firmware/freertos_demo/platform/gcc_arm_hml_mcu.ld)
 *   with the ARM-only sections removed; if a region here is wrong the image will
 *   either fail to link or land somewhere the emulated machine does not have,
 *   and run_vendor_smoke.sh turns that into a FAILURE, never a skip.
 */

ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 128K
  SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
}

SECTIONS
{
  .text.init : ALIGN(4)
  {
    KEEP(*(.text.init))
    KEEP(*(.text.Reset_Handler))
  } > FLASH

  .riscv_trap_vector : ALIGN(4)
  {
    KEEP(*(.riscv_trap_vector))
  } > FLASH

  .text : ALIGN(4)
  {
    *(.text .text.*)
    *(.rodata .rodata.*)
  } > FLASH

  .data : ALIGN(4)
  {
    __data_start__ = .;
    *(.data .data.*)
    *(.sdata .sdata.*)
    . = ALIGN(4);
    __data_end__ = .;
  } > SRAM AT > FLASH
  __data_load__ = LOADADDR(.data);

  /* -msdata=none: sbss is (deliberately) empty, but keep the symbol so a
   * relocated order of the .data directives still forms a sane region. */
  .bss (NOLOAD) : ALIGN(4)
  {
    __bss_start__ = .;
    *(.sbss .sbss.*)
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end__ = .;
  } > SRAM

  /* The heap MUST come BEFORE .stack in this list. If it came after it,
   * __stack_top__ would sit underneath __heap_end__ and the ASSERT at the
   * bottom -- __heap_end__ <= top of SRAM AND __stack_top__ > __heap_end__ --
   * could never hold. 18432 = 2 * HML_VENDOR_FRAME_BYTES(4096) +
   * HML_VENDOR_REASM_CTX_BYTES(0x2800): the two hml_reasm_open() buffers plus
   * the reassembler's own context. selfcheck_vendors.sh parses the literal
   * below and fails if it is not 2 * the frame cap + the context overhead in
   * board_config.h and in renode/boards/index. */
  .heap (NOLOAD) : ALIGN(8)
  {
    __heap_start__ = .;
    . = __heap_start__ + 18K;
    . = ALIGN(8);
    __heap_end__ = .;
  } > SRAM

  /* Stack at the top of internal SRAM (after the heap, so __stack_top__ really
   * is the top). 4 KiB: the demo keeps its multi-KiB scratch arrays in .bss on
   * purpose (see app/hml_mcu_demo.c), so this is headroom, not a measurement. */
  .stack (NOLOAD) : ALIGN(8)
  {
    . = ALIGN(8);
    __stack_bottom__ = .;
    . = . + 4K;
    . = ALIGN(8);
    __stack_top__ = .;
  } > SRAM

  /DISCARD/ :
  {
    *(.riscv.attributes)
    *(.comment)
    *(.note.GNU-stack)
  }
}

ASSERT(__heap_end__ <= ORIGIN(SRAM) + LENGTH(SRAM),
       "the demo heap does not fit in this part's 32 KiB of SRAM")
ASSERT(__stack_top__ > __heap_end__,
       "the stack and the heap overlap in SRAM")