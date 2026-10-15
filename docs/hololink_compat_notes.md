# hololink / HSB Emulator control-interop notes (M6b, Path B)

**Status: M6b is not started. Nothing described in this document is
implemented.** This file exists so that the decision to *not* implement it is
recorded deliberately, with the reasoning, rather than being discovered later as
an omission. It is the scope note for the plan's optional M6b milestone and for
"Path B" in the architecture diagram.

Read alongside [holoscan_mcu_lite_Plan.md](holoscan_mcu_lite_Plan.md) (the
M6b section, the architecture table and the kill criteria) and
[testing.md](testing.md) §8.

---

## 1. The question this answers

> We already emit RoCEv2 that a ConnectX could carry. Why can't a stock
> Holoscan app just receive it?

Because **the data plane and the control plane are negotiated separately, and
only the data plane is ours.** A Holoscan operator is not a passive packet
reader: the receiver's registers have to be programmed by hololink before the
first frame, and hololink expects to do that over its own wire protocol. Our
device speaks our own control protocol. So the question is not "are our packets
valid" but "does anything on the far side know how to program itself to accept
them, and will it accept them without being told".

The short answer, and the plan's own "critical correction": **no. RoCEv2 packets
alone do not unlock stock `RoceReceiverOp`.**

---

## 2. The two paths

| | **Path A (default, shipped)** | **Path B (optional, deferred)** |
| --- | --- | --- |
| Host receiver | `hml_host` / `hml_host_roce`, in this repo | Stock Holoscan `RoceReceiverOp`, `DocaRoceReceiverOp` or `LinuxReceiverOperator` |
| Discovery | Our 28-byte UDP beacon | hololink BOOTP with the `NVDA` vendor field |
| Configuration | Our control plane: 12-byte header, 11 opcodes, `hml_dest_cfg` | hololink ECB / `AddressMemory` + packetizer registers |
| Frame delivery to GPU | CPU reassembly, then the app's own copy | GPU-resident, no host bounce |
| What must be written | Done | BOOTP vendor, ECB subset, `DataChannel` register programming |
| Who programs the receiver | We do, over our protocol | hololink does, over its protocol |
| Today | Implemented and tested (M0–M4) | Not started |

The device data plane is **identical in both paths**: T0 framed UDP, or the same
socket-crafted RoCEv2 UC WRITE + EOF metadata record. Path B does not require
changing a single byte of the wire formats in
[protocol_v1.md](protocol_v1.md). It requires making a *third party* willing to
talk to us.

Sources: [HSB Emulator](https://docs.nvidia.com/holoscan/sensor-bridge/latest/emulation.html)
and [holoscan-sensor-bridge](https://github.com/nvidia-holoscan/holoscan-sensor-bridge),
as reviewed in the plan's Emulator/SDK appendix. The Holoscan SDK receive paths
(`RoceReceiverOp`, `LinuxReceiverOperator`, `DocaRoceReceiverOp`) are cited by
name in the plan; no URL for the SDK documentation was recorded in the plan, so
none is invented here.

---

## 3. What stock Holoscan receive actually requires

Taken from the SDK facts recorded in the plan's appendix:

| Stock component | What it needs from a peer | Do we have it? |
| --- | --- | --- |
| `RoceReceiverOp` | Wakes on **end-of-frame** UDP with the data already in GPU memory via ConnectX | We have a *weird* EOF signal: an RDMA write to a metadata slot past the window, not an EOF the SDK recognises. See §5. |
| `DocaRoceReceiverOp` | GPU-resident path driven by hololink `DataChannel` | No |
| `LinuxReceiverOperator` | Plain kernel UDP, no NIC — the CPU fallback | Closest to Path A, but still needs the sender's framing to match what its operator expects |
| `DataChannel` | Programs dest MAC/IP, QP, R_Key, GPU address, frame size into packetizer registers | No — our `hml_dest_cfg` is a wire struct, not a register write |
| hololink enumerator | BOOTP; reads a vendor field (`NVDA`) for device capabilities | No — we broadcast our own beacon |

The pattern is consistent: **every stock receiver component expects hololink to
have configured it first.** That is the whole cost of Path B.

---

## 4. What M6b would actually require

Scoping only. Nothing here is designed or built.

1. **BOOTP responder.** Answer the hololink enumerator's BOOTP request and fill
   in the `NVDA` vendor field with our UUID, capability bits and firmware
   version. Our existing 28-byte beacon carries exactly the same three values,
   so the *data* exists; the *protocol* and its field layout do not.
2. **ECB subset.** The Event Control Block is the register window a peer pokes
   to drive the receiver lifecycle. We would need at least enough of it to
   expose the packetizer address space, a reset/flush, and the config area.
3. **`DataChannel` equivalent.** Serialise the window description into the
   packetizer register layout the SDK expects: destination MAC and IP, QP,
   remote key, GPU buffer address, frame size. Our `hml_dest_cfg` is the right
   *content* in a different *shape*; mapping it onto the expected register
   offsets is real, version-specific work against a layout we do not control.
4. **A frame-delivery story the SDK accepts.** This is the hard one — §5.
5. **Per-SDK-version maintenance.** Register layouts are not a stable public
   contract. M6b is not a one-off; it is an ongoing compatibility tax tied to
   Holoscan releases.
6. **A reference peer to test against.** M6b's pass criterion in the plan is
   "an Emulator or hololink example runs without code forks". That needs either
   the HSB Emulator or a stock hololink/SDK host to test against, neither of
   which this project has.

Cost is dominated by items 3, 4 and 6, and item 5 is the ongoing liability.

---

## 5. The frame-delivery problem, stated honestly

Even with BOOTP, ECB and `DataChannel` done, our T1a EOF signalling is a
problem. This is the single most important thing to know about M6b.

**Our EOF is not an EOF.** In T0, a frame ends where a fragment with
`HML_FLAG_END` ends — a real, visible, in-band boundary the receiver can act
on. That is what `RoceReceiverOp` waking on end-of-frame expects.

In T1a there are no flags on the wire. A frame is closed by a dedicated 20-byte
RDMA WRITE to `remote_addr + frame_size`, a slot past the data window, and the
receiver is expected to interpret that address as "the frame is complete". The
address is derived purely from `hml_dest_cfg`, so our own demux recognises it
with no extra state. **A stock SDK receiver has no way to know that address is
special.** It would write those 20 bytes into the buffer as payload, or
fault.

So Path B is not blocked only on control-protocol work. Even with perfect BOOTP
and ECB, an unmodified `RoceReceiverOp` would not frame our stream correctly.
Fixing that means either changing our T1a EOF to an in-band signal the SDK
accepts, or requiring a modified receiver — and a modified receiver is not
"without code forks", which is the entire point of Path B.

This is a stronger argument for deferral than "M6b is a lot of work".

---

## 6. Decision gate

M6b stays deferred unless **all** of these are true. They are ordered so that
the cheapest disqualifiers come first.

| # | Condition | Why it is a gate |
| --- | --- | --- |
| 1 | There is a **concrete stock Holoscan application** that must consume this sensor unmodified | The plan's motivation for Path B is reuse, not interoperability as an end in itself. Without a named consumer, M6b is speculative. |
| 2 | A reference peer exists to test against — the HSB Emulator, or a stock hololink host | Otherwise there is no way to satisfy the pass criterion, and unregistered register-offset work cannot be validated. |
| 3 | T1a interop is **already validated against a real receiver** ([testing.md](testing.md) §8 item 1) | Chasing control interop while the data plane is unproven against any independent implementation risks debugging two unknowns at once. Do the cheap data-plane check first. |
| 4 | The EOF problem in §5 has a solution that keeps the receiver unmodified | If the answer is "the receiver must be modified", Path B is not Path B. |
| 5 | The ongoing Holoscan-version maintenance cost is explicitly accepted | This is a recurring cost, not a milestone. |
| 6 | M7 is otherwise complete | M6b after the ABI freeze would force an ABI change. Better before, but not at the cost of M7. |

If condition 1 cannot be answered with a named application, the answer is no,
and this document needs no further work.

---

## 7. What was deliberately **not** taken from hololink / HSB Emulator

Recorded so a future reader does not mistake an omission for an oversight. The
full comparison across all prior art is in
[../refs/prior_art.md](../refs/prior_art.md); the M6b-specific parts:

| Not taken | Why |
| --- | --- |
| **BOOTP discovery** | We ship our own 28-byte beacon over UDP 55100. A second discovery protocol would be a second source of truth for the same three values (UUID, caps, firmware). Keep one. Revisit only if gate §6 condition 1 fires. |
| **ECB register model** | Our peripheral model is a set of typed windows with explicit read/write errors, not a flat register block. An ECB clone would import a layout we would then have to mirror forever, for a receiver we do not own. |
| **`DataChannel` packetizer programming** | Our `hml_dest_cfg` is one 40-byte wire struct, self-describing and versioned. A register-mapping layer would be a translation table that must track Holoscan releases. |
| **A full ECB/BOOTP/hololink wire clone** | Explicitly a v1 non-goal in the approved plan. A clone is a permanent maintenance liability and does not make our data plane any better; it only makes our *control* plane resemble someone else's. |
| **The Emulator's build shape** | Emulator is an emulator-only build; ours is a library plus tools. The port strategy (POSIX reference + MCU ports) is the same idea, but we are not shipping an emulator. |
| **Holoscan SDK as a dependency** | Explicitly a v1 non-goal. The MCU library must not link a GPU pipeline framework. Any interop is host-side only. |
| **BOOTP's `NVDA` vendor field** | Reserved for NVIDIA devices. Writing it as a non-NVIDIA device is a misuse that would also require pretending to be someone we are not. |

---

## 8. What *was* taken, and is already in the tree

So this is not read as "M6b research was wasted". Three things came out of the
Emulator/SDK review and **are** implemented:

1. **`RoCEv2Transmitter` is Linux sockets, not verbs.** The plan's most
   consequential correction. It reordered T1: socket-crafted RoCEv2 is the
   *primary* T1 path, and Soft-RoCE is a secondary lab that can never be an
   acceptance gate. See [protocol_v1.md](protocol_v1.md) §7 and
   [testing.md](testing.md) §3.1.
2. **EOF/metadata semantics had to be defined in M0, not later.** The SDK
   receiver wakes on end-of-frame, so a project that leaves frame boundaries
   undefined until after the data plane is already too late. We defined
   `HML_FLAG_END`, the EOF-only flush and the T1a metadata record before any
   transport existed.
3. **`send(tensor, metadata)` shaped the API.** A synchronous
   buffer-plus-optional-metadata-flush send became `hml_frame_send` /
   `hml_frame_flush_eof`, which is why flushing is a first-class operation
   rather than an afterthought.

Also carried over from the review, and visible in the milestone statuses:
the Emulator's STM32 story is **control-plane only**, which is why
[holoscan_mcu_lite_Plan.md](holoscan_mcu_lite_Plan.md) accepts an MCU port that
compiles and serves control without a dataplane, and why M5 is honest about
being unmeasured rather than claiming an MCU throughput number.

---

## 9. Bottom line

Path A ships and works today. Path B requires writing a third party's
configuration protocol, matching a register layout we do not control, keeping it
in step with Holoscan releases, and — as §5 shows — solving an EOF-signalling
problem that would stop an unmodified receiver from framing our stream even after
all of that is done.

**Recommendation: keep M6b deferred.** Revisit only against the §6 gate, and
only with a named application that needs it. Until then, the correct claim for
this project is: *RoCEv2 wire-compatible data plane, own control plane, Path A
host receiver.* Any claim of `RoceReceiverOp` drop-in compatibility would be
false today.
