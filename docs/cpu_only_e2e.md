# CPU-only end-to-end suite (`tests/e2e_cpu`)

WHAT THIS IS: a real-`127.0.0.1`, kernel-socket end-to-end suite. Each scene
links the *shipping* library (transports, host, control, framer, reassembler,
demo) and drives it over actual UDP loopback, then asserts counters that the
library itself documents. It runs on the build machine's CPU, outside Renode.

WHAT THIS IS NOT: an MCU, FreeRTOS, lwIP, or silicon result. There is no
`renode/boards/` table in this repository; every vendor number in the MCU
profile scene is a labelled assumption. Code that executes identically on the
host CPU tells you nothing about how it behaves under the MCU compiler,
FreeRTOS scheduler, lwIP stack, or real silicon. Treat this suite as a fast
host-CPU canary governed by `tests/e2e_cpu` + this document — not as proof of
MCU behaviour.

## Building and running

The target is compiled with the rest of the tree (Release, warnings as errors):

```sh
cmake --build build --target e2e_cpu
./build/tests/e2e_cpu/e2e_cpu --list          # enumerate the five scenes
./build/tests/e2e_cpu/e2e_cpu <scene> --frames N --seconds S
```

Scenes emit one line per case, e.g.
`E2E scene=roce-e2e case=clean_counters_exact PASS`, plus a final
`E2E scene=... result=PASS/FAIL checks=n fails=n`. The process exits 0 only if
nothing failed; exit codes are 0 or 1.

`tests/e2e_cpu/run_matrix.sh <binary> [--quick|--full]` runs every scene plus
`--all`, prints a summary table, and verifies no sender/receiver processes are
left behind (`pgrep` on `hml_host|e2e_cpu|hml_gen`). `--quick` (default):
frames=40, 30 s per-scene budget. `--full`: frames=300, 120 s per-scene.

CTest integration lives in `tests/e2e_cpu/CMakeLists.txt`:

```sh
ctest --test-dir build -R 'e2e_cpu' --output-on-failure
```

## The five scenes

| Scene             | What it proves |
|-------------------|----------------|
| `udp-loopback`    | shipping UDP transport carries frames over loopback: clean, EOF-flush, reordered delivery, flow-credit pacing |
| `roce-e2e`        | real RoCEv2-style datagrams over loopback: exact counter arithmetic, loss + recovery, header/payload CRC, partition |
| `reasm-edge`      | `hml_reasm` edge cases through a real socket: order, reversal, duplicates, gaps, restart, EOF-only, garbage, bad CRC, oversized, 100-frame soak |
| `control-beacon`  | `hml_control` server thread over loopback with the real `hml_client`: beacon golden vector, codecs + rejects, ping/discover/caps/start-stop/reset, unknown opcode, garbage accounting |
| `mcu-profile`     | `hml_mcu_demo` body natively at each labelled profile cap + a cap-sized frame delivered byte-exact and an over-cap frame dropped over a real socket |

`--all` runs all five in one process to prove the ports (45700-45799,
loopback-only) and per-scene time budgets coexist.

## Boundaries asserted by every case

* Private loopback port range: `E2E_PORT_BASE` 45700, 100 ports, disconnected
  from every other suite in the tree.
* Each case is time-bounded; the whole run has a deadline (`e2e_budget_ms`).
* Counter assertions use the *same* documented meaning as `hml_reasm_stats` /
  `hml_control_*_count`: no hand-rolled interpretations.
* A case cannot pass on a hang or a crash; `timeout -k 5` wraps every stage in
  the matrix.

## Control scene notes

* Everything against a real `hml_control` instance. The beacon golden vector
  `484d4c42010001000000002a0000000300010000000001000002ab47` (uuid 42,
  caps `HML_CAP_UDP|HML_CAP_ROCEV2`, fw 0x00010000, ttl 1, crc 0xab47) is
  cross-checked against the library's own `hml_crc16`.
* The v1 client matches responses by opcode; `HML_OP_ERROR` answers any
  request (documented rule). A stale error from a raw unknown-opcode datagram
  is therefore consumed by the client's *next* call, which the scene asserts
  explicitly instead of assuming request/response correlation.

## MCU profile scene notes

The vendor table is labelled in the source as *assumptions*:

| Profile        | SRAM     | SDRAM   | frame cap       |
|----------------|----------|---------|-----------------|
| renode-cortexm4| 512 KiB  | 4 MiB   | 1 MiB (`HML_MAX_FRAME_SIZE`) |
| stm32f4        | 192 KiB  | 0       | 64 KiB          |
| stm32f7        | 320 KiB  | 0       | 128 KiB         |
| stm32h7        | 1024 KiB | 0       | 256 KiB         |
| nxp-rt1062     | 1024 KiB | 0       | 256 KiB         |
| esp32-s3       | 512 KiB  | 0       | 128 KiB         |
| renesas-ra4m1  | 96 KiB   | 0       | 32 KiB          |
| microchip-same54 | 192 KiB| 0       | 64 KiB          |

`heap = 2 * frame_cap + sizeof(hml_reasm_ctx)` must fit the assumed SRAM (SDRAM
for renode), because reassembly stages + gathers into two buffers AND allocates
its own context (9488 bytes on 32-bit) inside that same heap. For each profile
the scene runs
the `hml_mcu_demo` body natively at the cap, then drives an over-cap frame
past the cap and confirms it is counted as dropped and never delivered.

The renode default cap (1 MiB) is the hard wire-format maximum, so no over-cap
frame is expressible on the wire at that cap; for that profile the *socket*
check uses `wire cap = 1 MiB - 2*HML_MAX_FRAG_PAYLOAD` (emitted as a
`wire_cap_lowered` note) while the demo still runs at the full 1 MiB profile.
All other profiles use their default cap for both.

## What changed on disk

* `tests/e2e_cpu/` — harness, five scenes, CMake target + CTest wiring
  (`e2e_cpu_matrix`, four per-scene tests `TIMEOUT 120`), `run_matrix.sh`.
* `tests/CMakeLists.txt` — one line: `add_subdirectory(e2e_cpu)`.
* This file.

No code outside `tests/` and `docs/` was modified.