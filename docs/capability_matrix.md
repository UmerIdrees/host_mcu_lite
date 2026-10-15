# Host MCU Lite (`hml`) — Capability Matrix (M5)

What each platform can actually do today, and how to prove it on your own
hardware. This file is deliberately boring: every cell is either backed by a
command you can paste, or it says `TBD (not measured)`. No cell contains a
number nobody ran.

- Rows are *platforms*, not tiers. A cell says whether that platform has been
  **verified** on that tier, or is **not present** (the platform cannot do it),
  or is **TBD (not measured)** (it might, nobody has shown it).
- Tier names follow the plan: **T0** framed UDP, **T1a** socket-crafted RoCEv2
  over a normal UDP socket, **T2** real RDMA hardware RoCE, **GPU Direct** GPU
  memory reachable by the NIC.
- "verified" always names the test or the command that produced it.

Measured host for the POSIX x86_64 row: Intel Core i5-1334U (8 vCPU), Linux
6.18.0 x86_64, VMware guest, `CMAKE_BUILD_TYPE=Debug`, MTU 1500, loopback
(127.0.0.1) only — **no NIC, no ConnectX, no GPU on this machine.**

## The matrix

| Platform | T0 UDP | T1a socket RoCEv2 | T2 HW RoCE | GPU Direct | control plane | max stable Mbps (measured) | loss at that rate | notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| POSIX x86_64 | **verified** — `test_loopback` (M0/M1) and the 5000-frame run below | **verified, loopback only** — `test_roce_e2e` (M3), emit + demux in one process over UDP/14791; no ConnectX anywhere near it | **not present** — no RDMA NIC in this VM (`hml_accel_probe()` reports `have_ibverbs=false`) | **not present** — no NVIDIA GPU and no `nvidia-peermem` here (`hml_accel_probe()` reports `have_gpu=false`) | **verified** — M2 discovery + control tests | **TBD (not measured)** for the max *stable* rate. Measured *ceiling* with the generator unthrottled: **≈71 Mbps wire, ≈6000 pps, 0.46% frame loss** — that is above the stable rate, not at it | 0.46% frame loss at the 71 Mbps ceiling (15–19 fragment gaps, 0 CRC failures, 0 protocol failures, 17–22 frames dropped of 5000, reproduced twice). Loss at the max stable rate: **TBD (not measured)** | The ceiling is a single generator process writing 9000-byte frames (7 fragments each) into one loopback socket with a single receiver. It is a sanity number for a Debug build in a VM, **not** a network figure. The zero-loss knee was not bracketed: `hml_gen --rate N` only reached ≈¼ of the requested frames/s, so no controlled low-rate point could be established. See "How to fill this row". |
| POSIX aarch64 | **TBD (not measured)** — no aarch64 build or run on this track | **TBD (not measured)** | **TBD (not measured)** | **TBD (not measured)** | **TBD (not measured)** | **TBD (not measured)** | **TBD (not measured)** | Same code as x86_64 and expected to behave the same, but "expected" is not "measured". Needs an aarch64 host or an aarch64 CI runner with the loopback tests enabled. |
| Linux SBC (e.g. Raspberry Pi / Jetson class) | **TBD (not measured)** | **TBD (not measured)** — a Pi without a ConnectX NIC can still do T1a over Ethernet, that is the interesting case | **TBD (not measured)** | **TBD (not measured)** | **TBD (not measured)** | **TBD (not measured)** | **TBD (not measured)** | Uses the *same* `hml_port_posix` as x86_64; nothing in `src/` changes, so the only thing to measure is the platform. On a Jetson, add the GPU Direct row: a Tegra GPU is not necessarily reachable by the NIC. |
| FreeRTOS + lwIP, control-first | **not measured** — the port compiles only as a stub on this host; there is no FreeRTOS toolchain here | **not claimed** — control-first, matches the STM32 reality in HSB Emulator | **not present** — MCU class, no RDMA hardware | **not present** | **not measured** — the API surface is verified by `test_ports_api` (argument contract, `HML_ERR_NOTSUP` on every socket call, monotonic clock), but no datagram has crossed an lwIP stack | **TBD (not measured)** | **TBD (not measured)** | Real path is behind `HML_PORT_HAVE_FREERTOS`: `xTaskGetTickCount()` clock, lwIP raw UDP PCBs, FreeRTOS queue bridging lwIP's callback into the polling `hml_sock_recv()` model. **Not compiled or run anywhere in CI yet.** |
| ESP-IDF, control-first | **not measured** — stub on this host | **not claimed** — control-first | **not present** — MCU class | **not present** | **not measured** — API surface verified by `test_ports_api` only | **TBD (not measured)** | **TBD (not measured)** | Real path is behind `HML_PORT_HAVE_ESP_IDF`: `esp_timer_get_time()`, the same lwIP + FreeRTOS-queue shape. IDF components and `CONFIG_LWIP*` knobs are listed at the top of `ports/esp_idf/hml_port_esp.c`. **Not compiled by idf.py yet.** |

Legend: **verified** = a named test or command in this repo produced it ·
**not present** = the platform cannot do it, and the code says so at runtime ·
**TBD (not measured)** = unknown, and nobody has run the experiment.

## Measurement method

Every row that says "measured" was produced with the same procedure, so two
measurements are comparable. Anything not in this list is not a measurement.

**What is recorded, always**

| Metric | Where it comes from | Meaning |
| --- | --- | --- |
| `pps` | receiver fragment counter ÷ wall time | wire datagrams per second |
| `Mbps` | (fragments × 1486 B × 8) ÷ wall time | 1486 B = 1458 B HML datagram + 20 B IPv4 + 8 B UDP, the MTU-1500 worst case |
| `frame_ok` | `reasm frames_ok=N` from `hml_host` | complete, correctly reassembled frames |
| `frame_gap` | `reasm gaps=N` from `hml_host` | missing fragment sequence numbers, i.e. loss seen on the wire |
| `pkt_loss_%` | (frames sent − `frames_ok`) / frames sent × 100 | end-to-end frame loss, the number that decides the tier |
| `crc_fail` / `proto_fail` | `reasm crc_fail`, `proto_fail` | must stay 0; a non-zero CRC rate means a broken path, not a slow one |
| p50 / p99 latency | ts_us in the frame header, host receive − source `ts_us` | per-frame one-way latency, EOF wake to frame delivered. **Not yet instrumented** — every latency cell in this document is `TBD (not measured)` |
| CPU% | `top -b -n1 -p <pid>` or `/proc/<pid>/stat` sampled over the run | sender and receiver separately |
| drop-policy hits | `reasm frames_dropped=N` plus the transport's drop counter | how often the drop-oldest / stop-stream policy fired; a non-zero value under nominal conditions is a bug, not a tuning result |

**Rules**

1. Loss is measured, not modelled. If `pkt_loss_% > 0`, the rate is not stable
   and the row must say so.
2. A rate only counts as *max stable* when `pkt_loss_% == 0` for a sustained run
   (≥60 s) after a warm-up. A number where loss is already visible is a
   *ceiling*, and it is labelled that way.
3. One variable at a time: frame size, fragment count and stream count are held
   fixed while the rate ramps, and the ramp is recorded.
4. Loopback results never become a network claim. Loopback proves the wire
   format and the reassembly path; it says nothing about a NIC, a switch, or a
   link budget.

## How to fill this row

Run these on the machine you are standing at. They are the commands that produced
the POSIX x86_64 numbers above, and they work unchanged for any Linux host.

```sh
# 1. Configure and build (Debug, as measured).
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"

# 2. T0, UDP, loopback. Sender:
./build/adapters/synthetic/hml_gen --port 55101 --frames 5000 --size 9000 --rate 0 --xport udp
# 3. Receiver, in the other terminal (Ctrl-C prints the summary):
./build/host/hml_host/hml_host --port 55101
#    -> "summary frames=... " and "reasm frames_ok=N frames_dropped=N gaps=N crc_fail=N proto_fail=N"
#    Fill pps / Mbps / pkt_loss_% / frame_gap from those two lines plus the
#    sender's own elapsed time.

# 4. T1a, socket-crafted RoCEv2 over loopback, with the demux in the loop:
./build/tests/m3/test_roce_e2e      # emit + demux, clean / 2% loss / misconfig phases

# 5. T0 and T1a as repeatable regression, all platforms that can build it:
cd build && ctest --output-on-failure

# 6. What this host could accelerate (M6 capability probe; prints nothing, so
#    read the test log):
./build/tests/m5_ports/test_accel_probe
#    have_ibverbs / have_gpu / have_peermem decide the T2 and GPU Direct cells.

# 7. T2 and GPU Direct, only on a ConnectX box:
ibv_devinfo                                   # device present? active port? link layer = InfiniBand (RoCE)
lsmod | grep nvidia-peermem                    # GPU Direct reachable by RDMA hardware?
#    Then open HML_ACCEL_HOST_MR and post sends; compare CPU% against T0 for the
#    same Mbps. Until that runs, both cells stay "not present"/"TBD".

# 8. RTOS rows. Build with the real toolchain; the host build only proves the stub.
#    FreeRTOS:  add -DHML_PORT_HAVE_FREERTOS and put FreeRTOS + lwIP on the include path
#    ESP-IDF:   idf.py build with REQUIRES esp_timer lwip freertos
#    Then run the same hml_gen/hml_host example against the device and repeat steps 2-3.
```

Per-row extras:

- **POSIX aarch64** — the same steps 1-5 on an aarch64 host or aarch64 CI runner
  (`cmake -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc` plus a runner/emulator that
  can bind a UDP socket).
- **Linux SBC** — steps 1-5 on the SBC, then step 6 to see whether the NIC is
  RDMA-capable. Expect a 1 GbE ceiling; measure it, do not assume it.
- **FreeRTOS/lwIP** — build with `HML_PORT_HAVE_FREERTOS`, then run the same
  synthetic frame generator against the MCU and read `gaps`/`frames_dropped` from
  the device. The first real number will be dominated by tick resolution
  (`CONFIG_FREERTOS_HZ`): if it is 100 Hz, the protocol timestamps are 10 ms
  apart and per-frame latency cannot be measured at all until that is raised.
- **ESP-IDF** — `idf.py build flash monitor`, same generator, same counters, and
  read the `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE` / `CONFIG_LWIP_SO_RCVBUF` knobs in
  `ports/esp_idf/hml_port_esp.c` before concluding the port is slow.

## Portability statement: no `#ifdef` in `src/`

The plan's portability rule is *portability lives in `ports/`, not in the shared
source*. A preprocessor conditional in `src/` is how one platform's idea of the
world leaks into every other platform, and it is the single most expensive
defect this project can grow, because it is invisible until someone builds for
the second platform.

The rule is enforced mechanically, not by good intentions:

- `tests/m5_ports/test_core_no_ifdef.c` walks **every `.c` and `.h` under
  `src/`** and fails with `file:line` on `#if`, `#ifdef`, `#ifndef`, on an
  include of `unistd.h`, `sys/socket.h`, `lwip/…`, `freertos/…` or `esp_…`, and
  on the strings `"posix"` / `"FreeRTOS"` inside `src/core`.
- The only two things it allows are the two universal C idioms that are not
  platform conditionals: an include guard (`#ifndef X` followed by `#define X`)
  and `__cplusplus` around `extern "C" {`.
- Optional features are chosen by the **build system**, not by the source.
  `src/accel/` is the pattern: `CMakeLists.txt` picks exactly one backend
  translation unit (`hml_accel_ibverbs.c` or `hml_accel_backend_stub.c`), so
  neither file needs a conditional. The same approach is what the RTOS ports
  use, with `HML_PORT_HAVE_FREERTOS` / `HML_PORT_HAVE_ESP_IDF` set by the target
  that actually has the headers.

Current status: **passing**, 29 `.c`/`.h` files scanned, 0 violations.

Adding a platform means adding a `ports/<platform>/` directory and nothing else.
If you find yourself wanting an `#if` in `src/`, the answer is a new port or a
function pointer that the port fills in — not a conditional.
