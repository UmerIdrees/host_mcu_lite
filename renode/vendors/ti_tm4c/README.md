# Vendor: `ti_tm4c` -- TM4C1294-style Cortex-M4F, flash at address 0

**Status: this platform has never been executed.** It was written on a machine
with no Renode, no `mono`, no `dotnet` and no `arm-none-eabi-gcc`, and no
network access to any of them. The `.resc`, the `.robot`, the linker script and
the startup file in this directory have never been loaded by a Renode release,
never been fed to a linker and never been compiled. The CTest test
`ti_tm4c_renode_smoke` reports ***Skipped*** on such a machine, and that is the
correct outcome, not a pass. See [`../../../docs/renode_multi_vendor.md`](../../../docs/renode_multi_vendor.md).

## What is here

| File | Role |
| --- | --- |
| `ti_tm4c.resc` | the emulated machine: flash, SRAM, UART0, NVIC, LoadELF |
| `ti_tm4c.robot` | the assertions, run by `renode-test` |
| `firmware/vendor.env` | the build script's per-vendor inputs |
| `firmware/platform/board_config.h` | the same facts as C constants |
| `firmware/platform/gcc_ti_tm4c_hml_mcu.ld` | the linker script for this part |
| `firmware/platform/startup_ti_tm4c.c` | reset vector, `.data`/`.bss`, `_sbrk`, the console UART, the demo task |

## What is shared, and what is genuinely vendor-specific

**Shared, compiled unchanged** (from `renode/firmware/freertos_demo/` and `src/`):

* `app/hml_mcu_demo.c` -- the entire check body. **Not copied here.** This is the
  only thing that decides whether a run passes.
* `stub_freertos/FreeRTOS.h` and `stub_freertos/freertos_stub.c` -- the
  cooperative **NOT-FreeRTOS** task API, ticked by the ARMv7-M SysTick
  `COUNTFLAG`. Reused as-is.
* the library's own portable C.

**Vendor-specific** -- four files, each with a stated reason:

1. **`gcc_ti_tm4c_hml_mcu.ld`** -- and this is the row that is mapped at address
   **0**, unlike every other board in the matrix.
2. **`board_config.h`** -- the numbers, so the firmware and the linker script
   cannot disagree at compile time.
3. **`startup_ti_tm4c.c`** -- reset and `_sbrk` are per-linker-script; the
   console is UART0 at `0x4000C000`, written through the PL011 register block.
4. **`--frame-bytes 65536`** -- see below.

## Flash at address 0, and why that is worth spelling out

The TM4C1294 boots from `0x00000000`, so the reset vector table is at address 0
and `ti_tm4c.resc` writes `cpu VectorTableOffset 0x00000000` out in full. On the
other rows a wrong vector offset lands the image somewhere obviously empty; on
this row it would land in *code*, which is the quietest possible failure. The
self-check asserts that the literal in the `.resc` equals the `FLASH ORIGIN` in
the linker script, so the two cannot drift.

## The FPU: declared, present, and deliberately unused

`cpuType` is `cortex-m4f` because the part has an FPU and the platform model may
reasonably have one. The firmware is nevertheless compiled **`-mfloat-abi=soft`**
and contains no FP instruction. A demo that issued FP instructions would need
CPACR `CP10`/`CP11` set and lazy stacking configured, which is real bring-up work
with no payoff here: nothing in this harness needs floating point.

Do **not** "fix" the mismatch by adding `-mfpu=fpv4-sp-d16` on its own. Add the
CPACR enable to `startup_ti_tm4c.c` in the same change, or the image will take a
`UsageFault` on its first FP instruction. The robot test does not assert on this,
so the failure would look like a hang and be reported as a **FAILURE**.

## About the UART model name

The TM4C UART is a TI Stellaris PL411. No TM4C UART model that this harness has
ever seen exists to it, and Renode was not installed anywhere this matrix was
written, so `ti_tm4c.resc` asks for `UART.PL011` -- whose register block (data at
`+0x00`, flags at `+0x18`, `TXFF` as bit 5) is the closest match available -- and
`startup_ti_tm4c.c` writes that block. The two halves agree with each other,
which is what lets the image print at all. Neither has been checked against a data
sheet or a Renode release. If a real TM4C model turns up, change both halves.

## The frame cap, and why it is 65536 here

`hml_reasm_open(&r, 0)` takes the library default of `HML_MAX_FRAME_SIZE` = 1 MiB
and allocates **two** buffers of that size: **2 MiB of heap**. The base platform
can afford that because its linker script puts the heap in a modelled SDRAM
window. This board models no SDRAM. 256 KiB of SRAM holds an 8 KiB stack, roughly
5 KiB of `.bss` and the two reassembly buffers, so the cap is 64 KiB and the heap
138 KiB (the two buffers plus the reassembler context). Same code paths, same checks, same meaning.

With `--frame-bytes 0` you get the library default, the allocation fails, and the
image prints `HML-MCU-DEMO-WARN` then `HML-MCU-DEMO-FAIL` -- which **fails** the
robot test.

## The stub / real boundary

| Thing | Status |
| --- | --- |
| `app/hml_mcu_demo.c` and the library C | **REAL**, and the only thing under test |
| reset / `.data` / `.bss` / `_sbrk` in `startup_ti_tm4c.c` | **REAL** code, per this linker script |
| `clock_init()` and `uart_init()` | **DOCUMENTED NO-OPS.** Renode models neither the run-mode clock registers nor a baud divisor. On silicon they are where the clock configuration, the flash wait states, the pin mux and `UARTCR1`/`LCRH`/`IBRD`/`FBRD` belong. |
| The FPU | **PRESENT AND UNUSED.** No `CPACR` enable, no FP instruction. |
| `stub_freertos/` | **STUB. NOT FreeRTOS.** Never compiled against the kernel. |
| lwIP | **ABSENT.** Not vendored, not stubbed, not linked. |
| The UART interrupt line `5` | **DECORATION.** The demo takes no interrupt. Recorded in `renode/boards/index` and cross-checked so it cannot rot; no run would notice a wrong value. |
| The UART register block | **THE PL011 ONE, CHOSEN NOT VERIFIED.** See above. |
| The emulated machine | **NOT SILICON.** A green run would be evidence about this C code and about Renode, never about a TI part. |

## Two inherited strings that are wrong here, on purpose

The shared demo body prints `image=hml_mcu_demo.elf` and calls the heap "in SDRAM
by linker script". Both are base-platform text and both are **false here**.
Editing them would mean forking the shared body, so `startup_ti_tm4c.c` corrects
them from the outside instead: `print_board_identity()` runs first and prints the
real artefact name, the real heap size and the real memory map, plus a line saying
the image is soft float and never touches the FPU. Where the two disagree, the
vendor line is right, and `ti_tm4c.robot` asserts it.

## Running it, when the tooling exists

```sh
renode/scripts/build_vendor.sh ti_tm4c
renode/scripts/run_vendor_smoke.sh ti_tm4c

# the machine-check that needs no emulator at all, and passes today
renode/scripts/selfcheck_vendors.sh --vendor ti_tm4c
ctest --test-dir build -R ti_tm4c --output-on-failure
```
