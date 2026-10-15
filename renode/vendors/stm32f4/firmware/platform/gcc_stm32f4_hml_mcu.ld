/*
 * gcc_stm32f4_hml_mcu.ld -- linker script for the stm32f4 vendor platform.
 *
 * It matches platform/board_config.h, renode/vendors/stm32f4/stm32f4.resc and
 * the stm32f4 row of renode/boards/index. renode/scripts/selfcheck_vendors.sh
 * parses all four and fails if any number differs, so the emulated map and the
 * linked map cannot drift apart.
 *
 *   FLASH  0x08000000  1 MiB   code, rodata, the .data initialiser image
 *   SRAM   0x20000000  128 KiB .data, .bss, stack, and the newlib heap
 *
 * WHY THE HEAP IS IN INTERNAL SRAM AND WHY IT IS SMALLER THAN THE BASE
 * PLATFORM'S
 *   hml_reasm_open(&r, 0) takes the library default of HML_MAX_FRAME_SIZE (1
 *   MiB) and allocates TWO buffers of that size, i.e. 2 MiB of heap. The base
 *   platform (renode/holoscan_mcu_lite.resc) can afford that because it models a
 *   4 MiB SDRAM window and puts the heap there. An STM32F407 has no external
 *   SDRAM at all: 128 KiB of SRAM is the whole budget, and 8 KiB of stack plus
 *   roughly 5 KiB of .bss leaves a little under 112 KiB. So this board is built
 *   with --frame-bytes 32768 (75776 bytes of heap, see board_config.h), which is
 *   a cap the library's own reassembly checks accept -- renode/scripts/
 *   selfcheck_vendors.sh and the host test <vendor>_renode_native_selftest both
 *   check that, and the image prints the cap and the heap size it actually got
 *   on the UART on every boot.
 *
 *   Nothing in the library is changed to make this work: the script only decides
 *   where the heap lives and how big it is. If hml_reasm_open() cannot
 *   allocate, the image emits HML-MCU-DEMO-WARN and then HML-MCU-DEMO-FAIL,
 *   which fails the robot test. That is the intended behaviour on a part too
 *   small for the default, not something to paper over.
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
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1M
  SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
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
   * context. renode/scripts/selfcheck_vendors.sh parses the literal below and
   * fails if it is not 2 * the frame cap + the context overhead in
   * board_config.h and in renode/boards/index. */
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
       "the demo heap does not fit in this part's 128 KiB of SRAM")
ASSERT(__stack_top__ > __heap_end__,
       "the stack and the heap overlap in SRAM")
ASSERT(__stack_top__ <= ORIGIN(SRAM) + LENGTH(SRAM),
       "the stack does not fit at the top of this part's 128 KiB of SRAM")
