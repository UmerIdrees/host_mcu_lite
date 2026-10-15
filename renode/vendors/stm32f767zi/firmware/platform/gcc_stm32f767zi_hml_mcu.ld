/*
 * gcc_stm32f767zi_hml_mcu.ld -- linker script for the stm32f767zi vendor
 * platform (STM32F767ZI Nucleo, Cortex-M7).
 *
 * It matches platform/board_config.h, renode/vendors/stm32f767zi/stm32f767zi.resc
 * and the stm32f767zi row of renode/boards/index. renode/scripts/selfcheck_vendors.sh
 * parses all four and fails if any number differs, so the emulated map and the
 * linked map cannot drift apart.
 *
 *   FLASH  0x08000000  2 MiB   code, rodata, the .data initialiser image
 *   SRAM   0x20000000  512 KiB .data, .bss, stack, and the newlib heap
 *
 * WHY THE SRAM WINDOW IS 512 KiB AND NOT Renode's 0x10000000
 *   platforms/cpus/stm32f777.repl declares sram at 0x20000000 with size
 *   0x10000000, i.e. 256 MiB. That is a Renode placeholder, not a property of
 *   the part. Modelling a window 512x larger than the real silicon would let
 *   the image quietly depend on memory no STM32F767ZI has, which is the same
 *   mistake the stm32f4 row's header warns about in the other direction. So
 *   this script deliberately claims only 512 KiB: a subset of what Renode
 *   provides, and close to the real part's total SRAM. Anything the image needs
 *   beyond that has to be either smaller or absent, and the ASSERTs at the
 *   bottom turn a violation into a link failure instead of a silent overrun.
 *
 * WHY THE STACK IS 16 KiB HERE AND 8 KiB ON stm32f4
 *   This image additionally links lwIP, whose pbuf pools and the TCP/IP input
 *   path are stack-hungry; 8 KiB is the codec-only budget and is not enough
 *   headroom once a netif is in the call chain. This is headroom, not a
 *   measurement: no stack watermark is computed.
 *
 * THE HEAP INVARIANT
 *   138K = 2 * HML_VENDOR_FRAME_BYTES(65536) + HML_VENDOR_REASM_CTX_BYTES(0x2800).
 *   selfcheck_vendors.sh parses the literal below and fails the
 *   stm32f767zi_renode_selfcheck CTest test if it is not exactly that, so the
 *   two hml_reasm_open() buffers plus the reassembler's own context can never
 *   be quietly under-allocated.
 */

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 2M
  SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
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
    __exidx_start__ = .;
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    __exidx_end__ = .;
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

  /* The heap. 138K = 2 * FRAME_BYTES(64K) + REASM_CTX_BYTES(0x2800): the two
   * hml_reasm_open() buffers plus the reassembler's own context, and on this
   * row also lwIP's pbuf pools, which malloc from the same region. The literal
   * below is parsed by selfcheck_vendors.sh. */
  .heap (NOLOAD) : ALIGN(8)
  {
    __heap_start__ = .;
    . = __heap_start__ + 138K;
    . = ALIGN(8);
    __heap_end__ = .;
  } > SRAM

  /* Stack at the top of internal SRAM. 16 KiB, see the header note. It MUST
   * come after .heap in this list: the ASSERT below requires the stack to sit
   * above the heap, and a .stack emitted first would put __stack_top__ under
   * __heap_end__ and fail the link even when the two do fit. */
  .stack (NOLOAD) : ALIGN(8)
  {
    . = ALIGN(8);
    __stack_bottom__ = .;
    . = . + 16K;
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
       "the demo heap does not fit in this part's 512 KiB of SRAM")
ASSERT(__stack_top__ > __heap_end__,
       "the stack and the heap overlap in SRAM")
ASSERT(__stack_top__ <= ORIGIN(SRAM) + LENGTH(SRAM),
       "the stack does not fit at the top of this part's 512 KiB of SRAM")
