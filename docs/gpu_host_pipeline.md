# From a sensor to a GPU: what hml actually gives you

**Status: design note. There is no GPU code, no CUDA dependency and no
GPU benchmark anywhere in this repository, and nothing below should be read as
a performance claim.**

## 1. The claim being made, and the one not being made

The project has a working C data path from an embedded sender to a host
receiver, with CRCs, sequence numbers, reassembly and latency accounting. This
document describes how a GPU consumer would attach to that path, and — more
usefully — exactly where the current code stops.

What is **not** claimed:

| | |
|---|---|
| No GPU is involved | No CUDA is linked, and no device memory is ever touched. `src/accel/` mentions CUDA only to *refuse* it: `hml_accel_open(HML_ACCEL_GPU)` returns `HML_ERR_NOTSUP` on every build, because registering GPU BAR1 would need the CUDA driver API (see `src/accel/hml_accel.h`). There is no `cudaMalloc` and no pinned memory anywhere. |
| No throughput or latency number | Nothing here has been measured on a GPU. Any number would be fabricated. |
| No GPUDirect RDMA | GPUDirect is a function of the NIC, driver, GDRCOPY and PCIe topology. `hml_accel` probes for verbs and `nvidia-peermem` and deliberately reports "not present" either way. Nothing here establishes whether GPUDirect works, let alone how fast. |
| The host path is CPU-only | `host/hml_host` is a POSIX C program. Its receiver hands a **hash** to its callback, not the bytes. See section 3. |

## 2. The path that exists today

Two receivers, both real, both in this tree.

### 2.1 UDP T0 — `host/hml_host`

```
sender (hml_stream_send_frag)
  -> hml_udp_send_frag            src/transports/udp/hml_udp.c
  -> hml_sock_send               ports/posix/hml_port_posix.c
  -> wire: HML header | payload | CRC16

receiver (host/hml_host/hml_host.c)
  -> hml_sock_recv               one datagram, <= HML_MAX_FRAG_SIZE
  -> hml_reasm_feed_datagram     magic/version/header-CRC, seq + frag ordering,
                                  gap detection, payload CRC16
  -> hml_reasm EOF callback      (stream_id, frame_seq, frame, frame_bytes)
  -> host_on_frame               counters + hml_fnv1a32(frame, frame_bytes)
  -> hml_host_frame_cb           (stream_id, frame_seq, frame_bytes, hash)
```

`hml_host_poll()` blocks for at most `timeout_ms` on the first datagram, then
drains the socket queue with zero-timeout reads. The reassembler is
single-threaded and owns its buffer; the callback fires from inside
`hml_reasm_feed_datagram`, so a slow callback stalls that receiver. There is one
socket, one reassembler, one stream — v1 is thread-affine by design.

### 2.2 RoCEv2 T1a — `host/roce_demux`

`hml_roce_rx_open()` / `hml_roce_rx_poll()` take an `hml_roce_eof_cb` of
`(ctx, frame_seq, frame, n, crc32)`. This callback **does** receive the frame
bytes, and the receiver copies payload into a window it owns, tracking
`psn_gaps` and `meta_drops`. T1a is socket-crafted: it builds BTH/RETH/ICRC in
user space and never touches libibverbs.

### 2.3 What the data is

An HML frame is `HML_FRAME_HDR_SIZE` bytes of big-endian header, a payload of
at most `HML_MAX_FRAG_PAYLOAD` per datagram, and a 2-byte CRC16 over the
payload. A logical frame is the reassembly of all fragments sharing a
`stream_id`/`seq` run, up to `HML_MAX_FRAME_SIZE` (1 MiB). Payload is
**uninterpreted**: hml moves bytes and checks their integrity, and has no idea
what a sensor produced them.

## 3. The gap: `hml_host` does not hand you the bytes

This is the single most important fact for anyone planning a GPU consumer, and
it is easy to miss because the reassembly layer clearly has them.

`hml_reasm`'s EOF callback receives `const uint8_t *frame, size_t frame_bytes`.
`hml_host` sets that callback to `host_on_frame`, which computes a hash and
then calls the public `hml_host_frame_cb`:

```c
typedef void (*hml_host_frame_cb)(void *ctx, uint16_t stream_id, uint32_t frame_seq,
                                  size_t frame_bytes, uint32_t hash);
```

There is no `const uint8_t *` in that signature. The bytes exist in
`host_on_frame`'s `frame` parameter and are discarded when it returns. So:

- `hml::hml_core` + `hml_reasm` directly → **you get the bytes.** This is the
  supported route, and it is what `examples/firmware_dropin/linux/` does.
- `hml_host` → you get counters and an FNV-1a hash. Enough to verify a stream is
  intact and in order; not enough to look at a frame, let alone process one.

That distinction is a design decision with a rationale — the hash is
deliberately cheap, and `hml_host` is a receiver/monitor, not a data sink — but
it means **`hml_host` is the wrong place to attach a GPU consumer today.** A
consumer should own its own `hml_reasm` instance, exactly as the Linux example
does, and hand the frame straight to its own code from the EOF callback.

## 4. What a GPU consumer would have to add

In rough dependency order. None of this exists yet; this is the honest list.

### 4.0 The component that already declines to help

`src/accel/` is the existing GPU-adjacent layer, and its current state is a
deliberate refusal rather than an implementation. `hml_accel_open(HML_ACCEL_GPU)`
returns `HML_ERR_NOTSUP` on every build, including on a machine with
`nvidia-peermem` loaded, because registering GPU BAR1 would require linking the
CUDA driver API. What it does provide is `HML_ACCEL_HOST_MR`: an ibverbs
device/PD with a registered host memory region, when CMake finds both
`<infiniband/verbs.h>` and libibverbs. That is the natural place to hang a
zero-copy receive path, and its own header is careful to say that a registered
region is not a ConnectX data path and a loaded peermem module is not proof of
peer capability. `docs/capability_matrix.md` keeps every such claim at "not
present" until someone measures it on real hardware.

### 4.1 A receiver that surfaces the bytes

Either use `hml_reasm` directly (section 3), or extend `hml_host_frame_cb` with
a `const uint8_t *`. The second is a breaking change to a published API, so it
is more likely to arrive as a new callback alongside the old one. Either way
the frame pointer is **valid only for the duration of the callback** — it points
into the reassembler's buffer and is reused on the next frame. A GPU consumer
must either copy or submit within the callback, never retain the pointer.

### 4.2 A device-transfer step

Three options, none of them free:

| Approach | Cost | Note |
|---|---|---|
| `cudaMemcpy` from the reassembly buffer | one full copy per frame through pageable memory | simplest; a copy is still a copy |
| `cudaHostRegister` the buffer, then `cudaMemcpy` | register/unregister churn, or pin permanently | pinning the reassembly buffer once is plausible and cheap-ish, but this repo has not measured it |
| GPUDirect RDMA | no host bounce at all | requires an RDMA NIC, GDRCOPY, and a receive queue writing into device memory. **Nothing in this repo tests any of that.** |

The reassembly buffer is a `malloc`'d `hml_max_frame` region. Pinning or
registering it is the natural thing to try first, but the frame pointer moves
per frame and the buffer is reused, so the registration would have to cover the
whole buffer and the consumer would still need to tell the device where in it
the frame landed.

### 4.3 Keeping up, or not

hml reassembles on a single thread with no queue between reassembly and the
callback. A GPU submit that takes longer than the inter-frame interval will
back-pressure into the socket receive path, and UDP will drop. Whether a given
pipeline can absorb that depends entirely on the sensor rate, which this
project has no opinion about. `docs/backpressure.md` covers the stream side;
nothing here measures the receive side against a device.

### 4.4 Ordering and integrity, for free

Whatever a GPU consumer does, two properties are already handled upstream and
should not be re-implemented: a corrupt datagram is **dropped, never
delivered** (magic, version, header CRC and payload CRC16 are all checked
before a callback fires), and out-of-order fragments are detected as a gap via
`hml_reasm_stats.gaps`. A GPU consumer gets whole, CRC-checked, in-order
frames — or a counter that says one went missing.

The integrity guarantee is over the **wire bytes only**. It says a frame arrived
intact; it says nothing about whether the sensor produced sensible values.

## 5. How to try it today, honestly

Without writing a line of CUDA, the loop is: own a `hml_reasm`, feed it
datagrams from `hml_sock_recv`, and do whatever you like in the EOF callback.
`examples/firmware_dropin/linux/linux_main.c` is ~250 lines of exactly that,
including the frame layout and the fail-closed corrupt-frame check. Copy it as a
starting point rather than using `hml_host` and being surprised by the hash.

To see the reassembler's own view of stream health:

```c
hml_reasm_stats st;
hml_reasm_get_stats(reasm, &st);
/* st.frames_ok, st.frames_dropped, st.frags_recv,
   st.gaps, st.crc_fail, st.proto_fail */
```

And the EOF-latency histogram, which is the one performance-shaped number hml
does measure, and measures on the CPU:

```c
hml_latency lat;
hml_reasm_get_latency(reasm, &lat);
```

Treat that histogram as meaningless if the sender's clock and the receiver's are
different domains; `hml_reasm` records those samples as skew and discards them.
See `docs/latency.md`.

## 6. Related reading

- `docs/final_partitioning.md` — the component split and the port contract.
- `docs/soft_roce_lab.md` — the Soft-RoCE lab, and why it says nothing about
  GPUDirect or about a real NIC.
- `docs/protocol_v1.md` — the wire format, header layout and CRC coverage.
- `docs/limitations.md` — the project's own list of what is not finished.
- `docs/backpressure.md` — stream-side flow control.
