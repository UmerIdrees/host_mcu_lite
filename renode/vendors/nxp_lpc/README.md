# Vendor: `nxp_lpc` -- LPC4337-style Cortex-M4, executed in place

**Status: this platform has never been executed.** It was written on a machine
with no Renode, no `mono`, no `dotnet` and no `arm-none-eabi-gcc`, and no
network access to any of them. The `.resc`, the `.robot`, the linker script and
the startup file in this directory have never been loaded by a Renode release,
never been fed to a linker and never been compiled. The CTest test
`nxp_lpc_renode_smoke` reports ***Skipped*** on such a machine, and that is the
correct outcome, not a pass. See [`../../../docs/renode_multi_vendor.md`](../../../docs/renode_multi_vendor.md).

## What is here

| File | Role |
| --- | --- |
| `nxp_lpc.resc` | the emulated machine: XIP flash, SRAM, USART0, NVIC, LoadELF |
| `nxp_lpc.robot` | the assertions, run by `renode-test` |
| `firmware/vendor.env` | the build script's per-vendor inputs |
| `firmware/platform/board_config.h` | the same facts as C constants |
| `firmware/platform/gcc_nxp_lpc_hml_mcu.ld` | the linker script for this part |
| `firmware/platform/startup_nxp_lpc.c` | reset vector, `.data`/`.bss`, `_sbrk`, the console UART, the demo task |

## What is shared, and what is genuinely vendor-specific

**Shared, compiled unchanged** (from `renode/firmware/freertos_demo/` and
`src/`):

* `app/hml_mcu_demo.c` -- the entire check body. **Not copied here.** This is the
  only thing that decides whether a run passes.
* `stub_freertos/FreeRTOS.h` and `stub_freertos/freertos_stub.c` -- the
  cooperative **NOT-FreeRTOS** task API, ticked by the ARMv7-M SysTick
  `COUNTFLAG`. Reused as-is because that is real hardware on this part and Renode
  models it.
* the library's own portable C.

**Vendor-specific** -- four files, each with a stated reason:

1. **`gcc_nxp_lpc_hml_mcu.ld`** -- and this is the row where the flash address
   genuinely differs from every other row in the matrix. The LPC43xx executes
   **in place** out of its serial flash at `0x1A000000`: no boot copy, no
   separate load region, the vector table at `0x1A000000`.
2. **`board_config.h`** -- the numbers, so the firmware and the linker script
   cannot disagree at compile time.
3. **`startup_nxp_lpc.c`** -- reset and `_sbrk` are per-linker-script; the
   console is USART0 at `0x400C1000`. See the caveat below about its register
   block.
4. **`--frame-bytes 32768`** -- see below.

## The frame cap, and why it is 32768 here

`hml_reasm_open(&r, 0)` takes the library default of `HML_MAX_FRAME_SIZE` = 1 MiB
and allocates **two** buffers of that size: **2 MiB of heap**. The base platform
can afford that because its linker script puts the heap in a modelled SDRAM
window. **This platform models no SDRAM for this part.** 136 KiB of SRAM holds an
8 KiB stack, roughly 5 KiB of `.bss` and the two reassembly buffers, so the cap
is 32 KiB and the heap 74 KiB (the two buffers plus the reassembler context). Same code paths, same checks, same meaning.

With `--frame-bytes 0` you get the library default, the allocation fails, and the
image prints `HML-MCU-DEMO-WARN` then `HML-MCU-DEMO-FAIL` -- which **fails** the
robot test. That is the intended behaviour on a part too small for the default.

## THE UART: the base address is the part's, the register block is not

There is no LPC43xx USART model that this harness has ever seen, and Renode was
not installed anywhere this matrix was written. So `nxp_lpc.resc` asks Renode for
`UART.NS16550` at the part's **real** USART0 base (`0x400C1000`), and
`startup_nxp_lpc.c` writes the **NS16550** register block to match
(`THR +0x00`, `LSR +0x05`, `THRE` = bit 5).

Those two halves agree with each other, which is what lets the image print
anything at all. They do **not** agree with the LPC43xx USART register map from
UM10539. This is stated in both files, and it is the first thing to fix on a real
bring-up: either find or write a real LPC USART model, or change **both** halves
to the same register block you actually want. Changing only one makes the
platform's only possible outcome a timeout.

## The SRAM window is a stated simplification

136 KiB at `0x10000000`, as **one contiguous window**. The real LPC4337 splits its
SRAM across banks (UM10539). A linker script needs a contiguous region and the
demo's heap has to live in one place, so this models the aggregate rather than
claiming to be the part's real bank map. It is called out in `board_config.h`,
in the linker script and in `nxp_lpc.resc`.

## The stub / real boundary

| Thing | Status |
| --- | --- |
| `app/hml_mcu_demo.c` and the library C | **REAL**, and the only thing under test |
| reset / `.data` / `.bss` / `_sbrk` in `startup_nxp_lpc.c` | **REAL** code, per this linker script |
| `clock_init()` and `uart_init()` | **DOCUMENTED NO-OPS.** Renode models neither this part's clock-control block nor the SPI/QSPI flash controller this image runs out of. On silicon, that function is where the PLL/AHB setup, the USART0 clock divider, the pin mux and the flash controller belong. |
| `stub_freertos/` | **STUB. NOT FreeRTOS.** Never compiled against the kernel. |
| lwIP | **ABSENT.** Not vendored, not stubbed, not linked. |
| The UART register block | **THE EMULATED MODEL'S, NOT THE LPC ONE.** See above. |
| The USART0 IRQ line `58` | **DECORATION, AND THE LEAST CONFIDENT NUMBER IN THIS MATRIX.** The demo takes no interrupt. It is pinned in `renode/boards/index` and cross-checked so it cannot rot, but no run would notice a wrong value. |
| The emulated machine | **NOT SILICON.** A green run would be evidence about this C code and about Renode, never about an NXP part. |

## Two inherited strings that are wrong here, on purpose

The shared demo body prints `image=hml_mcu_demo.elf` and calls the heap "in SDRAM
by linker script". Both are base-platform text and both are **false here**.
Editing them would mean forking the shared body, so `startup_nxp_lpc.c` corrects
them from the outside instead: `print_board_identity()` runs first and prints the
real artefact name, the real heap size and the real memory map. Where the two
disagree, the vendor line is right, and `nxp_lpc.robot` asserts it.

## Running it, when the tooling exists

```sh
renode/scripts/build_vendor.sh nxp_lpc
renode/scripts/run_vendor_smoke.sh nxp_lpc

# the machine-check that needs no emulator at all, and passes today
renode/scripts/selfcheck_vendors.sh --vendor nxp_lpc
ctest --test-dir build -R nxp_lpc --output-on-failure
```
