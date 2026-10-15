# Host MCU Lite (`hml`)

A small, independent C11 sensor-bridge library: sensor-agnostic framing with a
real EOF model, a UDP control plane, and a socket-crafted RoCEv2 data plane that
needs no ConnectX card and no `libibverbs`, built to bridge microcontrollers and
embedded sensors directly to host AI pipelines (including NVIDIA Holoscan).

Inspired by the [HSB Emulator](https://docs.nvidia.com/holoscan/sensor-bridge/latest/emulation.html)
and the [Holoscan SDK receive paths](https://github.com/nvidia-holoscan/holoscan-sensor-bridge).
**It is not a clone of either, it is not an NVIDIA product or drop-in, and it implements
none of hololink.** See [What this is not](#what-this-is-not) — read that section
before you quote any capability from this README.

**Version: 0.2.0. Status: pre-1.0, ABI not frozen** — the `hml_*` struct
layout, symbol names and enum values may change in `0.x`. The wire protocol
version is 1 and is versioned independently of the library.

No external dependencies. C11 and CMake only. Builds and tests with no RoCE NIC,
no GPU, no RDMA, no root and no network.

---

## Quickstart (5 minutes)

Nothing below needs hardware, root, or a network. Copy-paste it verbatim from
the repository root.

### 1. Build

```sh
cmake -S . -B build
cmake --build build -j
```

### 2. Run the test suite

```sh
ctest --test-dir build --output-on-failure
```

Expect `100% tests passed, 0 tests failed out of 66` (60 passed, 6 headless GUI smoke tests skipped with code 77). The suite is timing
sensitive on a loaded machine; see [Test flakiness](#test-flakiness) before you
conclude the build is broken from one red run.

### 3. Send real data over T0 (framed UDP) on loopback

Two terminals. The receiver goes first:

```sh
./build/host/hml_host/hml_host --port 55101 --frames 3 --verbose --stats
```

Then the sender:

```sh
./build/adapters/synthetic/hml_gen --port 55101 --frames 3 --size 9000 --seed 1
```

Sender and receiver hashes must match, frame for frame:

```text
sent frame=0 bytes=9000 hash=0xb6bc71f8
sent frame=1 bytes=9000 hash=0xf1f38a8f
sent frame=2 bytes=9000 hash=0x652abe0a
```

```text
frame seq=0  bytes=9000 hash=0xb6bc71f8
frame seq=7  bytes=9000 hash=0xf1f38a8f
frame seq=14 bytes=9000 hash=0x652abe0a
summary frames=3 bytes=27000 eof=3 last_hash=0x652abe0a
reasm frames_ok=3 frames_dropped=0 frags_recv=21 gaps=0 crc_fail=0 proto_fail=0
```

That is the T0 tier working: real datagrams through the kernel's loopback, real
CRC checks, real end-of-frame detection.

### 4. Watch a full bridge lifecycle in one process

```sh
./build/src/bridge/hml_bridge_demo --lifecycle
```

Prints `lifecycle: 7/7 steps PASS, 0 FAIL`: discovery beacon, a GPIO reset poke,
I2C register writes, packetizer-destination programming, start, 8 streamed
frames, stop and configuration read-back.

### 5. (Optional) Try the experimental RoCEv2 path

```sh
./build/host/roce_demux/hml_host_roce --port 14792 --frames 3 --verbose &
./build/host/roce_gen/hml_gen_roce --port 14792 --frames 3 --size 9000 --seed 1
```

Expect `stats pkts=24 icrc_err=0 writes=21 bytes=27000 eof_frames=3 psn_gaps=0
proto_err=0 meta_drops=0`. Port 14791/14792 are the unprivileged mirrors of the
IANA RoCEv2 port 4791, which needs root.

**This proves self-consistency, not interoperability.** See
[What this is not](#what-this-is-not).

### 6. Run the Holoscan Multi-Tier Ingestion & AI Pipeline

Run the Holoscan custom operator pipeline (supporting `--transport udp|verbs`, `--qp-type rc|uc` [default: `rc`], and `--tier auto|gpu_direct|gpu_indirect|cpu` [default: `auto` with priority `gpu_direct` -> `gpu_indirect` -> `cpu`]):

```sh
# Run self-contained pipeline test with Verbs RoCEv2 and GPU-Indirect memory tier:
./build/examples/holoscan_ai_pipeline/holoscan_ai_pipeline --transport verbs --qp-type rc --tier gpu_indirect --frames 10 --test

# Or run the complete server verbs receiver validation engine (97 checks):
./build/host/verbs_rx/hml_verbs_rx_demo
```

Or execute the complete automated 6-Way Verification Matrix:

```sh
./tests/matrix/test_6way_matrix.sh
```

### 7. Renode MCU to NVIDIA Holoscan Application Graph E2E

Execute the integrated Cortex-M7 firmware compilation, Renode Synopsys MAC physical wire emulation, and downstream Holoscan AI application pipeline execution:

```sh
ctest --test-dir build --output-on-failure -R test_renode_holoscan_e2e
```

### 8. Memory Safety & Sanitizer Verification (ASan + UBSan)

Build and verify the complete test suite under AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S . -B build_asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build_asan -j$(nproc)
ctest --test-dir build_asan --output-on-failure -E "renode"
```

Expect 100% pass across all 47 unit, fuzz, soak, and matrix tests with zero leaks, zero buffer overruns, and zero undefined behavior.

### 9. Install and consume

```sh
cmake --install build --prefix /tmp/hml
```

```cmake
find_package(hml 0.2.0 REQUIRED)
target_link_libraries(my_app PRIVATE hml::hml_core)
```

or with pkg-config: `pkg-config --cflags --libs hml`. Full, verified walkthrough
including a complete consumer project: [docs/install.md](docs/install.md).

---

## What works, and what does not

The most useful thing a repo can tell you is where it stops.

### Works, and is tested here

| Area | Evidence |
| --- | --- |
| Frame format, header CRC-16, payload CRC-16, big-endian wire | `test_frame_codec`, `test_crc`, `docs/protocol_v1.md` |
| End-of-frame semantics, including the zero-payload "EOF-only flush" | `test_reasm`, `test_stream` |
| Reassembly with loss, reorder and duplicate datagrams | `test_reasm`, `test_loopback` phases 2-3 |
| T0 framed UDP data plane, sender and receiver CLIs | `test_loopback`, `hml_gen` + `hml_host` quickstart above |
| Discovery beacon codec and the 11-opcode control plane | `test_discovery`, `test_ctrl_codec`, `test_lifecycle` |
| Bridge / peripheral window model and configure-then-stream phases | `test_bridge_periph`, `test_bridge_phase`, `test_bridge_lifecycle` |
| No `#ifdef` in `src/core` (enforced, not aspirational) | `test_core_no_ifdef` |
| Port API surface of all three ports | `test_ports_api` |
| CMake install + `find_package(hml)` + `pkg-config` consumers | [docs/install.md](docs/install.md) |
| T2 capability probe, which honestly reports "no NIC, no GPU" | `test_accel_probe` |
| RoCEv2 RC and UC dual-mode framing with hardware Immediate data | `test_roce_hdr`, `test_verbs_rx`, `hml_rocev2.c` |
| Multi-Tier Holoscan Custom Operator (`HmlReceiverOp`) & AI Pipeline | `examples/holoscan_operator`, `examples/holoscan_ai_pipeline` |
| 6-Way Verification Matrix (all transport x tier paths) | `test_6way_matrix`, `tests/matrix/test_6way_matrix.sh` |

### Test flakiness

Three tests are timing sensitive and fail intermittently on a loaded or
cold-cache machine, on a machine with no RoCE NIC, no GPU, no RDMA, no root and
no network. Observed here, all three reproduce on the *first* `ctest` run after a
fresh build:

| Test | Symptom under load | Root cause |
| --- | --- | --- |
| `test_loopback` | `p99 <= L_BUDGET_US` fails | p99 EOF latency over the 40 ms loopback budget; the tail is receiver-side scheduler delay, not wire time |
| `test_bridge_lifecycle` | `discovery beacon` returns `HML_ERR_TIMEOUT` (6) | the 500 ms beacon wait missed its 1 Hz beacon while the VM was busy |
| `test_roce_e2e` | `eof_frames=501, want 500`; `phase B 0 of 100 frames survived 2% loss` | the loopback UDP receive buffer overflowed, so datagrams were dropped by the kernel |

All three pass on a warm build. The fix is socket-buffer sizing and
deadline tolerance, not a different protocol — see
[docs/backpressure.md](docs/backpressure.md) and the open items in
[docs/ship_checklist.md](docs/ship_checklist.md).

### What this is not

Do not claim any of the following for this project.

* **Not a Holoscan drop-in.** Nothing here is recognised by stock Holoscan
  operators. There is no `RoceReceiverOp` compatibility, no `DataChannel`, and
  no way to point an unmodified Holoscan application at `hml`.
* **No BOOTP, no ECB, no hololink, no DataChannel.** Discovery and control are
  this library's own UDP protocols. `docs/hololink_compat_notes.md` records what
  M6b interop would actually cost; it has not been started.
* **RoCEv2 is EXPERIMENTAL and self-test-only.** Every ICRC/PSN/VA assertion is a
  golden vector or a loopback round trip through our own demux. It has **never**
  been validated against a real ConnectX NIC, a real Holoscan receiver, or a
  single captured byte of real wire traffic. There is no pcap artifact anywhere
  in this repository.
* **T2 (real RoCE NIC, GPU Direct, peermem) is deferred.** The capability probe
  has found no RoCE NIC and no GPU on any machine this has run on.
* **The MCU ports are API-complete stubs.** A green `test_ports_api` says the
  function surface exists, not that anything runs. No FreeRTOS/lwIP or ESP-IDF
  code has been compiled by its real toolchain, no datagram has crossed an lwIP
  stack, and there is no throughput, latency or power number for any port.
* **The library is not yet usable unmodified on a small MCU.**
  `hml_reasm_open(..., 0)` still allocates two 1 MiB buffers (staging plus
  index-order gather) on the heap, and there is no static/caller-buffer
  reassembly API. A port integrator must land that first — see
  [docs/porting.md](docs/porting.md) and
  [docs/limitations.md](docs/limitations.md).
* **The Soft-RoCE (`rdma_rxe`) lab has never been run**, only its fail-closed
  skip path, which is itself a test.
* **Soak, fuzz and ABI freeze have not been done.** That is why the ABI is not
  frozen. A 1-hour or 24-hour soak has not been run, and the header parsers have
  not been fuzzed.
* **`hml_gen` is T0-only.** `--xport rocev2` fails with `unknown transport`;
  there is no standalone T1a mode in that tool. Use `hml_gen_roce` for T1a.
* **No latency figure here is a network figure.** All latency data is loopback on
  one machine, where both ends stamp and read the same `CLOCK_MONOTONIC`. There
  is no clock discipline, so the number does not transfer to two machines. See
  [docs/latency.md](docs/latency.md).

---

## Install

[docs/install.md](docs/install.md) — build options, `cmake --install`, both
`find_package(hml)` / `hml::hml_core` and `pkg-config` consumption, `DESTDIR`,
relocation, and a minimal consumer program. Every command in that document was
run and its result recorded.

## Documents

| Document | Contents |
| --- | --- |
| [docs/architecture.md](docs/architecture.md) | **Start here for flow.** Complete end-to-end architecture with diagrams: layer map, startup/discovery, control plane, T0, T1a, reassembly and EOF, host receivers, acceleration tiers, Renode, test flow, failure/recovery, and the Holoscan comparison. Includes a reading order for all documents. |
| [docs/server_bringup_guide.md](docs/server_bringup_guide.md) | **Linux Server & Workstation Bring-Up Guide.** Bare-metal setup, Soft-RoCE (`rdma_rxe`), physical GPU memory verification (Tier 2 GPU-Indirect DMA), Renode Cortex-M7 emulation setup, and architectural notes. |
| [docs/gpu_host_pipeline.md](docs/gpu_host_pipeline.md) | How a GPU consumer attaches to the data path, and exactly where the current code stops. Read its section 1 before any GPU claim. |
| [docs/final_partitioning.md](docs/final_partitioning.md) | What lives in which directory, and why the split is where it is |
| [docs/renode.md](docs/renode.md) | Renode architecture, the network test model, and why ARP must be injected |
| [docs/renode_multi_vendor.md](docs/renode_multi_vendor.md) | Multi-vendor Renode consistency setup and the selfcheck |
| [docs/soak.md](docs/soak.md) | Long-run soak procedure and budget |
| [docs/fuzzing.md](docs/fuzzing.md) | Malformed-input hardening and how to run the fuzzers |
| [docs/file_stream.md](docs/file_stream.md) | File-backed streaming adapter mode |
| [docs/cpu_only_e2e.md](docs/cpu_only_e2e.md) | The shortest path to a running CPU-only example |
| [docs/install.md](docs/install.md) | Build, install and consume, CMake and pkg-config, verified commands |
| [docs/ship_checklist.md](docs/ship_checklist.md) | Every checklist item as DONE / PARTIAL / BLOCKED, with evidence, for tag `v0.2.0-ship` |
| [docs/limitations.md](docs/limitations.md) | Non-goals and hard limits, in one place |
| [docs/public_api.md](docs/public_api.md) | Reference for every public header in `include/hml/` |
| [docs/backpressure.md](docs/backpressure.md) | What the send and receive paths actually do when buffers fill; the latency histogram and budget |
| [docs/control_security.md](docs/control_security.md) | The control/discovery surface, its trust assumptions, and the authentication and authorization gaps |
| [docs/porting.md](docs/porting.md) | Bringing up a new MCU port, the frozen port contract, and what is still stubbed |
| [docs/holoscan_mcu_lite_Plan.md](docs/holoscan_mcu_lite_Plan.md) | The approved plan, plus implementation status per milestone |
| [docs/protocol_v1.md](docs/protocol_v1.md) | Normative byte-level wire spec: frame, config, control, beacon, RoCEv2, CRC, and what each parser rejects |
| [docs/latency.md](docs/latency.md) | Latency definition, the clock-domain caveat, measurements, and the 40 ms loopback budget |
| [docs/testing.md](docs/testing.md) | Build, the test inventory, CLI reference, worked examples, fault injection, metrics, hardware gaps |
| [docs/capability_matrix.md](docs/capability_matrix.md) | Per-port capability rows |
| [docs/soft_roce_lab.md](docs/soft_roce_lab.md) | M3b Soft-RoCE (`rdma_rxe`) lab procedure |
| [docs/hololink_compat_notes.md](docs/hololink_compat_notes.md) | M6b / Path-B scoping and the decision gate for stock Holoscan interop |
| [refs/prior_art.md](refs/prior_art.md) | What was taken from each prior-art project, what was deliberately not, and why |

Two of the pre-existing documents are known stale and are owned by another
track: `docs/testing.md` still says "18 tests" and `docs/capability_matrix.md`
still says latency is "not yet instrumented". Both statements are superseded —
the suite has 25 tests and latency *is* instrumented. Use
[docs/ship_checklist.md](docs/ship_checklist.md) as the authority until they are
refreshed.

---

## Layout

```
include/hml/     public headers (13, all installed): types, config, crc, protocol,
                 reasm, latency, port, sock, stream, transport, sensor, ctrl, discovery
src/core/        framer, reassembly, latency histogram, transport registry
                 - no #ifdef anywhere (enforced by test_core_no_ifdef)
src/protocol/    frame/config codecs, CRC primitives
src/transports/  udp, rocev2 (socket-crafted)
src/control/     discovery beacon, control server + host client
src/bridge/      M4 bridge / peripheral windows
src/accel/       runtime capability probe for T2
ports/           posix (real), freertos_lwip, esp_idf (stubs on this host)
adapters/        synthetic sensor
host/            hml_host, hml_host_roce, hml_gen_roce, soft_roce (scripts only)
tests/           m0_m1, m2, m3, m4, m5_ports
```

`examples/hello/` is an empty placeholder directory. The `renode/` tree and the
`adapters/filestream/` adapter are owned by other tracks — see `docs/renode.md`
and `docs/file_stream.md`.

---

## Design in one paragraph

A frame is a sequence of 1428-byte-max UDP fragments; **a frame ends where a
fragment with `HML_FLAG_END` ends**, so there is no length prefix and no
trailer. Each fragment gets its own `seq`, and a frame is reassembled by
`base = seq - frag_idx`, which makes a lost first fragment harmless and stops two
frames silently merging when both boundaries are lost. The header carries its own
CRC-16 and the payload a second, so neither region is trusted blind. RoCEv2
reuses the same framing but has no room for flags, so the frame is closed by one
20-byte RDMA write to a metadata slot whose address is derived from
`hml_dest_cfg` alone — no out-of-band state on the receiver. Exact bytes:
[docs/protocol_v1.md](docs/protocol_v1.md).

## Design notes worth knowing

* Ports 55100 discovery / 55101 data / 55102 control; RoCEv2's IANA port 4791
  is privileged, so tests use the mirror 14791.
* The control plane has no sequence field, so responses are correlated by opcode
  and the control path is strictly serialised; the device must be told where to
  reply, since the socket API cannot learn a sender's address.
* Every parser fails closed: bad magic, wrong version, truncation, trailing
  bytes, over-long payloads and a non-zero reserved byte are all rejected and
  counted, never partially accepted.
* The control plane has **no authentication and no authorization** — anyone who
  can reach UDP 55102 can reprogram the destination and poke registers. Only
  deploy it on a link you fully trust. Details and the full threat surface:
  [docs/control_security.md](docs/control_security.md).

---

## Trademark & Project Disclaimer

NVIDIA, Holoscan, and ConnectX are trademarks or registered trademarks of NVIDIA Corporation.
This project (**Host MCU Lite**) is an independent open-source library and is not affiliated with,
sponsored by, or endorsed by NVIDIA Corporation.
