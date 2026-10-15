# Renode MCU Smoke Test (`Host MCU Lite`)

**Status: the emulator run has never happened.** This harness was written on a
machine with no Renode, no `mono`, no `dotnet` and no `arm-none-eabi-gcc`. The
platform description and the Robot test are therefore *unexecuted and
unverified*, and the CTest test that drives them reports **skipped** there, not
passed. Read section 8 before you quote any of this as a result.

This directory exists to do one small job: run the library's codec, CRC and
reassembly C on a Cortex-M and check the answers, with an emulator standing in
for the silicon. It is the cheapest available evidence that the portable code
compiles and behaves for a microcontroller, and it is *not* evidence about
FreeRTOS, about lwIP, about networking, or about any physical device.

| Document | Contents |
| --- | --- |
| [../renode/CMakeLists.txt](../renode/CMakeLists.txt) | The three CTest tests and what each one is allowed to claim |
| [../renode/holoscan_mcu_lite.resc](../renode/holoscan_mcu_lite.resc) | The emulated platform: CPU, UART, flash, SRAM, SDRAM window |
| [../renode/holoscan_mcu_lite.robot](../renode/holoscan_mcu_lite.robot) | The assertions |
| [../renode/scripts/build_mcu.sh](../renode/scripts/build_mcu.sh) | Cross-compiles the image |
| [../renode/scripts/run_smoke.sh](../renode/scripts/run_smoke.sh) | Builds and runs the robot test, with every timeout and the skip path |
| [../renode/scripts/selfcheck.sh](../renode/scripts/selfcheck.sh) | Proves the skip path is intact; always passes |
| [../renode/firmware/freertos_demo/README.md](../renode/firmware/freertos_demo/README.md) | The real/stub boundary, file by file |

---

## 1. Install

Nothing here is needed to build, install or test `holoscan_mcu_lite` itself. The
library and its own 22-test CTest suite must stay green on a machine with no
emulator, no cross compiler and no network, and they do. This directory adds
three more CTest tests (`renode_smoke`, `renode_selfcheck`,
`renode_demo_native_selftest`), for 25 in total.

To run the smoke test you need both of these:

### 1.1 The ARM cross compiler

The firmware is plain Thumb-2 Cortex-M4 C, so any `arm-none-eabi-gcc` will do.

```sh
# Debian / Ubuntu
sudo apt-get install -y gcc-arm-none-eabi

# Fedora / RHEL
sudo dnf install -y arm-none-eabi-gcc-cs

# macOS (Homebrew)
brew install --cask gcc-arm-embedded

# anything else: https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
arm-none-eabi-gcc --version
```

If your cross compiler has a different name, point the build at it:

```sh
HML_CROSS_CC=/opt/gcc-arm/bin/arm-none-eabi-gcc \
  renode/scripts/build_mcu.sh
```

### 1.2 Renode, plus `renode-test`

```sh
# Portable builds (this is what the harness expects: renode and renode-test
# next to each other in one directory)
#   https://github.com/renode/renode/releases
wget https://github.com/renode/renode/releases/download/v1.15.3/renode-1.15.3.linux-portable.tar.gz
tar xf renode-1.15.3.linux-portable.tar.gz
./renode-1.15.3/renode --version
```

Renode is distributed as a portable bundle (needs `mono` on Linux, or `dotnet`
for the newer native builds) and as packages:

```sh
# Debian / Ubuntu, from the Renode apt repo
curl -s https://raw.githubusercontent.com/renode/renode/master/scripts/renode-apt.sh | sudo bash -
sudo apt-get install -y renode

# macOS
brew install --cask renode
```

The harness drives **`renode-test`**, the Robot Framework entry point that ships
in the same directory as `renode`. Check both are present and runnable:

```sh
command -v renode renode-test
renode-test --help | head
```

If they are not on `PATH`, name them explicitly:

```sh
HML_RENODE=/opt/renode/renode \
HML_RENODE_TEST=/opt/renode/renode-test \
  renode/scripts/run_smoke.sh
```

**The robot test needs no X server.** It is run with `--console`, so it works
over SSH and in CI. (Running the `.robot` file from the Renode *GUI* does want a
display; that is not the path this harness takes.)

---

## 2. Run it

Everything, from the repository root:

```sh
# 1. see what is missing, and whether it can run at all
renode/scripts/build_mcu.sh --help

# 2. cross-compile the image only  -> renode/build/hml_mcu_demo.elf
renode/scripts/build_mcu.sh

# 3. build the image and run the robot test under Renode
renode/scripts/run_smoke.sh

# 4. through CTest, the way CI does it
ctest --test-dir build -R renode --output-on-failure
ctest --test-dir build -R renode_smoke -V      # the skip reason, in full
```

Useful knobs:

```sh
renode/scripts/build_mcu.sh --frame-bytes 32768   # small heap (see section 3)
renode/scripts/build_mcu.sh --out /tmp/hml-mcu --cc /opt/gcc-arm/bin/arm-none-eabi-gcc
renode/scripts/run_smoke.sh --timeout 900 --keep-log
HML_MCU_SMOKE_SKIP=1 renode/scripts/run_smoke.sh  # force the skip, exit 77
```

### 2.1 The three CTest tests

| Test | Always runs? | Can it fail? | Can it pass? |
| --- | --- | --- | --- |
| `renode_smoke` | yes (and is *skipped*) | yes, whenever Renode and the toolchain are present | only if the emulator ran and the firmware printed `result=PASS` |
| `renode_selfcheck` | yes, always expected to pass | yes, if the skip path or the markers rot | yes, unconditionally, and that is fine: it asserts the harness, not the firmware |
| `renode_demo_native_selftest` | yes, always | yes | yes, and it says almost nothing about an MCU (section 5) |

`renode_smoke` has `SKIP_RETURN_CODE 77`. Both scripts exit 77 when Renode or
`arm-none-eabi-gcc` is missing, printing exactly what is absent:

```
run_smoke.sh: [skip] the 'renode' executable was not found (looked in $HML_RENODE and PATH). ...
run_smoke.sh: [skip] The Cortex-M smoke test did NOT run. This is not a pass:
run_smoke.sh: [skip] no Cortex-M code was executed and nothing was validated.
```

When the tooling **is** present and the run fails, the exit code is non-zero
and CTest says `***Failed***`. A hang is bounded by `timeout` (default 300s,
plus a 15s kill-after) and is reported as a **failure**, never as a skip,
because an image that boots and then wedges is a real defect.

### 2.2 Exit codes

| Code | Meaning |
| --- | --- |
| 0 | The emulator ran and the firmware printed `result=PASS` |
| 77 | Skipped: no Renode, or no ARM toolchain. **Not a pass.** |
| 2 | Usage error in the arguments (a bug in the caller, not a skip) |
| 1 | The tooling was present and the run failed, or the build broke, or a self-check assertion broke |

---

## 3. The memory constraint, stated plainly

This is the one design fact a real bring-up must deal with first.

`hml_reasm_open(&r, 0)` takes the library's default frame size,
`HML_MAX_FRAME_SIZE` = **1 MiB**, and allocates **two** buffers of that size --
one staging area, one index-order gather buffer (`src/core/hml_reasm.c`, with
the note in the source). That is **2 MiB of heap**, from a single call, before
any payload buffer.

| Part | Internal SRAM | Default cap fits? |
| --- | --- | --- |
| STM32F4 (e.g. F405/F407) | 192 KiB | **No** |
| STM32F7 (e.g. F746) | up to 320 KiB + 512 KiB TCM/DTCM | **No** |
| STM32H7 + external SDRAM | ~1 MiB internal + SDRAM | Only in SDRAM |
| NXP RT-series + external SDRAM | ~512 KiB + SDRAM | Only in SDRAM |

The emulated platform in `renode/holoscan_mcu_lite.resc` models exactly that
shape: 512 KiB of internal SRAM at `0x20000000` and a 4 MiB external-SDRAM
window at `0x60000000`, and the linker script puts the newlib heap in the SDRAM
window. It is not hidden: the demo prints the cap and the resulting heap size
on the UART on every boot, `build_mcu.sh` prints it in the build log, and if
`hml_reasm_open()` cannot allocate, the image emits `HML-MCU-DEMO-WARN` and then
`HML-MCU-DEMO-FAIL`, which **fails** the robot test.

On a part with no external SDRAM, build with an explicit cap:

```sh
renode/scripts/build_mcu.sh --frame-bytes 32768     # 32 KiB frames, 74 KiB of heap (two buffers + the reassembler context)
```

The same code paths run, the same checks execute, and the smoke test means the
same thing. Only the largest accepted frame changes. Note that on real silicon
the SDRAM window needs FMC configuration, a refresh timer, pin muxing and a
cache/MPU policy before the first allocation; the emulated platform has no SDRAM
controller to configure, so the image does not do any of that.

---

## 4. What the smoke test does

Inside the emulated Cortex-M4, `renode/firmware/freertos_demo/app/hml_mcu_demo.c`
calls the shipping library and checks its answers. Every one of these is a real
library code path; none is re-implemented, mocked or short-circuited.

1. **CRC primitives** -- `hml_crc16` and `hml_crc32_ieee` against the published
   check vectors (`"123456789"` -> `0x29B1` and `0xCBF43926`), plus the
   documented `0xFFFF` init value for an empty input.
2. **Discovery beacon** -- `hml_beacon_fill` / `hml_beacon_parse` round trip
   (uuid, caps, fw version, ttl, 48-bit timestamp), and rejection of a short
   beacon, an over-long beacon, a bad magic, a corrupted CRC byte, and a
   flipped field.
3. **Control codec** -- `hml_ctrl_fill` / `hml_ctrl_parse` for `PING` and for
   `SET_DEST` carrying a full `hml_dest_cfg`, then `hml_proto_cfg_fill` /
   `hml_proto_cfg_parse`; then rejection of a bad magic, the wrong version, a
   truncated message, a trailing byte, a corrupted CRC, an over-long
   `payload_len`, and each of the `hml_dest_cfg` reserved bytes being non-zero.
4. **Frame codec** -- `hml_proto_frame_fill` / `hml_proto_frame_parse` round
   trip, and rejection of a bad magic, the wrong version, a bad header CRC, a
   flipped payload byte, and a header-only datagram that announced a payload.
5. **Reassembly** -- `hml_reasm_open` / `hml_reasm_feed_datagram` /
   `hml_reasm_close`: a three-fragment 200-byte frame delivered exactly once
   and byte-exact, a corrupt fragment in the middle counted in `crc_fail` and
   never delivered, the next clean frame still arriving, an EOF-only flush
   firing a zero-byte callback without incrementing `frames_ok`, and a frame
   that overruns the configured cap being dropped and counted rather than
   overrunning the buffer.
6. **The bare-metal path itself** -- the reset vector, the `.data` copy, the
   `.bss` zero, `_sbrk` (without which the reassembly buffers cannot exist),
   the UART write, and the SysTick read that drives the tick. If the image
   cannot get that far, no marker line appears and the test times out as a
   **failure**.

The robot test asserts the banner, the two "this is not X" notes, the `READY`
marker, every phase marker, the task's `rc=0`, and finally
`HML-MCU-DEMO-DONE result=PASS`.

## 5. What the smoke test does NOT cover

Say this out loud whenever the result is quoted.

* **Not FreeRTOS.** The image links `firmware/freertos_demo/stub_freertos/`, a
  hand-written single-task cooperative shim. It is not preemptive, has no
  priorities (they are stored and ignored), no tick interrupt, no context
  switch, no port layer, no `FreeRTOSConfig.h`, no hooks, no idle task. It was
  never compiled against real FreeRTOS, so **no semantic difference between the
  shim and the kernel has ever been detected**, and CI could not detect one.
  Nothing here exercises preemption, priority inversion, starvation, stack
  overflow, or a single real RTOS guarantee.
* **Not lwIP, and not networking at all.** No lwIP source is present, vendored,
  or stubbed. No socket is opened. **Not one datagram is sent or received** --
  the platform description has no network peripheral, so sending one is not
  merely out of scope, it is impossible. The beacon and control codecs are
  exercised by handing bytes to the library parsers inside the image, which
  tests the codec and nothing about the transport.
* **Not RoCEv2.** No verbs, no NIC model, no RoCEv2 packet. `HML_CAP_ROCEV2` is
  not claimed by the demo, and neither is `HML_CAP_UDP`: the image
  deliberately advertises `HML_CAP_MCU` only, so a host cannot act on a
  capability this image does not have.
* **Not a sensor, a DMA engine, a cache, an MPU, an interrupt or a clock tree.**
  None are present or enabled. The vector table holds two entries; a fault
  would be undefined behaviour, which is why the demo is written not to fault.
* **Not timing, throughput, power or memory efficiency.** Nothing is timed. The
  2 MiB reassembly default from section 3 is a cost, not a feature; no figure in
  this repository should be read as a performance number for an MCU.
* **Not silicon.** An emulator is a model with its own bugs. A green run says
  the code compiled for `cortex-m4` and produced the expected values under that
  model. It says nothing about an STM32, an NXP part, a real clock tree, real
  SRAM timing, real flash wait states, or real silicon bugs.
* **Not a port.** There is no FreeRTOS port in this repository. The existing
  `ports/freertos_lwip/hml_port_freertos.c` real path is still uncompiled
  anywhere, as [capability_matrix.md](capability_matrix.md) records.

## 5.1 `renode_demo_native_selftest` -- the one test that is easy to misread

This CTest test compiles `app/hml_mcu_demo.c` for the **build machine's**
architecture, links the **build machine's** library, and runs it with `printf`
as the sink. No Cortex-M instruction executes, no RTOS executes, Renode is not
involved, and there is no memory map, no MMIO, no SysTick and no UART.

Its only two jobs are to keep `hml_mcu_demo.c` compiling under
`-Wall -Wextra -Wpedantic -Werror` on a machine with no cross compiler, and to
keep the UART marker strings in sync with the robot test so a rename cannot turn
into a mysterious timeout under Renode.

An x86_64 run of this test is **not** a microcontroller result. Do not report it
as one. `renode_smoke` is the Cortex-M one.

---

## 6. The stub / real boundary

| Thing | Status | Where |
| --- | --- | --- |
| `app/hml_mcu_demo.c` | **REAL** library code under test | `renode/firmware/freertos_demo/app/` |
| `platform/startup_stub_rtos.c` | **REAL** reset/`_sbrk`/UART/clock, driving a stub task API | `renode/firmware/freertos_demo/platform/` |
| `platform/gcc_arm_hml_mcu.ld` | **REAL** linker script matching the emulated map | same directory |
| `platform/main_host_selftest.c` | **HOST-ONLY** runner, not Cortex-M | same directory |
| `stub_freertos/*` | **STUB. NOT FreeRTOS.** | `renode/firmware/freertos_demo/stub_freertos/` |
| lwIP | **ABSENT.** Not vendored, not stubbed. | -- |
| `ports/freertos_lwip/hml_port_freertos.c` | Pre-existing, **still uncompiled** on any real target | `ports/freertos_lwip/` |

The stub's `FreeRTOS.h` says `THIS IS NOT FreeRTOS` in its first line, and
`renode_selfcheck` fails the build if that sentence is ever removed. The
firmware `README.md` repeats the boundary, and every UART note line the image
prints says out loud that it is not a FreeRTOS port and that no datagram leaves
the image. The intent is that no reader, human or machine, can come away with
the impression that a FreeRTOS+lwIP port has been validated here.

---

## 7. Making it a real FreeRTOS + lwIP validation

The order matters: the network only becomes interesting once the RTOS is real,
because a UDP path over a stub scheduler proves nothing.

**Step 1 -- a real RTOS.** Fetch the FreeRTOS kernel plus its `ARM_CM4F` port
layer and write a `FreeRTOSConfig.h` (tick rate, heap, `configUSE_PREEMPTION`,
the hooks you actually need). Do not take any of it from this repository: none
of it is here, and it must not be faked.

**Step 2 -- a real lwIP.** Fetch lwIP and configure it for raw UDP PCBs.
`ports/freertos_lwip/hml_port_freertos.c` already documents the intended shape
(`udp_new` / `udp_bind` / `udp_connect` / `udp_recv` with a pbuf queue bridging
the callback into the polling `hml_sock_recv` model). The `stub_freertos/queue.h`
API it needs (`xQueueCreate` / `xQueueSend` / `xQueueReceive` / `vQueueDelete`)
is already shaped to match, so the real queue is a drop-in.

**Step 3 -- drop the stub.** Delete `renode/firmware/freertos_demo/stub_freertos/`
from the include path and replace the stub-driven task body with a real one that
calls `xTaskCreate` for the control-plane task and `vTaskStartScheduler()`. The
task should own the lwIP `tcpip` thread rather than fighting it.

**Step 4 -- build it.** Compile with `-DHML_PORT_HAVE_FREERTOS` so
`hml_port_freertos.c` takes its real path, and add the real sources, an MCU BSP
(clock tree, pin mux, and the FMC if the 2 MiB heap is to live in SDRAM), and a
linker script for the part. `build_mcu.sh` will need to grow a real FreeRTOS
source list; today it deliberately compiles no RTOS at all.

**Step 5 -- give it a link.** Add a network peripheral to
`renode/holoscan_mcu_lite.resc`. Renode's STM32 platform models `ethernet:
Ethernet.STM32F7_ETHERNET @ sysbus 0x40028000`; wire it to a
`NetworkInterface` and let the two ends talk. **Only then** can you claim
anything about a datagram, and even then only about the emulator.

**Step 6 -- assert on it, and say what it is.** Extend the robot test to wait
for a real beacon and to check a real control round trip, and add the
host-side half: `host/hml_host` plus a control client, so the claim becomes
"the MCU image sent N frames that the host reassembled with matching hashes".
Until step 6 is done, a boot log is not a network result.

**Step 7 -- record it honestly.** When all six steps are done, update
[capability_matrix.md](capability_matrix.md) and [testing.md](testing.md), which
currently say this port is uncompiled. Until then those statements stay true
and this document stays as it is.

---

## 8. Honest status of this harness

| Claim | Status |
| --- | --- |
| The build stays green with no Renode, no cross compiler, no network | **Verified.** `renode_smoke` is reported as `***Skipped***`, never as a failure. Last full run: 25 tests, `100% tests passed, 0 tests failed out of 25`, with `2 - renode_smoke (Skipped)`. |
| The harness fails closed when the tooling is missing | **Verified** by `renode_selfcheck`, which forces the skip path on every machine and requires exit 77 with no side effects. |
| `hml_mcu_demo.c` compiles clean and its checks pass | **Verified for the host build only** (`renode_demo_native_selftest`: 96 checks, 0 failures, under `-Werror`). This says nothing about Cortex-M execution. |
| The ARM build command line is right | **Partly verified.** `build_mcu.sh` was run with a stub `arm-none-eabi-gcc`: the source list, include paths, `-DHML_MCU_FRAME_BYTES`, the map file and the artefact check all behave, and a stub toolchain that produces no ELF is reported as a **failure** (exit 1), not a skip. The compiler itself has never run, so the ARM compilation and link are unverified. |
| The platform description loads in Renode | **UNVERIFIED.** Never executed. If `UART.STM32_UART` or a register offset differs in your Renode version, `run_smoke.sh` reports a **failure**, not a skip, and the fix belongs in this repository. |
| The firmware boots on an emulated Cortex-M4 and prints `result=PASS` | **UNVERIFIED.** Never executed. |
| The stub RTOS behaves like FreeRTOS | **No evidence, and not claimed.** The only hardware-backed part is the SysTick tick; see section 5. |
| Any MCU, sensor, FreeRTOS or lwIP statement | **Not supported by this harness.** See section 5. |

If you are the first person to run this with Renode installed, the two most
valuable things you can leave behind are a note in this file saying what you
actually observed, and a fix for whatever broke. Leaving a "skip" in the log is
a perfectly good outcome for this repository's CI; leaving a *stale claim of a
pass* is not.
