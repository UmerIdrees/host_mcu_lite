/*
 * gcc_microchip_sam_hml_mcu.ld -- linker script for the microchip_sam vendor
 * platform (SAME70-style Cortex-M7).
 *
 * It matches platform/board_config.h,
 * renode/vendors/microchip_sam/microchip_sam.resc and the microchip_sam row of
 * renode/boards/index. renode/scripts/selfcheck_vendors.sh parses all four and
 * fails if any number differs, so the emulated map and the linked map cannot
 * drift apart.
 *
 *   FLASH  0x00400000  2 MiB    code, rodata, the .data initialiser image
 *   SRAM   0x20400000  384 KiB  .data, .bss, stack, and the newlib heap
 *
 * NEITHER ADDRESS IS A CANONICAL ARM ONE. Flash is not at 0x08000000 and SRAM is
 * not at 0x20000000. Both are written out in full for exactly that reason: this
 * script is the kind of file where a plausible-looking 0x08000000 left over from
 * a more familiar part would link perfectly and then produce an image the
 * emulated machine has no memory for.
 *
 * CACHE AND MPU
 *   Nothing in this script enables the Cortex-M7 data cache or configures the
 *   MPU, and that is deliberate rather than an omission. hml_reasm's two buffers
 *   are allocated with malloc and then written and read by name, so with the
 *   D-cache on they would still be correct only by luck -- coherency with
 *   anything DMA-capable would not be. The image runs cache-off and MPU-off,
 *   which is correct and slow. On real silicon, turning the cache on is a
 *   bring-up task that has to come with a coherency policy for the reassembly
 *   buffers, not a line in this file.
 *
 * WHY THE HEAP IS IN INTERNAL SRAM AND SMALLER THAN THE BASE PLATFORM'S
 *   hml_reasm_open(&r, 0) takes the library default of HML_MAX_FRAME_SIZE (1
 *   MiB) and allocates TWO buffers of that size, i.e. 2 MiB of heap. The base
 *   platform can afford that because it models a 4 MiB SDRAM window and puts the
 *   heap there. This board models the part's internal SRAM only -- 384 KiB -- so
 *   it is built with --frame-bytes 131072: 266 KiB of heap after an 8 KiB stack.
 *   Same code paths, same checks, smaller frame cap. See
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
  FLASH (rx)  : ORIGIN = 0x00400000, LENGTH = 2M
  SRAM  (rwx) : ORIGIN = 0x20400000, LENGTH = 384K
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

  /* The heap. 272384 = 2 * HML_VENDOR_FRAME_BYTES(131072) + HML_VENDOR_REASM_CTX_BYTES
   * (0x2800): the two hml_reasm_open() buffers plus the reassembler's own
   * context. selfcheck_vendors.sh parses the literal below and fails if it is
   * not 2 * the frame cap + the context overhead in board_config.h and in
   * renode/boards/index. */
  .heap (NOLOAD) : ALIGN(8)
  {
    __heap_start__ = .;
    . = __heap_start__ + 266K;
    . = ALIGN(8);
    __heap_end__ = .;
  } > SRAM

  /* Stack at the top of internal SRAM. 8 KiB: the demo keeps its multi-KiB
   * scratch arrays in .bss on purpose (see app/hml_mcu_demo.c), so this is
   * headroom, not a measurement. */
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
       "the demo heap does not fit in this part's 384 KiB of SRAM")
ASSERT(__stack_top__ > __heap_end__,
       "the stack and the heap overlap in SRAM")
