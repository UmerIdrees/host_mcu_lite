# Prior art — what was taken, what was not, and why

Reference notes for `holoscan_mcu_lite`. The plan calls this directory
"notes linking Emulator + NXP RT1180 demos"; this file does that and adds the
negative decisions, which matter more than the positive ones for a project whose
main risk is accidentally becoming a clone of something it was meant to be
inspired by.

Companion documents: [holoscan_mcu_lite_Plan.md](../docs/holoscan_mcu_lite_Plan.md)
(the Emulator/SDK appendix and the v1 non-goals),
[hololink_compat_notes.md](../docs/hololink_compat_notes.md) (the M6b / Path-B
gate), [protocol_v1.md](../docs/protocol_v1.md) (what the wire actually looks
like after all these decisions).

## Sourcing rule for this file

Only two URLs appear in the approved plan, and only those two are used here:

* HSB Emulator — <https://docs.nvidia.com/holoscan/sensor-bridge/latest/emulation.html>
* holoscan-sensor-bridge — <https://github.com/nvidia-holoscan/holoscan-sensor-bridge>

Projects that the plan names but gives no URL for — the Holoscan SDK receive
operators, the NXP RT1180 demos, and `rdma_rxe` — are discussed **by name only**.
Where this pass did not independently open a source, the entry says so and is
limited to what the plan already records. No URL has been guessed or
constructed.

## Summary

| Prior art | Taken | Deliberately not taken | Where it landed |
| --- | --- | --- | --- |
| **HSB Emulator** | Sensor-agnostic framing; `send(tensor, metadata)` send shape; **socket-crafted RoCEv2**; pluggable `DataPlane` vtable; POSIX-reference + MCU-port split; STM32 control-first precedent | ECB/BOOTP wire clone; emulator-only build shape; the vendor control protocol | M0–M3, `include/hml/hml_protocol.h`, `src/transports/`, `ports/` |
| **holoscan-sensor-bridge** | Discovery/control/data as three separate planes; the "STM32 = control plane only" reality | The full sensor-bridge control surface; NVIDIA vendor fields | `src/control/`, M2, M5 stubs |
| **Holoscan SDK** | `DataChannel` shows the *fields* a receiver must be programmed with; EOF framing must be decided in M0 not M1; `LinuxReceiverOperator` is the CPU-fallback shape | Linking the SDK; depending on hololink `DataChannel`; claiming `RoceReceiverOp` drop-in | Framing decision in M0; deferred to M6b |
| **NXP RT1180 demos** | A second data point that MCU-class parts run the control plane, not a dataplane | An MCU dataplane commitment | M5 stubs stay stubs |
| **`rdma_rxe` (Soft-RoCE)** | That a verbs CQ/MR correctness lab is worth having | Any claim that it is the T1 path, or a ConnectX performance proxy | M3b, `host/soft_roce/` |

---

## 1. NVIDIA HSB Emulator

<https://docs.nvidia.com/holoscan/sensor-bridge/latest/emulation.html>

The most influential input, and the source of the plan's single most important
correction.

### Taken

| Emulator fact | What we did |
| --- | --- |
| Sensor-agnostic: sensors are `I2CPeripheral`s driven by an application loop, not baked into the core | `hml_sensor_t` is a three-function vtable (`open` / `acquire` / `release`). No sensor in `src/core`. Confirmed structurally by `test_core_no_ifdef`. |
| `DataPlane::send(DLTensor, FrameMetadata*)` — a synchronous send of a buffer plus optional metadata | Shaped `hml_frame_send` / `hml_frame_flush_eof`. The consequence that matters: **flushing is a first-class operation**, not an afterthought, and it got its own wire case (the EOF-only flush). |
| **`RoCEv2Transmitter` is Linux sockets crafting RoCEv2 over UDP** | This reordered the whole plan. Socket-crafted RoCEv2 became the **primary T1 path** (T1a), and Soft-RoCE was demoted to an optional lab. It is now `src/transports/rocev2/hml_rocev2.c` — hand-built IP/UDP/BTH/RETH/ICRC, no libibverbs, no ConnectX. |
| `BaseTransmitter` / `DataPlane` are pluggable | `hml_xport_ops` vtable; `hml_udp_register()` / `hml_roce_register()`; a transport name registry. |
| Emulator-only build, with Linux and STM32 HAL ports | Same shape: `ports/posix` as the real reference, `ports/freertos_lwip` and `ports/esp_idf` as ports only. |
| STM32 is control-plane only today | Accepted outright. It is why M5's exit criterion is "same API on ≥2 OS classes" and not "MCU dataplane throughput". |
| Loopback / netns testing before multi-machine | Same: all 18 tests are same-host loopback; netns is where M3b would start. |

### Not taken, and why

| Not taken | Why |
| --- | --- |
| **ECB / BOOTP wire clone** | Explicit v1 non-goal. Our control plane is a 12-byte header and 11 opcodes; hololink's is BOOTP plus a register block. Cloning it imports a layout we would have to mirror forever, to serve a receiver we do not own. Deferred to M6b behind a gate. |
| **`DataChannel` packetizer register programming** | Our `hml_dest_cfg` is a self-describing 40-byte wire struct. A register-mapping layer is a translation table that must track Holoscan releases forever. |
| **The emulator-only build shape** | We ship a library plus six tools, not an emulator. The "one buildable target per platform" discipline was kept; the "only an emulator" restriction was not. |
| **The BOOTP `NVDA` vendor field** | Reserved for NVIDIA devices. Filling it in as a non-NVIDIA device is both a misuse and a misrepresentation. |
| **The device's own control opcodes** | `PING`, `SET_DEST`, `GET_CAPS`, `POKE`, `READBACK` are a small, obvious set for a sensor with a memory-mapped register file. We defined our own with a different shape (fixed 12-byte header, 512-byte payload cap, opcode-correlated responses) and learned from the *design*, not the bytes. |
| **`FrameMetadata` as a struct with a DLTensor payload** | That is a Python/TVM-shaped API. In C the useful part is the *idea* that a frame boundary is a distinct signal from the payload, which is why `HML_FLAG_END` exists at all. |

---

## 2. holoscan-sensor-bridge

<https://github.com/nvidia-holoscan/holoscan-sensor-bridge>

### Taken

| Fact | What we did |
| --- | --- |
| Discovery, control and data are three separate concerns with separate ports | Three planes with three ports: 55100 beacon, 55102 control, 55101 data. It keeps a malformed control datagram from ever touching the reassembler, which is a real robustness property, not tidiness. |
| MCU-class parts run control, not data | M5 is honest about being unmeasured rather than claiming an MCU number. |
| The peripheral/lifecycle shape (init → configure → stream) | `hml_bridge` state machine `INIT → CONFIG → STREAM`, and the 7-step M4 lifecycle in `hml_bridge_demo --lifecycle`. |

### Not taken, and why

| Not taken | Why |
| --- | --- |
| The full sensor-bridge control surface | A discover → configure → start → poke → stop vocabulary is enough for a sensor bridge. Importing a larger surface before anything needs it would be speculative. |
| NVIDIA-specific discovery and vendor fields | See §1. Our beacon is our own 28 bytes; it does not pretend to be hololink's. |
| Any dependency on the Python/TensorFlow/TensorRT sensor stack | This is a C library. `hml_sensor_t` is three function pointers; a Python binding is out of scope and unclaimed. |

---

## 3. Holoscan SDK receive paths

`RoceReceiverOp`, `LinuxReceiverOperator`, `DocaRoceReceiverOp`. Named in the
plan's appendix; no URL was recorded in the plan for the SDK documentation, and
none is invented here. This entry is limited to the SDK facts the plan already
captures.

### Taken

| SDK fact | What we did |
| --- | --- |
| `RoceReceiverOp` wakes on **end-of-frame** UDP, with the data already resident in GPU memory via ConnectX | **The most important consequence in the project.** Because a stock receiver is triggered by an end-of-frame event, frame boundaries had to be defined in **M0**, before any transport existed. They were: `HML_FLAG_END` plus the EOF-only flush for T0, and the metadata record for T1a. A project that deferred this would have had to redesign its data plane after writing it. |
| No ConnectX → `LinuxReceiverOperator` does a CPU UDP → GPU copy | `hml_host` is exactly this shape: CPU reassembly, then the application copies. It is the fallback that keeps the project useful on a machine with no NIC. |
| `DataChannel` programs dest MAC/IP, QP, R_Key, GPU address, frame size | Our `hml_dest_cfg` (40 bytes: IP, port, QP, PSN, rkey, remote address, frame size, window length, hop limit) is the same **information**, chosen because T1a needs it on the wire. This is where the field list came from. |
| Holoscan pipelines are operators; hololink ≠ SDK | v1 ships `hml_host`. A Holoscan operator or a hololink shim is explicitly optional and deferred. |

### Not taken, and why

| Not taken | Why |
| --- | --- |
| **Linking the Holoscan SDK** | Explicit v1 non-goal. An MCU-side sensor library must not depend on a GPU pipeline framework. |
| **Depending on hololink `DataChannel`** | It would make M6b a prerequisite for the whole product instead of an option. |
| **Claiming `RoceReceiverOp` drop-in compatibility** | It would be false. The data plane is ours, but the receiver expects hololink to have programmed it over BOOTP + ECB, and our T1a EOF is a write to a metadata slot past the window, which a stock receiver has no way to recognise as a boundary. Both are analysed in [hololink_compat_notes.md](../docs/hololink_compat_notes.md) §5. |
| **Destination MAC** | `hml_dest_cfg` carries IPv4, not a MAC. RoCEv2 is routable IP, so L2 addressing is the NIC's problem, not ours. This is a deliberate simplification versus a same-subnet `DataChannel` config. |

---

## 4. NXP RT1180 HSB demos

Named in the plan as prior art; no URL was recorded in the plan and this pass
did not independently review them, so the entry is limited to the role the plan
assigns them.

### Taken

One thing, and it is worth taking: a **second, independent data point that
MCU-class parts run the sensor-bridge control plane rather than a dataplane.**
Two projects saying this makes it a pattern rather than an Emulator quirk, and
it is why M5's exit criterion is "same API on ≥2 OS classes" with a
control-only MCU smoke allowed to pass.

### Not taken, and why

| Not taken | Why |
| --- | --- |
| An MCU dataplane commitment | Both references point the same way: the dataplane on an MCU is not where the value is. `ports/freertos_lwip` and `ports/esp_idf` are API-complete stubs, and M5 is recorded as "builds, unmeasured" rather than quietly claiming a throughput. |
| A hard real-time latency budget | Aiming a dataplane at an MCU would force a latency target on a class that cannot meet one. |

---

## 5. Soft-RoCE / `rdma_rxe`

The plan's most firmly-held position, and the one that took the most
discipline: **Soft-RoCE is not how HSB Emulator or Holoscan Linux receive
work.** Emulator crafts RoCEv2 over sockets; Holoscan uses `RoceReceiverOp`
with a ConnectX or `LinuxReceiverOperator` with kernel UDP. `rdma_rxe` appears
nowhere in either path.

### Taken

That a verbs CQ/MR correctness lab is worth having at all — it exercises
queue-pair setup, memory registration and completion queues, which is code the
socket path never runs. Hence `host/soft_roce/` and M3b.

### Not taken, and why

| Not taken | Why |
| --- | --- |
| **The T1 path** | T1a is the socket emitter. Making `rdma_rxe` the main path would have contradicted the Emulator model, which is the thing that actually works on real hardware. |
| **Any performance claim** | The plan says it outright: "not a perf proxy for ConnectX". `rxe` is a software device; its numbers are not a NIC's numbers. |
| **A CI gate** | M3b exits **77** (skip) when the environment cannot run it, so it can never fail CI by accident. The default suite contains only `roce_lab_skip_path`, which proves the lab is correctly gated off. The lab itself has never been run — see [testing.md](../docs/testing.md) §3.1. |

---

## 6. Consolidated: what is ours alone

Decisions that came from no prior art, listed so they are reviewable as ours:

| Decision | Why it is ours |
| --- | --- |
| **The EOF-only flush** — a zero-payload fragment carrying only `HML_FLAG_END` | Nothing in the references required it. It came from the observation that a consumer may need waking without a data frame, and it is now a first-class wire case. |
| **`base = seq - frag_idx` frame identity in reassembly** | Derived from the fact that we allocate one `seq` per *fragment*. It makes a lost first fragment harmless and wraps cleanly at 2^32, and it is what stops two frames silently merging when both boundaries are lost. |
| **The 48-bit `ts_us` in the 28-byte beacon** | The frozen header comment said `u64`, which would have made the record 30 bytes. A fixed 28-byte record with a CRC covering exactly the first 26 bytes won, at the cost of 2^48 µs ≈ 8.9 years of uptime. |
| **The 20-byte T1a metadata record at `remote_addr + frame_size`** | Deriving the EOF slot from `hml_dest_cfg` alone, so the receiver needs no out-of-band state and no extra control message. The trade-offs are real and **unproven against a real receiver** — see [testing.md](../docs/testing.md) §8. |
| **Opcode-correlated control responses** | A consequence of freezing a 12-byte header with no sequence field. It forces a strictly serialised control plane; the alternative was a wider header. |
| **Rejecting non-zero reserved bytes** | Makes a future v2 that reuses a reserved field a loud protocol error on a v1 peer instead of a silent misread. |
| **`test_core_no_ifdef` as a test** | The M5 portability criterion as an executable assertion, rather than a review comment. |

---

## 7. Gaps in this review

Stated so nobody treats this file as a complete survey:

* The **Holoscan SDK** receive operators and the **NXP RT1180** demos were not
  independently opened in this pass. The plan's appendix is the source, and both
  are cited by name without a URL because none was recorded.
* **No packet capture** of the Emulator, or of anything else, exists. The plan
  lists pcap samples per tier as golden artifacts; the `docs/` tree has none.
  The T1a layout was derived from the described transmitter model and pinned
  against our own golden vectors, which proves self-consistency and not
  agreement with the Emulator.
* The two T1a-specific choices — BTH opcode `0x2A` (UC) where a plain RDMA
  Write is normally `0x0A` (RC), and the 20-byte little-endian metadata record
  — are the places where our reading of the Emulator is least certain. They are
  cheap to change and expensive to discover wrong.

---

## 8. Sources

As recorded in the approved plan; no other URL is cited anywhere in this
repository's documentation.

* NVIDIA HSB Emulator — <https://docs.nvidia.com/holoscan/sensor-bridge/latest/emulation.html>
* NVIDIA holoscan-sensor-bridge — <https://github.com/nvidia-holoscan/holoscan-sensor-bridge>

Cited by name only, with no URL recorded in the plan: the Holoscan SDK receive
operators `RoceReceiverOp` / `LinuxReceiverOperator` / `DocaRoceReceiverOp`,
the NXP RT1180 HSB demos, and the kernel's `rdma_rxe` Soft-RoCE driver.
