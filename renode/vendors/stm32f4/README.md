# Vendor: `stm32f4` -- STM32F407-style Cortex-M4

**Status: this platform has never been executed.** It was written on a machine
with no Renode, no `mono`, no `dotnet` and no `arm-none-eabi-gcc`, and no
network access to any of them. The `.resc`, the `.robot`, the linker script and
the startup file in this directory have never been loaded by a Renode release,
never been fed to a linker and never been compiled. The CTest test
`stm32f4_renode_smoke` reports ***Skipped*** on such a machine, and that is the
correct outcome, not a pass. See [`../../../docs/renode_multi_vendor.md`](../../../docs/renode_multi_vendor.md).

## What is here

| File | Role |
| --- | --- |
| `stm32f4.resc` | the emulated machine: flash, SRAM, USART2, NVIC, LoadELF |
| `stm32f4.robot` | the assertions, run by `renode-test` |
| `firmware/vendor.env` | the build script's per-vendor inputs (ISA flags, linker script, frame cap, cross prefix) |
| `firmware/platform/board_config.h` | the same facts as C constants, compiled into the image |
| `firmware/platform/gcc_stm32f4_hml_mcu.ld` | the linker script for this part |
| `firmware/platform/startup_stm32f4.c` | reset vector, `.data`/`.bss`, `_sbrk`, the console UART, the demo task |

## What is shared, and what is genuinely vendor-specific

**Shared, compiled unchanged** (from `renode/firmware/freertos_demo/`, and from
`src/`):

* `app/hml_mcu_demo.c` -- the entire check body: CRC vectors, the beacon codec,
  the control codec, the `hml_dest_cfg` codec, the frame codec and the whole
  reassembly suite. **Not copied into this directory.** This is the only thing
  that decides whether a run passes.
* `stub_freertos/FreeRTOS.h` and `stub_freertos/freertos_stub.c` -- the
  cooperative **NOT-FreeRTOS** task API, whose tick is the ARMv7-M SysTick
  `COUNTFLAG`. Reused as-is because that is real hardware on this part and Renode
  models it.
* the library's own portable C.

**Vendor-specific** -- exactly four files, and each exists for a stated reason:

1. **`gcc_stm32f4_hml_mcu.ld`**: the regions differ. 1 MiB flash at `0x08000000`
   and 128 KiB SRAM at `0x20000000` for this part, against 2 MiB + 512 KiB + a
   4 MiB SDRAM window in the base platform.
2. **`board_config.h`**: the numbers, so the firmware and the linker script
   cannot disagree at compile time.
3. **`startup_stm32f4.c`**: the reset vector and `_sbrk` are per-linker-script,
   and the console is `USART2` at `0x40004400` with `SR` at `+0x00`, `DR` at
   `+0x04` and `TXE` as `SR` bit 7. Those offsets are this part's, not generic.
4. **`--frame-bytes 32768`**: not a style choice. See below.

## The frame cap, and why it is 32768 here

`hml_reasm_open(&r, 0)` takes the library default of `HML_MAX_FRAME_SIZE` = 1 MiB
and allocates **two** buffers of that size -- staging plus the index-order
gather buffer. That is **2 MiB of heap**. The base platform can afford it
because its linker script puts the heap in a modelled SDRAM window. **An STM32F407
has no external SDRAM at all.** Its 128 KiB of SRAM holds the stack (8 KiB), the
`.bss` (roughly 5 KiB) and the two reassembly buffers, so this board is built
with a 32 KiB frame cap and a 74 KiB heap (the two buffers plus the reassembler
context `hml_reasm_open()` allocates alongside them). The code paths, the checks
and the meaning of the smoke test are unchanged; only the largest accepted frame
is.

If you build this board with `--frame-bytes 0` you get the library default, the
allocation fails, and the image prints `HML-MCU-DEMO-WARN` followed by
`HML-MCU-DEMO-FAIL` -- which **fails** the robot test. That is the intended
behaviour, not a bug to paper over.

## The stub / real boundary

| Thing | Status |
| --- | --- |
| `app/hml_mcu_demo.c` and the library C | **REAL**, and the only thing under test |
| `startup_stm32f4.c` reset / `.data` / `.bss` / `_sbrk` | **REAL** code, per this linker script |
| `clock_init()` and `uart_init()` | **DOCUMENTED NO-OPS.** Renode models neither this part's RCC nor a baud divisor. On real silicon this is where the clock tree, the PCLK enable, the flash wait states and the PA2/PA3 AF7 mux belong. |
| `stub_freertos/` | **STUB. NOT FreeRTOS.** Not preemptive, no priorities, no context switch, no tick interrupt, never compiled against the kernel. |
| lwIP | **ABSENT.** Not vendored, not stubbed, not linked. |
| The interrupt line `USART2_IRQn 37` | **DECORATION.** The demo enables no interrupt and handles no exception. It is recorded in `renode/boards/index` and cross-checked so it cannot rot; nothing in a run would notice if it were wrong. |
| The USART2 register offsets | **FROM THE REFERENCE MANUAL, UNVERIFIED.** They match what `UART.STM32_UART` models, but neither has been checked against a Renode release. This is the single most likely thing to need fixing on a real bring-up, and the failure will be a timeout and therefore a **FAILURE**, never a skip. |
| The emulated machine | **NOT SILICON.** A green run would be evidence about this C code and about Renode's fidelity, never about an STM32. |

## Two inherited strings that are wrong here, on purpose

The shared demo body prints `image=hml_mcu_demo.elf` and describes the heap as
being "in SDRAM by linker script". Both are base-platform text and both are
**false on this board**. Editing them would mean forking the shared body, which
is the one thing sharing it exists to prevent, so `startup_stm32f4.c` corrects
them from the outside instead: `print_board_identity()` runs first and prints the
real artefact name, the real heap size and the real memory map. Where the two
disagree, the vendor line is right. `stm32f4.robot` asserts the vendor line, so a
stale correction cannot pass unnoticed.

## Running it, when the tooling exists

```sh
renode/scripts/build_vendor.sh stm32f4
renode/scripts/run_vendor_smoke.sh stm32f4

# with explicit paths and a different cap
HML_CROSS_CC=/opt/gcc-arm/bin/arm-none-eabi-gcc \
  renode/scripts/run_vendor_smoke.sh stm32f4 --frame-bytes 16384 --timeout 900

# the machine-check that needs no emulator at all, and passes today
renode/scripts/selfcheck_vendors.sh --vendor stm32f4
ctest --test-dir build -R stm32f4 --output-on-failure
```
