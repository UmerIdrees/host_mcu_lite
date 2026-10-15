# EOF latency: definition, instrumentation, and the written budget

This is the "written budget" the M1 exit criterion demands ("p99 EOF latency
within written budget"). It documents what is measured, on what hardware, and —
just as importantly — what the number does **not** mean.

## 1. Definition

Latency is measured as:

```
latency_us = hml_clock_us() at frame delivery   -   final_hdr->ts_us
```

i.e. **tx enqueue → EOF receive**, matching the plan's metric list. One sample is
recorded per *delivered* frame, in `hml_reasm` at the moment the EOF callback
fires (`src/core/hml_reasm.c`). Aggregates live in an `hml_latency` histogram
(`include/hml/hml_latency.h`, `src/core/hml_latency.c`), retrievable via
`hml_reasm_get_latency()` or `hml_host_latency()`.

## 2. The clock-domain caveat (read this first)

There is **no clock discipline in v1** — no PTP, no NTP, no sync protocol. The
subtraction above is only meaningful when both ends share a clock domain:

| Deployment | Valid? | Why |
| --- | --- | --- |
| Loopback / same host (`test_loopback`, both CLIs on one box) | **yes** | both stamp and read `CLOCK_MONOTONIC` on the same machine |
| Two machines, no sync | **no** | deltas are dominated by clock offset, not by the wire |
| MCU on a synced network | **only if** the device clock is disciplined | otherwise same as above |

When the subtraction would go negative (unsigned wrap) the sample is **not**
recorded as a fake number: it increments `lat.skew_samples`. A sender that never
stamps (`ts_us == 0`) is treated the same way. The CLI then reports
`verdict=TBD`, never `PASS` — a metric that cannot be trusted is not reported as
good.

## 3. Histogram and percentile rules

- 64 buckets, default width 50 µs → 0..3150 µs resolved; above that, samples land
  in the top bucket (`overflow` counts them) and are still tracked exactly in
  `min_us`/`max_us`/`sum_us`/`n`.
- Percentiles are **nearest-rank**: the smallest bucket boundary `v` such that at
  least `pct`% of samples are `≤ v`. `pct 0` → `min_us`, `pct 100` → `max_us`.
- **Top-bucket rule:** if the nearest-rank walk terminates in the top bucket, the
  result is the exact `max_us` instead of a bucket boundary. The tail is never an
  interpolation. This is why `p99` frequently equals `max` in the measurements
  below — the distribution is short and the tail is heavy.
- `n == 0` → `HML_ERR_AGAIN`; `pct > 100` or NULL → `HML_ERR_ARG`.

## 4. Measured numbers (this machine)

VirtualBox Linux guest, gcc 15, `-O3`, Release build, 9000-byte frames = 7
fragments of 1428 B, `127.0.0.1`, default pacing.

`tests/m0_m1/test_loopback`, clean phase (1000 frames, 9,000,000 B, 0 drops):

```
latency p50=150us p95=2750us p99=18224us max=18224us n=1000 bucket=50us
```

`hml_host --latency --budget-us 20000`, 50 frames:

```
latency p50=850us p95=6191us p99=6191us min=37us max=6191us n=50 skew=0 overflow=10
latency p50=850us p95=6191us p99=6191us max=6191us n=50 skew=0 budget=20000us verdict=PASS
```

The full build is not a quiet machine: p50 sits at 150–850 µs, but the tail runs
to tens of milliseconds. That tail is **receiver-side queueing and scheduler
delay under CPU contention**, not wire time — the VM shares 8 cores with a
competing build. A frame that is dequeued 18 ms late is still delivered correctly;
it is a latency-budget question, not a correctness one.

## 5. The v1 budget

**`HML_LATENCY_BUDGET_US = 40000` (40 ms)**, set in
`include/hml/hml_config.h`, and asserted by `tests/m0_m1/test_loopback.c`
(`L_BUDGET_US`).

Justification, stated plainly: thirteen clean loopback runs on this machine gave
p50 = 200–250 µs and p99 between 9.8 ms and 32.7 ms. 40 ms is the smallest round
number above the worst observed p99, leaving ~1.2× headroom on the worst run.

**This budget is a loopback/VM smoke-test threshold, not a product target.** It is
deliberately loose so that CI does not flake on a loaded machine. Do not quote it
as an MCU, dual-machine, or saturated-rate figure.

## 6. What would make it meaningful on real hardware

1. **Discipline the device clock** (PTP on the device, or stamp with a
   host-provided offset over the control plane). Then the same delta is valid
   across machines and skew_samples should stay 0.
2. **Or change what is measured** for MCU/embedded work: report receiver-side
   queue depth and per-frame service time instead of a wall-clock delta. That
   metric is clock-independent and is the one that actually bounds an MCU's
   ability to keep up.
3. Tighten the budget only after a low-load, real-hardware measurement exists.

## 7. Reproducing

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/tests/m0_m1/test_loopback            # prints p50/p95/p99 for 1000 frames
./build/host/hml_host/hml_host --port 45801 --frames 50 --latency --budget-us 20000 &
./build/adapters/synthetic/hml_gen --dest-ip 127.0.0.1 --port 45801 --frames 50
```

`--latency` without `--budget-us` prints the percentiles and
`verdict=TBD`; with a budget it prints `PASS`/`FAIL` only when
`skew_samples == 0`.
