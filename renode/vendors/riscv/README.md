# Vendor: `riscv` -- GD32VF103-style RV32IMAC, 128 KiB flash at address 0

**Status: this platform has never been executed.** It was written on a machine
with no Renode, no `mono`, no `dotnet`, no `riscv32` toolchain and no network
access to any of them. The `.resc`, the `.robot`, the linker script and the
startup file in this directory have never been loaded by a Renode release,
never been fed to a linker and never been compiled. The CTest test
`riscv_renode_smoke` reports ***Skipped*** on such a machine, and that is the
correct outcome, not a pass. See
[`../../../docs/renode_multi_vendor.md`](../../../docs/renode_multi_vendor.md).

## What is here

| File | Role |
| --- | --- |
| `riscv.resc` | the emulated machine: flash, SRAM, UART0, CLINT, LoadELF |
| `riscv.robot` | the assertions, run by `renode-test` |
| `firmware/vendor.env` | the build script's per-vendor inputs |
| `firmware/platform/board_config.h` | the same facts as C constants |
| `firmware/platform/gcc_riscv_hml_mcu.ld` | the linker script for this part |
| `firmware/platform/startup_riscv.c` | reset, `.data`/`.bss`, `_sbrk`, the console UART, the **tick**, the demo task |

## What is shared, and what is genuinely vendor-specific

**Shared, compiled unchanged** (from `renode/firmware/freertos_demo/` and `src/`):

* `app/hml_mcu_demo.c` -- the entire check body. **Not copied here.**
* `stub_freertos/FreeRTOS.h` -- the NOT-FreeRTOS API *declarations*.
* the library's own portable C.

**NOT shared -- the stub implementation itself.** This is the one row where
`HML_VENDOR_SHARED_STUB=0` and the shared `freertos_stub.c` **is not compiled**:
its tick is the ARMv7-M SysTick `COUNTFLAG`, and RISC-V has no SysTick.
`startup_riscv.c` therefore re-implements the API surface the image actually
uses -- `xTaskCreate`, `vTaskStartScheduler`, `vTaskDelay`, `taskYIELD`,
`xTaskGetTickCount`, `hml_stub_elapsed_ms`, `hml_stub_systick_stalled` --
ticked by the **MMIO machine timer (CLINT `mtime`)** at `0x02000000` + `0xBFF8`,
with the same bounded-spin fallback. The queue half of the header is declared
but called by nothing in this image, so it is deliberately not implemented here.
Everything in that paragraph is in `startup_riscv.c`, and the file says so on
its first page.

**Vendor-specific, like the other four rows:**

1. **`gcc_riscv_hml_mcu.ld`** -- `ENTRY(Reset_Handler)`, `.text.init` pinned to
   address 0, a `.riscv_trap_vector`, no `.sdata`/`.sbss` (compiled
   `-msdata=none`).
2. **`board_config.h`** -- the numbers: 128 KiB flash at 0, 32 KiB SRAM at
   `0x20000000`, UART at `0x40000000`.
3. **`startup_riscv.c`** -- the biggest vendor file in the matrix, because it
   owns the tick as well as the reset path.
4. **`--frame-bytes 4096`** -- the smallest cap in the matrix, on the smallest
   part. See below.

## Reset is firmware, not a vector table

A Cortex-M finds its entry point in a vector table. A RISC-V core has none: it
fetches from the reset address (Renode's default lands it at the start of flash,
which is address 0 here and matches `FLASH ORIGIN`), and `mtvec` is a **CSR
programmed by the image**. `Reset_Handler` does exactly that before anything
else and then enters a tiny C start-up that copies `.data`, zeroes `.bss` and
calls `main()`. A trap that somehow does fire lands in `riscv_mtvec_entry()`,
which spins on purpose: a fixed-address dead hang is a bug you can point at,
whereas executing garbage is a bug you chase.

## The tick: mtime edges, tick resolution only

The shared stub's `vTaskDelay` waits for SysTick `COUNTFLAG` edges and it is
honest that "ms" means "tick edges, not wall time". This row mirrors that:
`wait_ticks()` counts forward motion of the 64-bit machine timer, one edge per
tick, from the low word at `CLINT + 0xBFF8` and the high word at `CLINT + 0xBFFC`
(read with a break-torn-read retry). If a model never clocks `mtime`, a bounded
spin budget kicks in, the pause falls back, and the image prints
`HML-MCU-DEMO-WARN` -- visibly, and never as a hang. No emulated-time figure may
ever be quoted as anything finer than tick resolution.

## About the CPU model name and the UART model name

Both are **chosen, not verified**, and each is flagged in the file that uses it:

* `CPU.RiscV32I` + `cpuType "rv32imac"` in `riscv.resc` is transcribed from
  memory; a Renode release that spells the core differently must be reconciled
  in that one file, and the comment saying so stays.
* `UART.NS16550` with THR at `+0x00` and LSR at `+0x05` is the same stand-in as
  the `microchip_sam` and `nxp_lpc` rows, and it is emphatically **not** the
  GD32VF103 USART map. `startup_riscv.c` and `riscv.resc` agree with each other,
  which is what lets the image print; both halves must change together.
* The CLINT's offsets match Renode's `RiscVClint` conventional layout, and the
  same "unverified, reconcile in one place" comment protects them. `0` is wired
  nowhere as an interrupt: there is no PLIC and no NVIC in this platform, and
  the `uart-irq` of `0` the image prints is fictional on purpose.

## The frame cap, and why it is 4096 -- the smallest in the matrix

`hml_reasm_open(&r, 0)` takes the library default of `HML_MAX_FRAME_SIZE` = 1 MiB
and allocates **two** buffers of that size: **2 MiB of heap**. The base platform
can afford that because its linker script puts the heap in a modelled SDRAM
window. This part has **32 KiB of SRAM total**, and the heap is not just the two
buffers: `hml_reasm_open()` also allocates its own reassembler context
(`sizeof(hml_reasm_ctx)` = 9488 bytes on this 32-bit target). So the heap here is
4 KiB stack, roughly 5 KiB of `.bss`, then **18 KiB of heap** = two 4096-byte
reassembly buffers **plus that context**. Same code paths, same checks, same
meaning. A `--frame-bytes 0` here fails the allocation and fails the robot test.
The demo reassembles frames of at most 200 bytes, so a 4096-byte cap is ample.

## The stub / real boundary

| Thing | Status |
| --- | --- |
| `app/hml_mcu_demo.c` and the library C | **REAL**, and the only thing under test |
| reset / `.data` / `.bss` / `_sbrk` in `startup_riscv.c` | **REAL** code, per this linker script |
| the task API and the tick | **SHIM**, written in `startup_riscv.c`; NOT FreeRTOS, NOT SysTick, tick resolution only |
| `clock_init()` and `uart_init()` | **DOCUMENTED NO-OPS.** Renode models neither the RCU nor a baud divisor. On silicon they are where the PLL config, the flash wait states, the mux and the USART config belong. |
| `stub_freertos/freertos_stub.c` | **NOT LINKED ON THIS ROW.** ARM-only SysTick tick; `startup_riscv.c` replaces it. |
| lwIP | **ABSENT.** Not vendored, not stubbed, not linked. |
| interrupts | **ABSENT.** No NVIC, no PLIC, no interrupt is ever taken. |
| The UART register block | **THE NS16550 ONE, CHOSEN NOT VERIFIED.** See above. |
| The CPU model / CLINT layout / mtime offsets | **CHOSEN FROM MEMORY, FLAGGED UNVERIFIED.** See above. |
| The emulated machine | **NOT SILICON.** A green run would be evidence about this C code and about Renode, never about a GigaDevice part. |

## Running it, when the tooling exists

```sh
renode/scripts/build_vendor.sh riscv
renode/scripts/run_vendor_smoke.sh riscv

# the machine-check that needs no emulator at all, and passes today
renode/scripts/selfcheck_vendors.sh --vendor riscv
ctest --test-dir build -R riscv --output-on-failure
```