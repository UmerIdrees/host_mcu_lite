# Backpressure, overflow policy and the latency budget

What the send and receive paths actually do when their buffers fill, read from
the code in `src/`, plus the `hml_latency` histogram and the 40 ms budget.

Short version: **there is no flow control.** Every overflow policy in this
library is *drop and count*. Nothing blocks, nothing retries, nothing slows the
sensor down, and nothing tells the sender. The control plane has no
backpressure-related opcode.

---

## 1. Send path

### 1.1 What happens on a full socket buffer

The only backpressure signal the send path has is `HML_ERR_AGAIN`.

`hml_sock_send` (`ports/posix/hml_port_posix.c:82`) sets the socket
`O_NONBLOCK` at create time and maps `EAGAIN` / `EWOULDBLOCK` from `sendto()`
to `HML_ERR_AGAIN`; a short send is `HML_ERR_IO`.

`hml_udp_send_frag` (`src/transports/udp/hml_udp.c:50`) then:

```c
hml_status st = hml_sock_send(im->sock, im->dgram, n);
if (st == HML_OK) {
  im->frags_sent++;
  im->bytes_sent += n;
} else if (st == HML_ERR_AGAIN) {
  im->frags_again++;
}
return st;
```

It counts the `HML_ERR_AGAIN` in `frags_again` and **returns the error to the
caller anyway.** It does not retry and does not buffer.

`hml_stream_send_frag` (`src/core/hml_stream.c:77`) propagates it and — this is
the important part — **does not advance `next_seq`, `frag_idx` or
`frame_bytes`**:

```c
hml_status st = s->ops->send_frag(s->x, &h, data, len);
if (st != HML_OK) return st; /* counters are not advanced on a failed send */
```

So a failed fragment is cleanly retryable: the caller may re-issue the identical
`hml_stream_send_frag()` call and it will produce a byte-identical datagram
under the same sequence number. This is deliberate.

**But the built-in callers do not use that.** `hml_gen`, `hml_gen_roce` and
`hml_ctrl_device` all propagate the error out and **abort the stream**:

| Caller | On a failed fragment |
| --- | --- |
| `hml_gen` (`adapters/synthetic/hml_gen.c`) | returns the status, exits non-zero, stops sending |
| `hml_gen_roce` (`host/roce_gen/hml_roce_gen.c:90`) | `if (st != HML_ERR_AGAIN) break;` — a non-`AGAIN` error breaks the loop; an `AGAIN` is... also not retried, it just continues |
| `hml_ctrl_device` (`src/control/hml_ctrl_device.c:176`) | `if (hml_sock_send(...) == HML_OK) d->frames_sent++;` — the frame is **silently skipped** and the loop continues |

`hml_ctrl_device` is the worst case and the clearest example: a full socket
buffer drops one synthetic frame, increments nothing that anyone can observe
except the device's own final `frames=N` total, and carries on. There is no
counter for it.

### 1.2 What is *not* backpressure

There is no pacing, no rate limit, no credit, no window, and no
`HML_ERR_BUSY` on the data path. `HML_ERR_BUSY` exists in `hml_status` but is
only ever returned by `hml_transport_register` for a duplicate registration.

The `hml_dest_cfg.window_len` field (remote window in 4 KiB units) is a
*receiver-side* hint that configures the simulated RDMA memory window in
`hml_roce_rx_open`. It is not enforced by the sender.

### 1.3 Frame-size limits are hard rejections, not backpressure

| Check | Returned | Where |
| --- | --- | --- |
| fragment `len > HML_MAX_FRAG_PAYLOAD` (1428) | `HML_ERR_TOOLARGE` | `hml_stream_send_frag` |
| frame would exceed `cfg->max_frame_bytes` | `HML_ERR_TOOLARGE` | `hml_stream_send_frag` |
| `HML_ROCE_MAX_PAYLOAD` exceeded, or the datagram does not fit the internal buffer | `HML_ERR_TOOLARGE` | `roce_send_frag` / `roce_emit` |
| `dest->frame_size == 0` | `HML_ERR_ARG` at open | `roce_open_ex` — a zero `frame_size` would make the EOF meta slot collide with the first data write |
| control payload `> HML_CTRL_MAX_PAYLOAD` (512) | `HML_ERR_TOOLARGE` | `xchg` in `hml_client.c` |

These are configuration errors, surfaced immediately. They are not a policy for
a slow consumer.

### 1.4 The RoCEv2 send path differs

`roce_emit` (`src/transports/rocev2/hml_rocev2.c:293`) keeps `psn` and `ip_id`
**untouched when `hml_sock_send` fails**, so a retry emits a byte-identical
packet — the same invariant as the UDP path. The only case where a packet is
deliberately *not* resent is the fault-injection hook, which models a datagram
already in flight and therefore **advances** the PSN so the receiver sees a
genuine gap.

---

## 2. Receive path

### 2.1 The kernel buffer is the first and least controlled queue

`hml_sock_udp_create` (`ports/posix/hml_port_posix.c:40`) creates the socket and
binds it, but **never calls `setsockopt(SO_RCVBUF)` or `SO_SNDBUF`.** There is no
`SO_RCVBUF` or `SO_SNDBUF` call anywhere in `src/`, `ports/`, `host/` or
`adapters/`. The receive window is therefore whatever the kernel's
`net.core.rmem_max` default happens to be.

**Consequence, observed directly:** when the receiver cannot drain the socket
fast enough, the kernel silently discards datagrams. Nothing in this library
sees that event — it is not an error return, not a counter, not a log line. The
reassembler only learns about it later, as a *gap*. This is the mechanism behind
the `test_roce_e2e` flake documented in the README:

```text
phase A: 0.86 s  eof_frames=501 writes=4509 bytes=4509000 icrc_err=0
phase B: 0 of 100 frames survived 2% loss
```

501 frames instead of 500, and a phase that injected 2% loss but lost 100% of
its frames: the receive buffer overflowed. `gaps` and `frames_dropped` went up,
`crc_fail` and `proto_fail` stayed 0, because the lost datagrams never arrived —
they were dropped before the library saw them. The counters cannot distinguish
"lost on the wire" from "dropped by the kernel because we were too slow."

The only lever available today is to make the consumer faster, since the library
exposes no socket-buffer knob. Sizing `SO_RCVBUF` is an open item in
[ship_checklist.md](ship_checklist.md).

### 2.2 `hml_reasm`: drop-oldest, with a pending-gap grace period

The reassembler has exactly **2 slots** (`HML_REASM_SLOTS`, `src/core/hml_reasm.c:50`),
so at most 2 streams are reassembled at once. Its overflow policy is
**drop-oldest**.

**Slot exhaustion** (`hml_reasm_feed_datagram`, when a datagram arrives for a
`stream_id` that owns no slot):

```c
if (victim == HML_REASM_SLOTS) { /* every slot is busy: drop the oldest frame */
  victim = 0;
  for (unsigned i = 1; i < HML_REASM_SLOTS; i++)
    if (r->slots[i].seq_base < r->slots[victim].seq_base) victim = i;
}
if (s->in_flight) r->st.frames_dropped++;
```

"Oldest" is the smallest `seq_base`, which is a sound proxy here because the
sequence wraps at 2^32 and a 2-slot working set will never be more than 2^31
behind. **A third concurrent stream costs the oldest in-flight frame**, and
`frames_dropped` is incremented. A free slot is preferred, so this only fires
under genuine third-stream contention.

**Frame-identity violation** (a fragment whose `seq != seq_base + frag_idx`):
the in-flight frame is dropped, `frames_dropped++`, and a new window starts. If
the abandoned frame was `pending`, the drop is also charged to `gaps`. This is
the case that stops two frames silently merging.

**Payload overflow** (a fragment that would exceed `cap`):

```c
if ((size_t)h.payload_len > r->cap - s->bytes) {
  s->poisoned = true;
} else { ...stage it... }
```

The slot is marked `poisoned` and the **rest of the frame is swallowed**: every
subsequent fragment of that frame is staged (or ignored) until `HML_FLAG_END`
arrives, at which point `slot_finish` does `frames_dropped++` and clears the
frame. Nothing is ever delivered from a poisoned frame. This is the correct
behaviour — a truncated frame is worse than a dropped one — but it means one
oversized fragment costs a whole frame plus the rest of its fragments' worth of
buffer.

**Final-size overflow** (the assembled frame is larger than `cap`): also
`frames_dropped++`, never delivered.

**Non-uniform fragmentation** (all fragments but the last must share a payload
length): `gaps++` and `frames_dropped++`, never delivered. v1 has no segment
reassembly beyond the uniform case.

**The pending grace period** is the subtle part, and it is a *deliberate
anti-thrash* rule:

> Because a datagram that lost its END flag may still be in flight, a frame that
> reaches `HML_FLAG_END` with missing indices is not dropped straight away: it
> becomes "pending" and waits for its stragglers (this is what makes reversed
> delivery order work). The loss is confirmed — `gaps++` and `frames_dropped++` —
> as soon as a datagram proves the stragglers will not arrive, i.e. a fragment of
> the next frame.

So a gap costs one frame's latency, not one frame, **provided the next frame
arrives**. If the stream stalls right after a lossy frame, the pending frame sits
in memory until either a straggler or the next frame's first fragment shows up.
There is **no timer** that expires it. On a stalled stream that is a retained
buffer indefinitely. `hml_reasm_reset()` is the only way to clear it.

**Duplicates are idempotent and free:** a `frag_idx` whose presence bit is
already set is ignored, and `frags_recv` still counts the datagram.

### 2.3 `hml_roce_rx`: the whole window, wiped on any meta failure

`hml_roce_rx` (`host/roce_demux/hml_roce_demux.c`) simulates a remote memory
window of `window_len_bytes` and a 20-byte metadata slot at
`window_base + frame_size`.

| Condition | Action | Counter |
| --- | --- | --- |
| ICRC mismatch (when `verify_icrc`) | drop the datagram, PSN **not** advanced | `icrc_err++` |
| bad IP/UDP/BTH/RETH, wrong UDP port, `wlen` inconsistent, wrong `rkey` (when non-zero), VA outside the window | drop the datagram, PSN not advanced | `proto_err++` |
| PSN discontinuity forward by more than 1 | note it, then process the datagram normally | `psn_gaps++` |
| PSN stale or duplicated (`fwd >= 0x800000`) | **ignore the datagram entirely** | nothing |
| meta length not 20, bad magic, `frame_bytes > frame_size` or `> window_len_bytes`, or the frame CRC-32 does not match | drop the frame **and `memset` the whole window to zero** | `meta_drops++` |
| meta `frame_seq` equal to the last delivered one | ignore — a frame is never delivered twice | nothing |

Two consequences worth stating plainly:

* **A single corrupt or misattributed metadata write destroys the entire
  in-progress frame**, because the window is the only copy of the data. There is
  no per-fragment CRC on the RoCEv2 path; the ICRC protects the packet in
  flight, and the meta record's CRC-32 protects the *assembled frame*. Losing
  the meta write loses the frame with no partial recovery.
* **PSN continuity doubles as a loss detector.** Because every drop above leaves
  the receiver PSN untouched, the next accepted datagram shows a gap. So
  `psn_gaps` is the honest "packets went missing" counter for T1a — but only
  once the cause is understood, since a `meta_drops`-induced window wipe looks
  identical on the wire to a genuine network loss.
* `hml_roce_rx_poll` handles *every* datagram currently available and then
  returns; it does not block once the queue is drained. A caller that does not
  re-enter it promptly will overflow the socket buffer, exactly as in 2.1.

### 2.4 The MCU ports: bounded queue, drop when full

`ports/freertos_lwip` and `ports/esp_idf` each create a **FreeRTOS queue of 8
receive items** (`xQueueCreate(8, sizeof(rx_item))`). The lwIP `udp_recv`
callback runs on the tcpip thread and pushes a `pbuf*` with a **zero wait**:

```c
/* No wait: the tcpip thread must not block. A full queue means "drop now" and
 * is accounted by the caller's gap/frame_gap counters. */
if (xQueueSend(s->rxq, &item, 0) != pdTRUE) pbuf_free(p);
```

The pbuf is freed and the datagram is gone. **8 datagrams is about 11 KiB of
payload** at 1458 bytes per datagram. Overflowing it is easy on any real
link-rate input.

The comment's claim that this "is accounted by the caller's gap/frame_gap
counters" is only true indirectly: the queue overflow produces no counter of its
own, so it surfaces only as a reassembly `gaps` / `frames_dropped` on the frame
that lost a fragment. There is no `rxq_overflow` counter.

The two additional knobs that matter, documented at the top of
`ports/esp_idf/hml_port_esp.c` and untested because the real path has never been
compiled by its own toolchain:

* `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE` and `CONFIG_LWIP_TCPIP_MBOX_SIZE` must fit
  the intended RX rate.
* `CONFIG_LWIP_SO_RCVBUF` sets the per-socket receive buffer.

The 8-deep queue is a hardcoded constant in both ports. It is not configurable.

### 2.5 Bridge stub FIFO: `HML_ERR_AGAIN`, all-or-nothing

`src/bridge/hml_bridge_stub.c:105` — the stub UART window is a 256-byte
circular FIFO. A read larger than what is buffered returns `HML_ERR_AGAIN` with
**nothing popped**, so a partial read is impossible.

---

## 3. Policy summary

| Path | Buffer fills | Policy | Observable? |
| --- | --- | --- | --- |
| UDP socket send | kernel `sndbuf` | `HML_ERR_AGAIN` up the stack; no retry; counters not advanced; shipped callers abort or silently skip | `frags_again` in the transport, but **not exposed by any public API or CLI** |
| RoCEv2 send | kernel `sndbuf` | identical, plus `psn`/`ip_id` preserved for a byte-identical retry | no counter |
| Socket receive | kernel `rcvbuf` | **kernel drops silently**; library sees a gap much later | only `gaps` / `frames_dropped`, indistinguishable from wire loss |
| Reassembly buffer | `cap` bytes | mark frame `poisoned`, swallow the rest, `frames_dropped++` on END | `frames_dropped` |
| Reassembly slots (2) | 3rd stream | drop-oldest by lowest `seq_base` | `frames_dropped` |
| Reassembly gap | missing `frag_idx` | hold `pending` until the next frame's fragment proves the loss; **no timer** | `gaps` + `frames_dropped`, delayed |
| RoCEv2 window | bad meta or CRC | wipe the entire window, drop the frame | `meta_drops` |
| RoCEv2 PSN | forward discontinuity | count, keep going | `psn_gaps` |
| MCU rx queue | 8 items | `pbuf_free`, datagram lost, no counter | nothing |
| Control server | `HML_CTRL_MAX_MSG` receive buffer (526 B) | a datagram longer than 526 bytes cannot parse (length must be exact) and is silently dropped, `bad_count++` | `hml_control_bad_count()` |
| Discovery | client window shorter than the beacon period | **structurally guaranteed to miss** — see below | `HML_ERR_TIMEOUT` |

The single most important honest statement: **the loss counters in this library
cannot distinguish congestion from corruption from the network.** `gaps` is
raised by a dropped kernel datagram, a genuinely lost datagram, a reordered
burst that arrived after the frame closed, and a UDP queue overflow on an MCU
port. Separating them needs per-cause counters that do not exist yet.

### 3.1 Discovery has no backpressure either, and a short wait is a coin flip

The control plane's *only* flow control is the caller's wait. The device emits a
beacon every `beacon_period_ms` (default `HML_CTRL_DEFAULT_BEACON_PERIOD_MS` =
**1000 ms**; the first one is due immediately, and `hml_control_poll` drives it),
and the client's `hml_client_discover(cl, timeout_ms, ...)` waits at most
`timeout_ms`.

**A client wait shorter than one beacon period can miss every beacon.** Once the
initial beacon has fired, the next is a full period away, so a
`timeout_ms < beacon_period_ms` call is guaranteed to expire unless it happens
to straddle a beacon. This is not a slow-path symptom; it is arithmetic, and it
is the cause of the intermittent `test_bridge_lifecycle` failure recorded in
[ship_checklist.md](ship_checklist.md) §4 (500 ms wait vs 1000 ms period).

The rule for callers: **wait at least one full beacon period, and prefer two.**
Or shorten the device's `beacon_period_ms` toward `HML_CTRL_MIN_BEACON_PERIOD_MS`
(10 ms) when discovery latency matters more than beacon traffic.

---

## 4. `hml_latency` — the histogram

Full API reference: [public_api.md](public_api.md) §`hml_latency.h`.

### 4.1 How it is tapped

Exactly one place, `src/core/hml_reasm.c:177`, at the moment a frame is
delivered:

```c
uint64_t now = hml_clock_us();
if (s->final_hdr.ts_us == 0u || now < s->final_hdr.ts_us) {
  r->lat.skew_samples++;
} else {
  (void)hml_latency_record(&r->lat, now - s->final_hdr.ts_us);
}
```

So one sample per **delivered** frame with payload, measured as
`hml_clock_us() - final_hdr->ts_us`: the sender's stamp on the *closing*
fragment versus the receiver's clock at delivery. Frames that are dropped
contribute nothing — a slow path that drops frames reports a *better* latency
than one that delivers them, which is why the gap and drop counters must always
be read alongside the percentiles.

Two cases are counted as `skew_samples` instead of being recorded:

* `ts_us == 0` — a sender that never stamps.
* `now < ts_us` — unsigned wrap, i.e. the two ends do **not** share a clock
  domain. This is the honest-failure path: a bogus value is never clamped into
  the histogram.

`hml_roce_rx` does **not** tap latency at all; it has no histogram. `hml_host`
exposes the reassembler's copy through `hml_host_latency()`.

### 4.2 The clock-domain caveat

There is no PTP, no NTP, no sync protocol. The subtraction is only meaningful
when both ends share a clock domain, which today means **loopback on one
machine**, where both stamp and read the same `CLOCK_MONOTONIC`. Across two
machines the delta is dominated by clock offset. Do not quote a number from this
histogram for a two-machine or MCU deployment.

### 4.3 Resolution and the p99-equals-max trap

64 buckets, default width 50 µs, so `0..3150 µs` is resolved. Anything above
that lands in the tail bucket: `overflow++`, and the value survives only in
`n` / `sum_us` / `min_us` / `max_us`.

Percentiles are **nearest-rank** over the histogram, and if the rank walk ends
in the tail bucket the result is the **exact `max_us`**, not a bucket boundary.
On a real loopback tail this happens routinely, so:

> **`p99` frequently equals `max`.** When it does, the p99 was not resolved by
> the histogram at all; it is a worst-case sample wearing a percentile's name.
> Always read `overflow` before quoting a percentile.

### 4.4 Measured loopback numbers

Reference run, 1000 frames of 9000 bytes (7 fragments each), `127.0.0.1`, on a
VirtualBox guest that shares its cores with a concurrent build:

```text
latency p50=150us p95=2750us p99=18224us max=18224us n=1000 bucket=50us
```

That is the run recorded in [latency.md](latency.md) and the one this project's
claims rest on. Re-measured during this documentation pass, five more runs of
`test_loopback` on the same machine:

```text
p50=200us p95=2500us  p99=12346us max=12346us
p50=200us p95=2750us  p99=16004us max=16004us
p50=250us p95=2500us  p99=14882us max=14882us
p50=200us p95=2500us  p99=10800us max=10800us
p50=250us p95=25925us p99=25925us max=25925us
```

Read honestly, that spread says two things:

1. **p50 and p95 are stable** at roughly 200–250 µs and 2.5–2.75 ms.
2. **p99 is not a stable percentile** — it ranges from 10.8 ms to 25.9 ms across
   identical runs, and in **every** run `p99 == max`. The tail is not resolved by
   the histogram; what is being reported is the single worst sample. The spread
   is receiver-side queueing and scheduler delay on a busy VM, not wire time.

A frame dequeued 20 ms late is still delivered correctly. This is a
latency-budget question, not a correctness one.

### 4.5 The budget

**`HML_LATENCY_BUDGET_US = 40000` (40 ms)**, defined in
`include/hml/hml_config.h:26`, and asserted by
`tests/m0_m1/test_loopback.c:357` as `p99 <= L_BUDGET_US`.

What it is: a **loopback smoke-test threshold on a shared VM.** It exists so
CI catches a gross regression, and it is deliberately loose — 40 ms is above
the worst p99 observed in thirteen clean runs, leaving roughly 1.2x headroom on
the worst one.

What it is **not**: an MCU figure, a dual-machine figure, or a saturated-rate
figure. Override at build time with `-DHML_LATENCY_BUDGET_US=<n>`; `0` disables
the budget.

`hml_host --latency` prints `verdict=TBD` unless `--budget-us` is given, and
only prints `PASS`/`FAIL` when `skew_samples == 0`
(`hml_host_latency_ok` in `host/hml_host/hml_host.c`). A metric that cannot be
trusted is not reported as good.

The budget is **load-sensitive and will flake.** On a cold or busy machine
`test_loopback` failed at exactly this check during this pass, with a p99 above
40 ms. That is the budget doing its job at the edge of its range, not a broken
build — but until the socket buffer is sized and the receive path is drained
more aggressively, treat a budget failure as "the machine was busy" only after
you have re-run it.

---

## 5. What would fix this, in order of value

1. **Size the receive socket.** `SO_RCVBUF` in `hml_sock_udp_create`, plumbed
   through `hml_sock.h` as an option or a new call. Today the kernel's default
   is the only limit, and overflowing it is invisible.
2. **Add per-cause loss counters** so congestion, corruption and wire loss are
   distinguishable, and surface the UDP transport's `frags_again` through a
   public stats getter.
3. **Add a pending-frame timeout** to `hml_reasm` so a stalled stream cannot
   retain a frame buffer indefinitely.
4. **Make the MCU rx queue depth and its overflow count configurable** instead of
   a hardcoded 8.
5. **Widen the latency histogram** or report a coarse tail bucket, so p99 is a
   percentile rather than a worst-case sample.
6. **Only then** tighten `HML_LATENCY_BUDGET_US` against a low-load,
   real-hardware measurement.

---

## See also

* [limitations.md](limitations.md) — non-goals, including the 2 MiB reassembly
  allocation that makes all of this moot on a small MCU.
* [latency.md](latency.md) — the full latency definition, clock-domain table and
  the original measurement.
* [public_api.md](public_api.md) — every counter's declaring header.
* [ship_checklist.md](ship_checklist.md) — these items as open work.
