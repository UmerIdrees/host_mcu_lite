# Host MCU Lite (`hml`) — Soak Testing

Bounded long-running tests for the paths that hold state across requests: CPU
fragment reassembly, the control plane, and the RoCEv2 data plane. They exist to
answer the two questions a unit test cannot — *does it leak*, and *do the
counters still add up after a long time*.

Everything runs on the host with **no RoCE NIC, no GPU, no RDMA, no root and no
network**. The two socket-backed tests bind loopback only.

| Guide | Scope |
| --- | --- |
| This file | The three soak tests, what they assert, how to lengthen them |
| [fuzzing.md](fuzzing.md) | Fuzz harnesses, corpus, replay, mutation |
| [testing.md](testing.md) | The unit and integration suite |
| [protocol_v1.md](protocol_v1.md) | Byte-level wire layout and the rejected-on-parse tables |

---

## 1. The three tests

| Test | Drives | Threads | Ports |
| --- | --- | --- | --- |
| `test_soak_reasm` | `hml_reasm_*` CPU reassembly, both capacities | none | none |
| `test_soak_ctrl` | `hml_control_poll` server + `hml_client_*`, over loopback | 2 | 4 |
| `test_soak_roce` | `hml_rocev2` transmitter → `hml_roce_demux` receiver, over loopback | 2 | 1 per churn iteration |

All three are `RUN_SERIAL`, because the socket-backed ones bind ports and a
parallel run would race them for those ports.

### Why these three

`test_soak_reasm` is the one the plan names directly (M7 T0: "24-72h soak,
memory flat"). Reassembly is the only component that keeps per-stream state
across a long run, and it has two capacity settings that must both be flat.

`test_soak_ctrl` covers the control plane, which a unit test drives in-process
but a device never does. `hml_control` is documented as **not internally
locked**: one poller thread per `hml_control_t`. So this test runs the server on
a thread and the client in main, which is the only configuration that can find a
threading defect. A soak that called `hml_control_poll` synchronously would pass
against a server that deadlocks under its documented usage.

`test_soak_roce` covers the only path where a leak is most likely to be
invisible: both sides own buffers sized by configuration (the receiver's window,
the transmitter's send buffer) and both are driven by long streams, so a
per-frame leak would show only as a slow RSS climb.

---

## 2. Build and run

```sh
cmake -S . -B /tmp/trkm -G Ninja -DCMAKE_BUILD_TYPE=Release -DHML_WARNINGS_AS_ERRORS=ON
cmake --build /tmp/trkm -j8

ctest --test-dir /tmp/trkm -R test_soak --output-on-failure
```

```text
1/4 Test #39: test_soak_reasm ..................   Passed    5.01 sec
2/4 Test #40: test_soak_reasm_long .............   Passed   30.01 sec
3/4 Test #41: test_soak_ctrl ...................   Passed    5.03 sec
4/4 Test #42: test_soak_roce ...................   Passed    5.55 sec

100% tests passed, 0 tests failed out of 4
Total Test time (real) =  45.61 sec
```

The same binaries take `--seconds N`, so a CTest run and a multi-hour run are the
same code:

```sh
/tmp/trkm/tests/soak/test_soak_reasm --seconds 3600    # the plan's 1 h M1 case
/tmp/trkm/tests/soak/test_soak_ctrl  --seconds 3600
/tmp/trkm/tests/soak/test_soak_roce  --seconds 3600
```

`--port-base P` moves a socket-backed test's private range, for running two
copies against each other:

```sh
/tmp/trkm/tests/soak/test_soak_ctrl --seconds 600 --port-base 45200
```

| Argument | Default | Range |
| --- | --- | --- |
| `--seconds N` | 5 | `N` ≥ 1; `0` is clamped to 1 |
| `--port-base P` | first of the private range | must be inside that test's range |

Each test has its own private port range, so they can run simultaneously with the
fuzz harnesses and with the M2/M3 suites:

| Test | Range |
| --- | --- |
| `test_soak_ctrl` | 45100–45299 |
| `test_soak_roce` | 45300–45499 |

`--port-base` is validated against the range, and a busy port is skipped by
`soak_port_pick` rather than turned into a confusing later failure. A range with
no free port fails immediately with a clear message.

---

## 3. What is asserted continuously

Every soak checks three things at every sample, not only at the end. A leak that
appears at hour 3 has to be visible as growth, not only as a final verdict.

1. **Accounting invariants** that must hold at every sample — counters that
   partition the traffic, never contradict each other, and never exceed what was
   fed.
2. **Resident-set flatness**, as a *slope* with a generous budget rather than as
   `start == end`. A one-time arena allocation would read as a leak under a
   strict equality; a real per-iteration leak shows within seconds against a
   budget of a few hundred KiB.
3. **A wall-clock deadline checked inside the loop**, so a hang is a bounded
   failure that prints what it had done, rather than a CTest timeout.

The RSS baseline is taken after a warm-up (`soak_rss_sample(&rss, 2u)`), because
the first samples are dominated by first-touch page faults and by whatever the
test itself allocated lazily. RSS is read from `/proc/self/statm`; where that is
unavailable the tests say so rather than silently reporting flatness they cannot
see.

### Failures re-baseline

A failed growth check re-baselines and carries on, so one finding does not print
once per subsequent sample and bury the rest of the run. The verdict is still
recorded: `soak_finish` reports `FAILED` with the count if any check ever tripped.

---

## 4. `test_soak_reasm`

Phases, each with its own exact counter assertion — one blended budget could not
distinguish "loss ate the frames" from "a bug ate the frames":

| Phase | What it drives |
| --- | --- |
| clean | Two streams, four fragments per frame, stream id rotating |
| loss | One hole per N frames, each proved by the next frame's first fragment |
| crc | A broken payload CRC and a broken header CRC, each changing nothing else |
| flush | Zero-payload EOF-only flushes, which wake the consumer without counting |
| dup | A duplicate fragment delivered while the frame is in flight |
| reset | A frame abandoned mid-flight by `hml_reasm_reset` |
| churn | 4000 open/close pairs, so a context that is not fully released shows up |

Reassembly's deferred-loss rule is what the `loss` phase tests: a stream is only
*declared* missing once a later datagram proves the stragglers will not arrive,
because on a real network reordering is normal. A soak that simply dropped a
fragment and checked the next frame was gone would be asserting the wrong
behaviour, and would fail against a correct implementation.

Latency is recorded as a histogram and reported as a distribution, not a mean:

```text
frames_ok=40324 gaps=11860 crc_fail=4744 frags_recv=237200 flushes=9488 resets=2372
latency n=40324 p50=0 p99=0 max=3977 skew=0
PASS test_soak_reasm: 37952 iterations in 5.0 s, rss 2048 KiB -> peak 2048 KiB (flat)
```

`skew` counts frames whose fragments carried no timestamp — the documented
"sender does not stamp" case, which the library reports as clock skew rather than
as a latency. Zero is the correct answer here, and a non-zero count would mean the
sender stamps were being read as missing. `n + skew` must equal `frames_ok`, so
every delivered frame is either timed or explicitly attributed; that identity is
what stops a frame being quietly dropped from the statistics as well as from the
count.

The arena is opened at a fixed 32 KiB (`ARENA_BYTES`), and the churn phase opens
and closes 4000 contexts at that size. `hml_reasm_open` allocates twice the
capacity, so a context that is not fully released only becomes visible under
repeated open/close — which is why that phase exists and why its budget is
applied as a total rather than per iteration: each iteration allocates 64 KiB, so
a per-iteration budget would flag the peak of a single uncollected context.

---

## 5. `test_soak_ctrl`

One open server, one client on a thread, continuous `set_dest` → `start` →
`stop` → `status` cycles with malformed traffic interleaved, then an
open/close churn.

The reconciliation is the core of it. `rx_count` and `bad_count` are the server's
own counters, and the test requires:

- `rx_count` rises by exactly the number of datagrams sent;
- `bad_count` rises by exactly the number deliberately malformed;
- together they partition the traffic — nothing swallowed, nothing double-counted.

A datagram silently dropped by the server shows up as a reconciliation failure
rather than as a slow drift nobody notices.

Malformed traffic comes in six shapes, because a soak that sends one shape of
garbage proves only that one shape is rejected: too short for a header, broken
CRC, a length field claiming more than the datagram carries, wrong magic, wrong
version, and a non-zero reserved byte. After each batch the stored destination,
the streaming flag and all six callback counters must be unchanged — which is
what makes a fuzz finding here meaningful, since the fuzz harness proves
malformed input is *rejected* and this proves rejection has no *side effect*.

Two further properties:

- **A `GET_DEST` after a `SET_DEST` must read back byte for byte.** A server that
  accumulated state instead of replacing it fails here, with the differing field
  named.
- **An unknown opcode must be answered**, not ignored. A host waiting on that
  socket would otherwise block until its timeout, so this is a correctness
  property, not a robustness nicety. The response is checked for
  `op == HML_OP_ERROR` with `FLAG_ERR` and a `HML_ERR_NOTSUP` payload.

```text
rounds=642 datagrams=7745 rejected=1926 served=5819 beacons>0
PASS test_soak_ctrl: 642 iterations in 5.0 s, rss 2052 KiB -> peak 2116 KiB (flat)
```

The beacon is checked at the end: the advertised uuid, caps and firmware version
must match what the server was configured with, and two consecutive discoveries
must show an advancing timestamp. That is the third thing on the control plane
that runs on a timer, alongside the two request paths.

The 300-iteration open/close churn exists because `hml_control_open` binds a port
and allocates receive buffers. A server that leaks on close only shows up there,
and each iteration sends a real request first so the buffers are dirtied before
they are freed — a leak of a buffer that was never touched would be invisible.

---

## 6. `test_soak_roce`

The real transmitter against the real receiver over loopback, with faults
injected in between by the M3 fault hook. Three phases:

| Phase | Faults | What it establishes |
| --- | --- | --- |
| clean | none | Every frame must arrive, with zero PSN gaps |
| lossy | drop / flip / re-address, on a fixed pattern | Loss is fully explained, and each fault is attributed to the right check |
| churn | none | 200 receiver + transmitter open/close cycles |

### The partition

The main invariant, checked every round:

```
pkts == writes + eof_frames + meta_drops + icrc_err + proto_err
```

Every datagram the receiver read must be in exactly one bucket. Note the four
terms, not three: the EOF metadata write is a real RDMA WRITE and is counted in
`pkts`, but it lands in `eof_frames` or `meta_drops` rather than `writes`, so
leaving it out makes the identity fail by one per frame.

### Fault attribution

Checked after the drain, where the totals are final. This is the part that pins
down *which* check catches each corruption:

| Injected | Expect | Why |
| --- | --- | --- |
| payload bit flip | `icrc_err` | The datagram is structurally perfect; only the checksum can catch it |
| rkey rewritten, ICRC refreshed | `proto_err` | The checksum is valid, so only the key check can catch it |
| VA moved out of the window, ICRC refreshed | `proto_err` | Same: a receiver that only checked the CRC would have accepted it |

The two refreshed-ICRC corruptions are the interesting ones. A demux that got
either backwards would still balance the partition above, so this is a genuinely
separate check.

### Loss accounting

A frame is declared only when its meta write arrives, and the meta record's CRC
covers the whole window, so a frame is lost if any of its data writes or its meta
write was dropped, flipped or re-addressed. The assertion is therefore that frames
lost never exceeds datagrams damaged — one damaged datagram can cost at most one
frame. The exact count depends on which fragment of which frame the pattern hit
and is deliberately not asserted, but exceeding the bound would mean a damaged
datagram cost more than its own frame, which would be a real accounting bug.

### Content verification

Counters can all add up while the receiver assembles the wrong bytes. Frame
content is therefore deterministic — `frame[i] == (uint8_t)(seq + i)` — and every
delivered frame is verified byte for byte in the callback, with a length check and
a meta-CRC check. Any mismatch is a finding.

### Threading

The receiver runs on a thread; the sender is the main thread. The sender samples
the receiver's counters, so the receiver publishes a whole snapshot through a
**seqlock**: one atomic load on the poll path, no lock in the hot loop, and every
sample internally consistent. Sampling the counters directly would intermittently
catch `pkts` one datagram ahead of the bucket that datagram was about to land in,
and the partition identity would fail for no reason.

Flow control caps the sender at 48 datagrams beyond what the receiver has drained,
compared against the datagrams actually put on the wire rather than those offered
to the fault hook — a dropped datagram is never received, so counting it would let
the gap grow without bound and the sender would wait forever.

The clean phase requires zero PSN gaps. Nothing was dropped or corrupted, so a gap
there would mean datagrams went missing on a lossless loopback: a kernel drop or
a receiver bug, not something the demux can account for.

```text
clean: frames=3628 datagrams=36280 psn_gaps=0
lossy: frames=5560 delivered=6146 datagrams=91880 dropped=1503 flipped=1021 badkey=748
       badva=588 icrc_err=1021 proto_err=1336 meta_drops=2656
total: rounds=2297 frames_sent=9188 (clean 3628 + lossy 5560)
PASS test_soak_roce: 2297 iterations in 5.5 s, rss 2112 KiB -> peak 2180 KiB (flat)
```

`icrc_err=1021` matches `flipped=1021` and `proto_err=748+588=1336` matches
`badkey+badva` exactly, which is the attribution holding.

---

## 7. Long runs

The plan's numbers are hours, and the same binaries take `--seconds`:

| Case | Command |
| --- | --- |
| M1, 1 h, memory flat | `test_soak_reasm --seconds 3600` |
| M2, control plane | `test_soak_ctrl --seconds 3600` |
| M7 / T0, 24 h | `test_soak_roce --seconds 86400` |

Everything is checked at every sample, so an hour-long run reports the first
sample where an invariant broke, not just a final red verdict. A run that is
merely slow is not a failure; a run that stops making progress fails in bounded
time and prints its counters.

Watch the RSS line as it goes. `rss 2048 KiB -> peak 2048 KiB (flat)` is the
expected shape. A peak that climbs steadily while the iteration count climbs
linearly is the signature of a per-iteration leak, and the test will say so with
the growth, the baseline and the iteration count.

Note that a leak below the noise floor of `RLIMIT_AS`-free allocation is not
found by these tests. A heap profiler on a shorter run is the complement, not the
substitute: the soak proves the trend over hours, and a profiler finds the site.

---

## 8. Sanitizers

The soaks are worth running under ASan and UBSan, where the leak detector is the
point — a bounded allocation bug in the churn phases shows up there as a definite
leak rather than as a slow RSS slope:

```sh
cmake -S . -B /tmp/trkm-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build /tmp/trkm-asan -j8
ctest --test-dir /tmp/trkm-asan -R test_soak --output-on-failure
```

Two caveats, both real:

- `test_soak_roce` sets `RLIMIT_AS` nowhere but allocates through the demux, and
  ASan's shadow mapping makes the RSS numbers meaningless. Soak flatness under a
  sanitizer is not evidence; run the plain build for the memory verdict and the
  sanitized build for the correctness verdict.
- The fuzz harnesses' own address-space cap is skipped under a sanitizer for the
  same reason. See [fuzzing.md](fuzzing.md).

---

## 9. Troubleshooting

| Symptom | Cause | What to do |
| --- | --- | --- |
| `no free UDP ports in ...` | The private range is occupied | Pass `--port-base`, or wait for the other run |
| `served and rejected must partition the traffic` | The server lost or double-counted a datagram | Real finding; the datagram count in the FAIL line says which side is short |
| `latency ... skew != 0` | Something queued on a synthetic path | Real finding; the histogram line has the distribution |
| `resident set grew ...` | A leak, or a one-time allocation past the warm-up | Raise the warm-up if it is clearly first-touch; otherwise a real finding |
| CTest timeout | A hang | Run the binary directly: the deadline check prints its counters and exits |
