# Renode multi-vendor harness (`renode/vendors/`)

**Status: not one of these platforms has ever been executed.** This harness was
written on a machine with no Renode, no `mono`, no `dotnet`, no
`arm-none-eabi-gcc` and no `riscv32-unknown-elf-gcc`, and with no network access
to any of them. Every platform description, every Robot test, every linker
script and every startup file in `renode/vendors/` has never been loaded by a
Renode release, never been fed to a linker and never been compiled. On this
machine all five CTest smoke tests report **skipped**, which is the correct
outcome -- a skip is not a pass, and nothing below should be quoted as one.
Read section 8 before you report any of this as a result.

`sibling document: docs/renode.md` is the same honesty story for the original,
single-part (`cortex-m4`) harness. This document is the multi-vendor extension:
the same smoke idea, repeated over five boards in `renode/boards/index`, so the
library's codec/CRC/reassembly C is checked against a spread of emulated parts.

---

## 1. The five boards

`renode/boards/index` is a tab-separated manifest that is the **single source of
truth** for the harness. Every other file that states a number (vendor.env,
`board_config.h`, the linker script, the `.resc`, the `.robot`) is checked
against it by `renode/scripts/selfcheck_vendors.sh`, which is a CTest test that
always runs and always must pass. The rows:

| vendor | machine | arch | console UART | flash | SRAM | frame cap | linker / startup | cross prefix |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `stm32f4` | `stm32f4` | `cortex-m4` | USART2 `0x40004400` IRQ 37 | 1 MiB @ `0x08000000` | 128 KiB @ `0x20000000` | 32768 | `gcc_stm32f4_hml_mcu.ld` / `startup_stm32f4.c` | `arm-none-eabi-` |
| `nxp_lpc` | `nxp_lpc` | `cortex-m4` | UART0 `0x400C1000` IRQ 58 | 512 KiB @ `0x1A000000` | 136 KiB @ `0x10000000` | 32768 | `gcc_nxp_lpc_hml_mcu.ld` / `startup_nxp_lpc.c` | `arm-none-eabi-` |
| `ti_tm4c` | `ti_tm4c` | `cortex-m4f` | UART0 `0x4000C000` IRQ 5 | 1 MiB @ `0x00000000` | 256 KiB @ `0x20000000` | 65536 | `gcc_ti_tm4c_hml_mcu.ld` / `startup_ti_tm4c.c` | `arm-none-eabi-` |
| `microchip_sam` | `microchip_sam` | `cortex-m7` | USART2 `0x400E0800` IRQ 35 | 2 MiB @ `0x00400000` | 384 KiB @ `0x20400000` | 131072 | `gcc_microchip_sam_hml_mcu.ld` / `startup_microchip_sam.c` | `arm-none-eabi-` |
| `riscv` | `riscv` | `rv32imac` | UART `0x40000000` IRQ `-` | 128 KiB @ `0x00000000` | 32 KiB @ `0x20000000` | 4096 | `gcc_riscv_hml_mcu.ld` / `startup_riscv.c` | `riscv32-unknown-elf-` |

Two numbers above are **model stand-ins**, and they are flagged loudly in every
`.resc`, startup comment and per-vendor README rather than silently assumed
correct:

* The UART **register blocks** are Renode models (`UART.STM32_UART`,
  `UART.PL011`, `UART.NS16550`) placed at each part's documented console base
  address. The base addresses come from the datasheets; the register layouts the
  models implement have not been checked against a Renode release from this
  machine.
* The **interrupt line numbers** (37, 58, 5, 35) are from memory/datasheets,
  most unverifiable here, and pure decoration anyway: the demo enables no
  interrupt and handles no exception. They are recorded and cross-checked so
  they cannot silently rot, but nothing in a run would notice if one were wrong.

The RISC-V row is deliberately the odd one out (section 7).

## 2. Layout

```
renode/
  boards/index                      the manifest; all 16 columns are checked
  vendors/
    host_vendor_selftest.c          one HOST runner, reused by all five boards
    <vendor>/
      <vendor>.resc                 the emulated machine for this board
      <vendor>.robot                the assertions for this board
      firmware/
        vendor.env                  build inputs: ISA flags, linker, cap, prefix
        platform/
          board_config.h            the index's numbers as C constants
          gcc_<vendor>_hml_mcu.ld   the linker script
          startup_<vendor>.c        reset vector, .data/.bss, _sbrk, UART, task
```

Everything else is shared and never copied: `firmware/freertos_demo/app/` (the
entire check body) and `firmware/freertos_demo/stub_freertos/` (the cooperative
NOT-FreeRTOS stub) for the ARM boards, and the library's own portable C.

## 3. Install

Same as `docs/renode.md`, plus the RISC-V toolchain:

```sh
# ARM parts (all four share the same compiler)
sudo apt-get install -y gcc-arm-none-eabi

# RISC-V part
#   https://www.sifive.com/software (or your distro package), needs:
#     -march=rv32imac -mabi=ilp32  (the RV32 core is IMAC: no F/D, no C disabled)
riscv32-unknown-elf-gcc --version

# Renode portable build, plus its renode-test wrapper
#   https://github.com/renode/renode/releases
command -v renode renode-test
```

Any `*`-less IMAC RV32 toolchain works. The build passes
`-march=rv32imac -mabi=ilp32 -msdata=none -Wl,--no-relax` and links with
`--specs=nosys.specs` (no semihosting, no floating-point variant).

## 4. Run it

```sh
# from the repository root

# the machine-checked consistency proof: needs nothing, always must pass
renode/scripts/selfcheck_vendors.sh           # all five boards
renode/scripts/selfcheck_vendors.sh --quiet   # JSON on stdout, suited to CI
renode/scripts/selfcheck_vendors.sh --vendor riscv   # one board
renode/scripts/selfcheck_vendors.sh --no-negative    # skip the corruption probe

# build one image  -> renode/build/<vendor>/<vendor>_hml_mcu_demo.elf
renode/scripts/build_vendor.sh stm32f4
renode/scripts/build_vendor.sh riscv

# build and run one board's smoke test (needs Renode + that board's toolchain)
renode/scripts/run_vendor_smoke.sh stm32f4
renode/scripts/run_vendor_smoke.sh riscv --timeout 900

# the whole thing through CTest, the way CI does it
ctest --test-dir build -R 'renode' --output-on-failure
ctest --test-dir build -L vendor --output-on-failure
ctest --test-dir build -R stm32f4_renode_smoke -V   # the skip reason, in full
```

The 15 tests (three per board):

| Test | Always runs? | Can it fail? | Can it pass? |
| --- | --- | --- | --- |
| `<v>_renode_smoke` | yes, and is *skipped* without Renode + that board's toolchain | yes, whenever the tooling is present | only if the emulator ran and the firmware printed `result=PASS` |
| `<v>_renode_selfcheck` | yes, always expected to pass | yes, if this board's files rot out of agreement | yes, unconditionally, and that is fine: it asserts the harness, not the firmware |
| `<v>_renode_native_selftest` | yes, always | yes | yes, and it says almost nothing about an MCU (section 6) |

## 5. What a pass would mean, per board

A `<v>_renode_smoke` pass means: that board's demo image booted on its emulated
machine, executed the shipping codec/CRC/reassembly C, and printed
`HML-MCU-DEMO-DONE` / `result=PASS` on the emulated UART. The image's RTOS is a
cooperative stub (and on the RISC-V row, a shim living inside the startup file,
see section 7); there is no lwIP and no network stack at all.

A pass would **not** mean: FreeRTOS works, lwIP works, a datagram crosses a
network, a sensor works, the model matches silicon, or any real MCU was
involved. An emulator is not an MCU. The native selftest's own header says
exactly this and is registered in CTest with a `not-mcu` label so it can never
be mistaken for an emulated run.

## 6. The native selftest, and what it is allowed to claim

`renode/vendors/host_vendor_selftest.c` is compiled by CMake **five** times, once
per board, each time with `-I` at that board's `firmware/platform/` and the
shared demo body at that board's frame cap. It compiles every `board_config.h`
under `-Wall -Wextra -Wpedantic -Werror` and proves the cap the index declares
is one the library's reassembly checks actually accept. No Cortex-M and no RV32
instruction executes, no RTOS executes, Renode is not involved. The
`<v>_renode_native_selftest` tests always run on any machine, always pass on a
healthy tree, and are labelled `not-mcu`.

## 7. The RISC-V row, and why it is the outlier

`riscv` is `rv32imac`, not a Cortex-M, so it cannot reuse the ARM stubs:

* **No NVIC and no PLIC** are wired. The index column says `uart_irq -`; the
  `.resc` has no `nvic:`/`plic:` and the CPU is `CPU.RiscV32I`.
* **The tick** is the emulated CLINT's `mtime` (MMIO at `0x02000000+BFF8` for
  the low word, `+BFFC` for the high word), read from the supervisor-free
  machine-level timer loop by edges, with a bounded busy fallback. The shared
  `FreeRTOS.h` task API (`xTaskCreate`, `vTaskStartScheduler`, `vTaskDelay`,
  `taskYIELD`, `xTaskGetTickCount`, `hml_stub_elapsed_ms`,
  `hml_stub_systick_stalled`) is reimplemented in `startup_riscv.c`. The queue
  API is deliberately **not** present, because nothing this image links calls it
  -- adding it would mean writing an untestable port by hand.
* **`HML_VENDOR_SHARED_STUB=0`**: `firmware/freertos_demo/stub_freertos/` is not
  compiled into the RISC-V image. `build_vendor.sh` skips it.
* **`riscv_mtvec_entry`** intentionally dead-hangs: on this model there is no
  UART interrupt to service, so an interrupt means a bug and the honest thing is
  to stop.
* The stack and `mtvec` are set in assembly in the reset vector; `_start_c`
  does `.data`/`.bss` and `_sbrk` uses the same `__heap_start__`/`__heap_end__`
  symbols the linker defines, so the memory map rules are identical to the ARM
  boards.

Nothing about the RISC-V row has been run either. In particular `CPU.RiscV32I`,
`IRQControllers.RiscVClint` and the `VectorTableOffset` spelling are Renode
model names written from documentation, not from an executed release.

## 8. The negative test

`selfcheck_vendors.sh` runs a corruption probe by default: it copies one board's
directory into a temp tree, rewrites that board's `uart_addr` column in the
copied index to `0xdeadbeef`, and asserts the checker **fails** there. A
selfcheck that cannot fail on a corrupt index would be the check that failed to
check. It is on by default and can be disabled with `--no-negative` (as the
CTest selfcheck tests do, because they already run their own invariant).

## 9. What cannot be proven from this machine

Be literally honest in any report. From a machine with no emulator, no cross
compiler and no network, none of the following has been verified:

* that a real Renode release loads any `<vendor>.resc` (model names like
  `CPU.RiscV32I`, `IRQControllers.RiscVClint`, `UART.PL011`, `UART.NS16550`;
  `VectorTableOffset` semantics; the device trees);
* that any linker script links or any startup file compiles (no toolchain);
* that the interrupt lines are right (most are unverifiable here; they are
  decoration anyway);
* that the register offsets match the parts (they come from reference manuals,
  and the UART blocks are stand-in models);
* `riscv32-unknown-elf-` even exists (a guess at a common RISC-V prefix).

What **is** verified on any machine: all five boards stay textually in agreement
with `renode/boards/index` (the selfcheck), all five `board_config.h` compile
under `-Werror` with the shared demo body at the documented cap (the native
selftests), and all five skip paths behave (exit 77, no side effects, when the
skips are forced).

## 10. Adding a sixth board

1. Add a row to `renode/boards/index` (16 tab-separated columns; 15 if you
   prefer, but the checker expects 16 and will fail loudly if you guess).
2. Copy `renode/vendors/stm32f4/` to the new name and edit `.resc`, `.robot`,
   `vendor.env`, `board_config.h`, the linker script and the startup file to the
   new part's numbers.
3. Add the CTest triple to `renode/CMakeLists.txt`'s `HML_VENDORS` list.
4. Run `renode/scripts/selfcheck_vendors.sh --vendor <new>` and fix every
   failure it reports before running anything.