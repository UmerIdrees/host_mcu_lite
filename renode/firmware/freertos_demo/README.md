# `freertos_demo` -- what is real here and what is a stub

Read this before quoting anything in this directory.

## The short version

| Path | What it is |
| --- | --- |
| `app/hml_mcu_demo.c` | **REAL.** Runs the shipping `holoscan_mcu_lite` library: CRCs, beacon codec, control codec, `hml_dest_cfg` codec, frame codec, reassembly. Nothing is reimplemented or faked. |
| `platform/startup_stub_rtos.c` | **REAL** Cortex-M reset/`_sbrk`/UART glue, driving a **STUB** task API. |
| `platform/main_host_selftest.c` | **HOST-ONLY** runner. No Cortex-M, no emulator, no RTOS. |
| `platform/gcc_arm_hml_mcu.ld` | **REAL** linker script, matching the emulated memory map. |
| `stub_freertos/` | **STUB. NOT FreeRTOS.** Read `stub_freertos/FreeRTOS.h`. |
| `lwIP` | **ABSENT.** There is no lwIP here, not a vendored copy, not a stub. |

There is **no FreeRTOS port in this directory and there never was one.** The
directory name says "freertos_demo" because that is what the demo would become;
what it *is* today is a codec smoke test driven by a cooperative stub.

## The stub, stated plainly

`stub_freertos/FreeRTOS.h` implements the slice of the FreeRTOS API that
`ports/freertos_lwip/hml_port_freertos.c` uses. It is:

* **not preemptive** -- nothing preempts anything;
* **not priority based** -- priorities are stored and ignored;
* **not a real-time kernel** -- no tick interrupt, no context switch, no port
  layer, no `FreeRTOSConfig.h`, no hooks, no idle hook, no stats, no tracing;
* **not compiled against real FreeRTOS** -- no FreeRTOS source was available
  when this was written, so no semantic difference between this shim and the
  kernel was ever detected. Any could exist and CI would not see it.

The only genuinely hardware-backed piece is the tick: a counter of SysTick
`COUNTFLAG` edges, which is real ARMv7-M behaviour. If a platform model never
drives SysTick, the stub's bounded fallback keeps the image running and the
demo says so on the UART (`HML-MCU-DEMO-WARN systick-did-not-advance`)
instead of hanging.

## What the image does not do

* It sends and receives **nothing**. No socket, no UDP, no lwIP, no MAC, no
  ARP. The emulated platform in `renode/holoscan_mcu_lite.resc` has no network
  peripheral to send with. The beacon and control codecs are exercised by
  handing bytes to the library parsers directly, not by putting them on a wire.
* It touches no sensor, no DMA, no cache, no MPU/PMP.
* It makes no timing claim, no throughput claim, no power claim.
* It enables no interrupt and handles no exception.
* **Emulated Cortex-M is not silicon.** A green Renode run is evidence about
  this C code and about the emulator, not about any real MCU.

## The memory constraint this image is built around

`hml_reasm_open(&r, 0)` takes the library default frame size,
`HML_MAX_FRAME_SIZE` = 1 MiB, and allocates **two** buffers of that size
(staging + index-order gather). That is **2 MiB of heap**, which does not fit
in the internal SRAM of a small Cortex-M. The linker script therefore puts the
newlib heap in the emulated SDRAM window at `0x60000000`.

If you are porting this to a part with no external SDRAM, build it with
`--frame-bytes N` (see `renode/scripts/build_mcu.sh`): that passes an explicit
smaller cap to `hml_reasm_open()`. The same code paths run and the smoke test
means the same thing; only the accepted frame size changes. The image always
prints the cap and the resulting heap size on the UART, and it fails loudly
(`HML-MCU-DEMO-FAIL`, and the robot test fails) if `hml_reasm_open()` cannot
allocate.

## How to make this a real FreeRTOS + lwIP validation

`docs/renode.md` section 7 has the full list. The short version: supply a real
FreeRTOS kernel plus its ARM_CM4F port layer and a `FreeRTOSConfig.h`, supply
real lwIP, build with `-DHML_PORT_HAVE_FREERTOS` so
`ports/freertos_lwip/hml_port_freertos.c` takes its real path, add an MCU BSP
for the clock tree and the FMC, and delete this directory's `stub_freertos/`
from the include path. Until then, nothing here says anything about FreeRTOS
or lwIP.
