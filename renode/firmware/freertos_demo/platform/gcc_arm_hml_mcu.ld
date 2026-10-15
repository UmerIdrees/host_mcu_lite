/*
 * gcc_arm_hml_mcu.ld -- minimal linker script for the Cortex-M smoke demo.
 *
 * It matches the platform description in renode/holoscan_mcu_lite.resc:
 *
 *   FLASH  0x08000000  2 MiB   code, rodata, the .data initialiser image
 *   SRAM   0x20000000  512 KiB stack, .data, .bss
 *   SDRAM  0x60000000  4 MiB   the newlib heap (._user_heap_stack)
 *
 * WHY THE HEAP IS IN SDRAM, and why that is a real constraint and not a
 * trick: hml_reasm_open(&r, 0) takes the library default frame size of
 * HML_MAX_FRAME_SIZE, which is 1 MiB, and allocates TWO buffers of that size
 * (staging + index-order gather). That is 2 MiB of heap, which does not fit
 * in the internal SRAM of any small Cortex-M. A part with external SDRAM
 * behind an FMC (STM32H7, NXP RT-series) can hold it, so the heap lives there
 * in this image. A part without SDRAM must be built with
 * --frame-bytes N, which passes an explicit smaller cap to hml_reasm_open();
 * the same code paths then run and the smoke test still means the same thing.
 * The demo prints the number it actually asked for on the UART, so a real
 * bring-up cannot miss it.
 *
 * Nothing in the library is changed to make this work: the script only decides
 * where the heap lives. On real silicon the SDRAM window must be initialised
 * (FMC configuration, refresh timer, pin mux, and a cache/MPU policy for that
 * region) before any allocation happens; this image has no SDRAM controller to
 * initialise because there is no SDRAM controller in the emulated platform
 * either.
 */

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 2M
  SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
  SDRAM (rwx) : ORIGIN = 0x60000000, LENGTH = 4M
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

  /* Stack at the top of internal SRAM, heap in SDRAM. See the header comment. */
  .stack (NOLOAD) : ALIGN(8)
  {
    . = ALIGN(8);
    __stack_bottom__ = .;
    . = . + 8K;
    . = ALIGN(8);
    __stack_top__ = .;
  } > SRAM

  .heap (NOLOAD) : ALIGN(8)
  {
    __heap_start__ = .;
    /* >= 2 * HML_MAX_FRAME_SIZE, the default cap, PLUS the reassembler's own
     * context (sizeof(hml_reasm_ctx) = 9488 bytes on this target, rounded up to
     * 0x2800 with room for the newlib chunk headers of the three allocations).
     * 2M alone was short by ~9.5 KiB and hml_reasm_open() failed on the last
     * chunk; see renode/boards/index and the vendor board_config.h files. */
    . = __heap_start__ + 2M + 0x2800;
    . = ALIGN(8);
    __heap_end__ = .;
  } > SDRAM

  /DISCARD/ :
  {
    *(.note.GNU-stack)
    *(.comment)
  }
}

ASSERT(__heap_end__ <= ORIGIN(SDRAM) + LENGTH(SDRAM),
       "the demo heap does not fit in the emulated SDRAM window")
