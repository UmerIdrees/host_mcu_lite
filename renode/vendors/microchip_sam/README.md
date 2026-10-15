# Vendor: `microchip_sam` -- SAME70-style Cortex-M7, 2 MiB flash at 0x00400000

**Status: this platform has never been executed.** It was written on a machine
with no Renode, no `mono`, no `dotnet` and no `arm-none-eabi-gcc`, and no
network access to any of them. The `.resc`, the `.robot`, the linker script and
the startup file in this directory have never been loaded by a Renode release,
never been fed to a linker and never been compiled. The CTest test
`microchip_sam_renode_smoke` reports ***Skipped*** on such a machine, and that
is the correct outcome, not a pass. See
[`../../../docs/renode_multi_vendor.md`](../../../docs/renode_multi_vendor.md).

## What is here

| File | Role |
| --- | --- |
| `microchip_sam.resc` | the emulated machine: flash, SRAM, UART0, NVIC, LoadELF |
| `microchip_sam.robot` | the assertions, run by `renode-test` |
| `firmware/vendor.env` | the build script's per-vendor inputs |
| `firmware/platform/board_config.h` | the same facts as C constants |
| `firmware/platform/gcc_microchip_sam_hml_mcu.ld` | the linker script for this part |
| `firmware/platform/startup_microchip_sam.c` | reset vector, `.data`/`.bss`, `_sbrk`, the console UART, the demo task |

## What is shared, and what is genuinely vendor-specific

**Shared, compiled unchanged** (from `renode/firmware/freertos_demo/` and `src/`):

* `app/hml_mcu_demo.c` -- the entire check body. **Not copied here.** This is the
  only thing that decides whether a run passes.
* `stub_freertos/FreeRTOS.h` and `stub_freertos/freertos_stub.c` -- the
  cooperative **NOT-FreeRTOS** task API, ticked by the ARMv7-M SysTick
  `COUNTFLAG`. A Cortex-M7 has an ARMv7-M SysTick, so this is reused as-is.
* the library's own portable C.

**Vendor-specific** -- four files, each with a stated reason:

1. **`gcc_microchip_sam_hml_mcu.ld`** -- the flash and SRAM origins this part
   actually uses: **not** `0x08000000` and **not** `0x20000000`.
2. **`board_config.h`** -- the numbers, so the firmware and the linker script
   cannot disagree at compile time.
3. **`startup_microchip_sam.c`** -- reset and `_sbrk` are per-linker-script; the
   console is UART0 at `0x400E0800`, written through the emulated model's
   register block (see the UART note).
4. **`--frame-bytes 131072`** -- see below.

## Neither address is a canonical ARM one

Flash is at `0x00400000` (a bootROM-and-bias-mapping consequence of the part),
not at `0x08000000`; SRAM is at `0x20400000`, not `0x20000000`. Both are written
out in full everywhere on this board exclusively so a copy-paste from a more
familiar STM32-style map cannot silently link an image the emulated machine has
no memory for. The self-check asserts the `.resc`'s `VectorTableOffset` is
`0x00400000` and equals the linker script's `FLASH ORIGIN`, so the guard is
mechanical, not notional.

## The M7: cache, MPU, FPU -- all four deliberately off

The platform model declares `cortex-m7` and the part really does have a
double-precision FPU, an I-cache, a D-cache and an MPU. The **image** runs with
every one of them disabled, and `startup_microchip_sam.c` says so three times
(at the top, in `clock_init()`'s comment and in an identity line). Each is a
decision:

* **Cache-off** is *safe* here only because `hml_reasm`'s two buffers are
  ordinary `malloc`'d memory accessed by name -- there is nothing to
  maintain. Against a DMA engine it would not be safe, and turning the D-cache
  on without a coherency policy for those buffers would be a bug, not a feature.
* **MPU-off** means no region protection and no stack-overflow trap. A future
  `-fstack-protector` stack guard would only trip the MPU if one is configured.
* **FPU-off** (`-mfloat-abi=soft`) means no `CPACR` write and no FP instruction:
  enabling the FPU would buy nothing this harness uses.

Do not "fix" the platform by enabling a cache inside `microchip_sam.resc` --
that would change the observed behaviour of an image that was never written with
cache maintenance in mind, and the run would pass on a machine that behaves
differently from the one the claim is about.

## About the UART model name

No SAME70 UART model that this harness has ever seen exists to it, and Renode is
not installed anywhere this matrix was written, so this row asks for
`UART.NS16550` -- the same stand-in decision as the `nxp_lpc` row -- and the
startup's constants (THR at `+0x00`, LSR at `+0x05`) were chosen to match. The
**base address is the SAME70's UART0**; the **register block is emphatically not**
the SAME70 USART register map. Both halves agree with each other, which is what
lets the image print at all. When real silicon or a real model appears, change
**both** halves or the only outcome is a timeout. The `35` interrupt line is a
guess from the Harmony+ table, is decoration (the demo takes no interrupt), and
is recorded in `renode/boards/index` so the guess cannot quietly rot.

## The frame cap, and why it is 131072 here

`hml_reasm_open(&r, 0)` takes the library default of `HML_MAX_FRAME_SIZE` = 1 MiB
and allocates **two** buffers of that size: **2 MiB of heap**. The base platform
can afford that because its linker script puts the heap in a modelled SDRAM
window. This board models only the part's internal 384 KiB of SRAM: an 8 KiB
stack, roughly 5 KiB of `.bss`, then **266 KiB of heap** = two 131072-byte
buffers. Same code paths, same checks, same meaning. This is the largest frame cap
in the matrix, on the largest part in the matrix.

With `--frame-bytes 0` you get the library default, the allocation fails, and the
image prints `HML-MCU-DEMO-WARN` then `HML-MCU-DEMO-FAIL` -- which **fails** the
robot test.

## The stub / real boundary

| Thing | Status |
| --- | --- |
| `app/hml_mcu_demo.c` and the library C | **REAL**, and the only thing under test |
| reset / `.data` / `.bss` / `_sbrk` in `startup_microchip_sam.c` | **REAL** code, per this linker script |
| `clock_init()` and `uart_init()` | **DOCUMENTED NO-OPS.** Renode models neither the PMC nor a baud divisor. On silicon they are where the PMC gating, the master clock selection, the flash wait states, the SERCOM mux and the UART configuration belong. |
| Cache / MPU / FPU | **PRESENT IN THE MODEL, DELIBERATELY UNUSED IN THE IMAGE.** |
| `stub_freertos/` | **STUB. NOT FreeRTOS.** Never compiled against the kernel. |
| lwIP | **ABSENT.** Not vendored, not stubbed, not linked. |
| The UART interrupt line `35` | **DECORATION AND A GUESS.** No data sheet was available to check it. Recorded in `renode/boards/index` and cross-checked so it cannot rot; no run would notice a wrong value. |
| The UART register block | **THE NS16550 ONE, CHOSEN NOT VERIFIED.** See above. |
| The emulated machine | **NOT SILICON.** A green run would be evidence about this C code and about Renode, never about a Microchip part. |

## Two inherited strings that are wrong here, on purpose

The shared demo body prints `image=hml_mcu_demo.elf` and calls the heap "in SDRAM
by linker script". Both are base-platform text and both are **false here**.
Editing them would mean forking the shared body, so `startup_microchip_sam.c`
corrects them from the outside instead: `print_board_identity()` runs first and
prints the real artefact name, the real heap size and the real memory map, plus
lines saying this part has no SDRAM, that the image is a bare M7, and that the
UART register block is the model's and not the part's. Where the two disagree,
the vendor line is right, and `microchip_sam.robot` asserts it.

## Running it, when the tooling exists

```sh
renode/scripts/build_vendor.sh microchip_sam
renode/scripts/run_vendor_smoke.sh microchip_sam

# the machine-check that needs no emulator at all, and passes today
renode/scripts/selfcheck_vendors.sh --vendor microchip_sam
ctest --test-dir build -R microchip_sam --output-on-failure
```