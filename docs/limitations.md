# Limitations and non-goals

What `v0.2.0-ship` does **not** do, and why. This is the document to read before
you rely on anything in the [README](../README.md).

The project is an **independent, HSB-inspired library**, not a Holoscan port and
not a drop-in for anything. Two decisions shape most of what follows:

1. **Independent library (plan Path 1A).** We do not link the Holoscan SDK into
   the MCU library, and we do not clone hololink's wire protocols.
2. **Ship T0 + T1a, defer everything needing hardware.** There is no RoCE NIC and
   no GPU in the build/validation environment.

Anything below marked **not implemented** is a deliberate non-goal, not a bug.

---

## 1. Holoscan and hololink interop: **not implemented**

**The single most important limitation.** An HML device is **not** usable by a
stock Holoscan application.

| Expectation | Reality |
| --- | --- |
| `RoceReceiverOp` works out of the box | **No.** That operator expects hololink to have configured the bridge via BOOTP + ECB + `DataChannel`. We implement none of those. |
| `LinuxReceiverOperator` with a GPU path | **No.** We ship `hml_host`, a CPU UDP receiver speaking our own framed protocol. |
| BOOTP discovery | **Not implemented.** We ship our own 28-byte beacon on UDP 55100. Two discovery protocols would be two sources of truth for the same three values. |
| ECB register access | **Not implemented.** |
| `DataChannel` destination programming | **Not implemented.** We use our own `HML_OP_SET_DEST`. |
| HSB Emulator as a reference peer | **Not run.** |

**RoCEv2 packets alone do not unlock the stock operator.** Our T1a packets have
the right shape, but the control path that configures the bridge is the actual
gate, and it is not built. See [hololink_compat_notes.md](hololink_compat_notes.md)
for the gap analysis, and [ship_checklist.md](ship_checklist.md) §7.

The shipped host receiver is `hml_host`. Use it.

---

## 2. RoCEv2 is experimental: **no hardware validation**

The RoCEv2 implementation (BTH/RETH/PSN/ICRC, UC WRITE, 20-byte EOF metadata)
is **verified only against itself, on loopback, in one process family.**

* **No real ConnectX/Mellanox NIC.** The BTH layout and ICRC were written from
  the specification, **not captured from a NIC or from wire traffic.** We have
  no packet capture to compare against.
* **No switch, no dual-host RoCE, no real QP.** Loopback does not exercise
  congestion control, MTU discovery, PFC/ECN, or a switch's behaviour.
* **No `ibverbs`, no GPU Direct, no `nvidia-peermem`.** The acceleration path is
  off; T0 CPU is the shipped path.
* **Soft-RoCE (T1b) was never run.** `roce_lab_skip_path` passes by confirming
  the lab is *skipped*. That is not evidence Soft-RoCE works.

Treat T1a as a **self-consistent implementation under test**, not as validated
interoperability. A real NIC may reject or mis-parse our packets.

---

## 3. MCU readiness: **not ready on a small MCU**

The project is named for MCUs, but **`v0.2.0-ship` does not fit on a small MCU.**
This is a design gap, not a tuning problem.

### 3.1 The 2 MiB blocker

`hml_reasm_open(r, 0)` allocates **`2 * HML_MAX_FRAME_SIZE` = 2 MiB of heap**
(staging + gather) by default, plus ~9 KiB of fixed per-context state.

**There is no static-buffer or caller-buffer reassembly API.** If your MCU has
less than ~2 MiB of free heap, this library does not fit.

The partial workaround is a smaller `cap`, but understand what it does: a frame
larger than `cap` is **poisoned and dropped whole** (`frames_dropped++`), never
delivered. So `cap` is a promise that frames never exceed it, not a buffer knob.
Pair it with `hml_stream_cfg.max_frame_bytes`, which also enforces the limit at
the sender. This works, but it means **MCU frames must be well under 1 MiB.**

### 3.2 The two MCU ports are unverified

`ports/freertos_lwip` and `ports/esp_idf` each have a real path **and** a
host-buildable stub. The host build compiles the stub, so:

* **A green host build proves nothing about the real MCU path.**
* Neither port has been compiled by its own toolchain (ESP-IDF, arm-none-eabi).
* Neither has been run on hardware.
* **No measured task stack depths exist for any port.**

Also: the MCU RX queue is a **hardcoded 8 datagrams** (~11 KiB) in both ports,
and its overflow has **no counter** — a dropped datagram surfaces only later as
reassembly `gaps`, indistinguishable from wire loss.

See [porting.md](porting.md) §7 and [backpressure.md](backpressure.md) §2.4.

---

## 4. Security: **no authentication, no encryption**

The control plane (UDP 55102) is **completely unauthenticated and unencrypted.**
Anyone who can reach the port can `SET_DEST` (redirect the stream), `POKE`
(write a device register), `READBACK` (read one), and `START`/`STOP`/`RESET`.
Both the device and the client bind `HML_IP_ANY` (0.0.0.0), so there is no
loopback-only option.

* A tree-wide grep for `auth`, `token`, `tls`, `ssl`, `encrypt`, `hmac`,
  `nonce`, `secret` finds **no security mechanism of any kind.** The only
  "token" is a 32-bit PING echo value.
* The **CRC-16 is a corruption checksum, not a security control.** It has no
  key, provides no confidentiality and no sender authentication, and there is
  **no replay protection** — a captured `SET_DEST` replays forever.
* The control message carries **no sequence number or nonce**, and the client
  correlates responses on **the opcode byte alone**, so a stray or injected
  response to an earlier same-opcode request is accepted.
* Discovery broadcasts `uuid`, `caps` and `fw_ver` to the whole subnet.

**Suitable only for a physically controlled, trusted link.** Not suitable for any
untrusted network. Full inventory in [control_security.md](control_security.md).

---

## 5. Network and transport limits

| Limit | Detail |
| --- | --- |
| **IPv4 only** | No `AF_INET6`, no IPv6 anywhere in `src/`, `include/`, or `ports/`. |
| **No TSO/GSO** | No offload or segmentation offload. Every 1458-byte fragment is a separate `sendto`. |
| **No VLAN** | No 802.1Q tagging. |
| **MTU 1500 assumed** | `HML_DEFAULT_MTU` is 1500. The fragment payload is a fixed 1428 bytes. **No MTU discovery** — a path with a smaller MTU silently loses every frame unless you lower `max_frame_bytes`. |
| **Data plane is not confidential or authenticated** | Per-fragment CRC-16 only. Anyone on the path reads and can modify frames. |
| **No flow control** | Every overflow policy is drop-and-count. See [backpressure.md](backpressure.md). |
| **No IP fragmentation** | HML fragments at the application layer. |

---

## 6. Concurrency: **single-threaded per stream**

**The library is not thread-safe, and there is not a single mutex in it.**

* `hml_transport` is a fixed-capacity, register-once registry; the header says
  "v1: single-threaded per stream."
* The accel probe cache is "deliberately unlocked," to be probed once during
  single-threaded start-up.
* Each stream is owned by exactly one thread. Use one stream per thread, or
  serialise externally.

`pthread` appears only in `hml_bridge_demo.c`, the demo tool — not in the
library.

---

## 7. Protocol and reassembly limits

* **2 reassembly slots** (`HML_REASM_SLOTS`). A third concurrent stream drops
  the oldest in-flight frame. There is no way to configure this.
* **No non-uniform fragmentation.** All fragments but the last must share a
  payload length. v1 has no segment reassembly beyond that uniform case.
* **No pending-frame timeout.** A frame awaiting stragglers is held until the
  next frame's fragment proves the loss. On a stalled stream that is a retained
  buffer indefinitely.
* **Loss counters cannot distinguish causes.** `gaps` is raised by a dropped
  kernel datagram, real wire loss, late reordering, and MCU queue overflow
  alike. There are no per-cause counters.
* **No IPv4 header options, no DSCP, no fragmentation reassembly at the IP layer.**
* **Sequence space is 32-bit** and wraps. Correct with up to 2 in-flight frames,
  so wraparound is handled, but there is no extended timestamp for ordering.

---

## 8. Latency numbers: **loopback only**

The reference figures (`p50=150us p95=2750us p99=18224us`) are **loopback on
one machine.** Do not quote them for a two-machine or MCU deployment.

* No PTP, no NTP, no time sync. `hml_clock_us()` is per-device and the sender's
  `ts_us` is subtracted from the receiver's clock, so across two unsynchronised
  boards the number is dominated by clock offset. The code detects this and
  reports `skew_samples` / `verdict=TBD` rather than a fake green, but the
  honest answer for a two-board deployment is **"unmeasured."**
* **`p99` equals `max` in every observed run.** The 64-bucket, 50 µs histogram
  resolves only up to ~3150 µs; the tail is not resolved, so the reported p99 is
  a worst-case sample, not a percentile.
* The 40 ms `HML_LATENCY_BUDGET_US` gate is a **loopback smoke threshold on a
  shared VM**, not a hardware SLA. It is load-sensitive and does flake on a
  busy machine.

Full detail in [latency.md](latency.md) and [backpressure.md](backpressure.md) §4.

---

## 9. Explicit non-goals (from the plan)

Not started, not planned, and not to be inferred as oversights:

* Full ECB / BOOTP / hololink wire clone.
* Linking the **Holoscan SDK** into the MCU library.
* **AI on device.**
* **FPGA MIPI line-rate** on ESP/STM.
* **Safety certification.**
* **PTP grandmaster** (and see §8 — no time sync at all).
* **T2 hardware acceleration** as a shipped path: no NIC, no GPU, so it is
  deferred, not delivered.

---

## 10. Test and validation gaps

* **No soak.** The plan asks for a 72 h T0 soak and a 24 h T1a soak. Neither was
  run.
* **No ASAN/UBSAN pass** in this release's evidence.
* **No 1M+ malformed-packet fuzz run** as evidence (a fuzz harness exists in
  `tests/fuzz/`, but a 1M+ run is not on record).
* **No ABI symbol freeze check.**
* **No porting-guide dry-run by a second engineer.**
* **No dual-host / netns test.**
* **Three known load-sensitive test flakes** (`test_loopback` p99 budget,
  `test_roce_e2e` receive-buffer overflow, `test_bridge_lifecycle` discovery
  timeout) — see [ship_checklist.md](ship_checklist.md) §4.
* `docs/testing.md` and `docs/capability_matrix.md` are **stale** — they predate
  the current tree (they say 18 tests and "latency uninstrumented"). They belong
  to another track and were not edited here.

---

## 10a. Packaging and partitioning gaps

These are new, and they are gaps in the *distribution* of the library rather
than in the protocol.

* **`HML_NO_POSIX` is an install flavour, not a build switch.** It drops
  `port_posix` (and, because they link it, `control`, `udp` and `rocev2`) from
  both the shared arm and the export set, and stamps `HML_NO_POSIX` on the
  installed interface. It does **not** stop `ports/posix` being compiled: the
  in-tree host tools, adapters and tests are POSIX programs and link it. A build
  that never compiles socket code at all is `firmware/hml_object`, which never
  adds that directory. A `-DHML_NO_POSIX=ON` install is genuinely portless and
  consumable — verified with a consumer that supplies only the seven contract
  symbols and links no socket code.
* **Transports are not installed by default.** `-DHML_INSTALL_TRANSPORTS=ON`
  adds `hml_udp` and `hml_rocev2`. They are off because their registration
  entry points are not `HML_API`-decorated, so a `-fvisibility=hidden` shared
  object would export nothing a consumer can call.
* **A portless install cannot carry the transports.** The static arm drops them
  under `HML_NO_POSIX` because an export set that references a target which does
  not exist is rejected at generate time. Embedded firmware that wants UDP gets
  it through `firmware/hml_object`, which is the supported path.
* **The generated port-contract object can be discarded** by a linker using
  `--gc-sections`, since it is unreferenced data. Force it with
  `-u hml_port_contract_check`.
* **`hml_firmware_contract_source` is a cache variable**, published from
  `hml_firmware_add()` so it survives the function boundary. A project adding
  `firmware/hml_object` twice, or expecting a normal variable, will be surprised.
* **Not installed at all**, host-side and deliberately so: `hml_bridge`,
  `hml_accel`, `hml_host`, `hml_roce_demux`, `hml_roce_gen`, `hml_synth`, the
  FreeRTOS/ESP ports, and every host tool.
* `docs/testing.md` and `docs/capability_matrix.md` remain stale; they belong to
  another track.

---

## 11. If you only remember five things

1. **Not a Holoscan drop-in.** No BOOTP/ECB/hololink/`DataChannel`. Use `hml_host`.
2. **RoCEv2 is unvalidated on hardware.** Self-consistent on loopback only.
3. **Does not fit a small MCU.** 2 MiB heap allocation, no static alternative.
4. **Control plane is open.** No auth, no encryption, trusted link only.
5. **Latency is loopback-only**, and `p99` is really `max`.

---

## See also

* [ship_checklist.md](ship_checklist.md) — per-item verified/BLOCKED status.
* [hololink_compat_notes.md](hololink_compat_notes.md) — the interop gap in detail.
* [control_security.md](control_security.md) — the trust model.
* [backpressure.md](backpressure.md) — overflow policy and loss counters.
* [porting.md](porting.md) — bringing it to a new target.
* [final_partitioning.md](final_partitioning.md) — the component split and the seven-symbol port contract.
* [gpu_host_pipeline.md](gpu_host_pipeline.md) — where the host path hands you a hash instead of the bytes.
* [latency.md](latency.md) — the latency definition.
