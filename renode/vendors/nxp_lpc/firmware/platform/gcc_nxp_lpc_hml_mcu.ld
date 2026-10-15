/*
 * gcc_nxp_lpc_hml_mcu.ld -- linker script for the nxp_lpc vendor platform.
 *
 * It matches platform/board_config.h, renode/vendors/nxp_lpc/nxp_lpc.resc and
 * the nxp_lpc row of renode/boards/index. renode/scripts/selfcheck_vendors.sh
 * parses all four and fails if any number differs, so the emulated map and the
 * linked map cannot drift apart.
 *
 *   FLASH  0x1A000000  512 KiB  code, rodata, the .data initialiser image
 *   SRAM   0x10000000  136 KiB  .data, .bss, stack, and the newlib heap
 *
 * NOTE ON THE FLASH ADDRESS, because it is the thing that makes this row
 * different from every other: 0x1A000000 is where the LPC43xx executes code
 * in place from, out of its serial flash. So there is no separate "load flash"
 * and no boot copy -- the image IS the XIP region, and the Cortex-M reset vector
 * table sits at 0x1A000000. nxp_lpc.resc spells that address out literally as
 * the VectorTableOffset, and selfcheck_vendors.sh checks that it equals the
 * FLASH ORIGIN below.
 *
 * NOTE ON THE SRAM: 136 KiB is modelled as ONE contiguous window at 0x10000000.
 * The real part's SRAM is split across banks (UM10539); a linker script needs a
 * contiguous region and the demo's heap has to live in one place, so this is a
 * stated simplification rather than a claim about the silicon. See
 * board_config.h.
 *
 * WHY THE HEAP IS IN INTERNAL SRAM AND WHY IT IS SMALLER THAN THE BASE
 * PLATFORM'S
 *   hml_reasm_open(&r, 0) takes the library default of HML_MAX_FRAME_SIZE (1
 *   MiB) and allocates TWO buffers of that size, i.e. 2 MiB of heap. The base
 *   platform can afford that because it models a 4 MiB SDRAM window and puts
 *   the heap there. This part has no SDRAM in this platform, so this board is
 *   built with --frame-bytes 32768: 74 KiB of heap out of 136 KiB of SRAM,
 *   after an 8 KiB stack. Same code paths, same checks, smaller frame cap. See
 *   docs/renode_multi_vendor.md section 4.
 *
 * STATUS
 *   This script has NEVER BEEN FED TO A LINKER. The machine it was written on
 *   has no arm-none-eabi-gcc and no Renode. It is transcribed from the base
 *   platform's script (renode/firmware/freertos_demo/platform/gcc_arm_hml_mcu.ld)
 *   with the regions changed; if a region here is wrong the image will either
 *   fail to link or land somewhere the emulated machine does not have, and
 *   run_vendor_smoke.sh turns that into a FAILURE, never a skip.
 */

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x1A000000, LENGTH = 512K
  SRAM  (rwx) : ORIGIN = 0x10000000, LENGTH = 136K
}

ENTRY(Reset_Handler)

SECTIONS
{
  .isr_vector : ALIGN(4)
  {
    KEEP(*(.isr_vector))
  } > FLASH

  .text : ALIGN(4)
  {
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.glue_7) *(.glue_7t)
  } > FLASH

  .ARM.exidx : ALIGN(4)
  {
    __exidx_start = .;
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    __exidx_end = .;
  } > FLASH

  .data : ALIGN(4)
  {
    __data_start__ = .;
    *(.data .data.*)
    . = ALIGN(4);
    __data_end__ = .;
  } > SRAM AT > FLASH
  __data_load__ = LOADADDR(.data);

  .bss (NOLOAD) : ALIGN(4)
  {
    __bss_start__ = .;
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(4);
    __bss_end__ = .;
  } > SRAM

  /* The heap. 75776 = 2 * HML_VENDOR_FRAME_BYTES(32768) + HML_VENDOR_REASM_CTX_BYTES
   * (0x2800): the two hml_reasm_open() buffers plus the reassembler's own
   * context. selfcheck_vendors.sh parses the literal below and fails if it is
   * not 2 * the frame cap + the context overhead in board_config.h and in
   * renode/boards/index. */
  .heap (NOLOAD) : ALIGN(8)
  {
    __heap_start__ = .;
    . = __heap_start__ + 74K;
    . = ALIGN(8);
    __heap_end__ = .;
  } > SRAM

  /* Stack at the top of internal SRAM. 8 KiB: the demo keeps its multi-KiB
   * scratch arrays in .bss on purpose (see app/hml_mcu_demo.c), so this is
   * headroom, not a measurement. It MUST come after .heap in this list: the
   * region below asserts the stack sits above the heap, and a .stack emitted
   * first would put __stack_top__ underneath __heap_end__ and fail the link
   * even though the two fit. */
  .stack (NOLOAD) : ALIGN(8)
  {
    . = ALIGN(8);
    __stack_bottom__ = .;
    . = . + 8K;
    . = ALIGN(8);
    __stack_top__ = .;
  } > SRAM

  /DISCARD/ :
  {
    *(.note.GNU-stack)
    *(.comment)
  }
}

ASSERT(__heap_end__ <= ORIGIN(SRAM) + LENGTH(SRAM),
       "the demo heap does not fit in this part's 136 KiB of SRAM")
ASSERT(__stack_top__ > __heap_end__,
       "the stack and the heap overlap in SRAM")
ASSERT(__stack_top__ <= ORIGIN(SRAM) + LENGTH(SRAM),
       "the stack does not fit at the top of this part's 136 KiB of SRAM")
