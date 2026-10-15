# Porting guide

How to bring HML to a new target, and the constraints you will hit. Written for
an engineer who has the source and an MCU toolchain, and wants T0 frames flowing.

**Read [limitations.md](limitations.md) first.** The single hardest constraint —
a 2 MiB reassembly allocation with no static alternative — may stop you before
you write a line of code.

---

## 1. The one architectural rule

> **There must be NO `#ifdef` inside `src/core/`.**

This is stated in `include/hml/hml_port.h` and it is enforced by a test:
`test_core_no_ifdef` fails the build if a preprocessor conditional appears in
core. All portability lives in `ports/`.

This is a good rule and it is why the core is readable, so respect it. If you
find yourself wanting a `#ifdef` in `src/core/hml_reasm.c`, the answer is a new
port-level function, not a conditional.

**What is in `src/core`:** `hml_crc`, `hml_proto_frame` / `cfg`, `hml_reasm`,
`hml_stream`, `hml_latency`, `hml_transport` registry. None of it touches an OS.

**What is a port:** `hml_clock_us`, the `hml_sock_*` UDP calls, and the build
rules. That's the whole contract.

---

## 2. The port contract

A port must supply exactly two things: a clock, and the `hml_sock_*` family.
The same seven symbols are declared in one place, the `HML_PORT_FN_LIST`
X-macro in `include/hml/hml_partition.h`, and `cmake/hml_firmware.cmake`
generates a translation unit from it whose initialisers are real function
addresses. Adding that file to your port's sources turns a missing symbol into a
link error that names it:

```cmake
add_library(my_port STATIC src/port_my_board.c ${hml_firmware_contract_source})
target_link_libraries(my_firmware PRIVATE my_port hml::hml_core)
```

Your port must **not** also define `hml_port_contract_check` itself; both
example stubs say so in a comment, because defining it in both places is a
duplicate symbol. Note that the generated object is unreferenced data, so a
linker with `--gc-sections` may discard it — force it with
`-u hml_port_contract_check` if that happens. The full contract, including which
libc functions the portable arms call, is in
[final_partitioning.md](final_partitioning.md) section 3.

### 2.1 `uint64_t hml_clock_us(void)`

A monotonic microsecond counter. It is used for two unrelated purposes and
**you must not conflate them**:

1. **Timeouts** — beacon scheduling, request/response budgets, poll deadlines.
   A coarse or jumpy clock breaks control-plane timeouts.
2. **Latency samples** — `hml_clock_us() - ts_us` becomes a histogram sample.
   See §6; this one has a trap.

It does **not** need to be a wall clock and does not need to be synchronised to
anything. The three shipped ports each use their own monotonic source:
`CLOCK_MONOTONIC` on POSIX, `esp_timer_get_time` on ESP-IDF, FreeRTOS ticks
converted on FreeRTOS/lwIP.

```c
/* src/core calls this; never inline a different one in core. */
uint64_t hml_clock_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
```

### 2.2 The `hml_sock_*` calls

From `include/hml/hml_sock.h` — five functions, IPv4 only in v1:

| Function | Contract | Notes |
| --- | --- | --- |
| `hml_sock_udp_create(out, bind_ip, bind_port, broadcast)` | Create a UDP socket; bind when `bind_port != 0`; set `SO_BROADCAST` when `broadcast`. `bind_ip` is a host-order IPv4 (`HML_IP_ANY` = 0.0.0.0, `HML_IP_LOOPBACK` = 127.0.0.1) | must be **non-blocking**; every shipped port sets `O_NONBLOCK` |
| `hml_sock_set_dest(s, ip, port)` | Set the default destination | `ip` is host-order |
| `hml_sock_send(s, buf, n)` | Send one datagram | `HML_ERR_AGAIN` on would-block, `HML_ERR_IO` on a short/failed send |
| `hml_sock_recv(s, buf, cap, n, timeout_ms)` | Receive one datagram | `timeout_ms < 0` block, `0` poll, `> 0` bounded wait; returns `HML_OK` with `n > 0`, `HML_ERR_AGAIN` for poll/timeout, or `HML_ERR_IO` |
| `hml_sock_close(s)` | Close | |

**The return-code mapping is not cosmetic.** The send path's
retry-safety guarantee depends on it: when `hml_sock_send` returns non-`HML_OK`,
`hml_stream_send_frag` deliberately does **not** advance `next_seq` / `frag_idx`
/ `frame_bytes`, and `roce_emit` does **not** advance `psn` / `ip_id`. A port that
returns `HML_OK` for a datagram that was not sent will cause sequence corruption.
A port that swallows a would-block into `HML_OK` will silently drop data with
no counter. Map errors honestly.

**`timeout_ms` is advisory, not exact.** The control server computes poll
deadlines from it and tolerates early returns. Returning early with
`HML_ERR_AGAIN` is safe; returning *late* is not.

### 2.3 Two extra obligations, not spelled out in the header

These are not in `hml_sock.h` but the code depends on them, and getting them
wrong is the most common way to port this badly:

* **A bounded receive queue.** lwIP's UDP callback runs on the tcpip thread and
  must not block. All three shipped ports hand off to a fixed-depth queue
  (8 items on the MCU ports) and **drop with `pbuf_free` when it is full**, with
  no counter. See [backpressure.md](backpressure.md) §2.4. Size this deliberately;
  the hardcoded 8 is a starting point, not a recommendation.
* **`SO_RCVBUF` sizing.** No port calls it. The POSIX port therefore inherits
  the kernel's `net.core.rmem_max` default and silently drops datagrams when the
  consumer falls behind, with no counter. If your port can size it, do. This is
  the single highest-value port improvement you can make.

---

## 3. Platform detection

`include/hml/hml_port.h` picks a tag in this order:

```c
#if defined(HML_PORT_POSIX)
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#define HML_PORT_POSIX
#elif defined(HML_PORT_FREERTOS)
#elif defined(FREERTOS) || defined(ESP32) || defined(ESP_PLATFORM)
#define HML_PORT_FREERTOS
#endif
```

A port may predefine its own tag (`-DHML_PORT_FREERTOS`) or rely on the compiler
macros. **Predefining is safer on a cross-compile**, because the host build's
`__linux__` can otherwise win over your intended target. Note the ordering
consequence: on a Linux host that also predefines `HML_PORT_FREERTOS`, the
`__linux__` branch matches first and `HML_PORT_POSIX` is *also* defined. If you
see that, cross-compile properly rather than fighting the macro.

Note the tag is `HML_PORT_FREERTOS` for **both** MCU ports — ESP-IDF is built on
FreeRTOS, so the two share a tag and differ by build rules, not by tag.

---

## 4. The three shipped ports

| Port | Directory | Tag | Status |
| --- | --- | --- | --- |
| POSIX host | `ports/posix` | `HML_PORT_POSIX` | **fully built and tested** on Linux |
| FreeRTOS/lwIP | `ports/freertos_lwip` | `HML_PORT_FREERTOS` | **real and stub paths; never compiled by its own toolchain** |
| ESP-IDF | `ports/esp_idf` | `HML_PORT_FREERTOS` | **real and stub paths; never compiled by its own toolchain** |

**The honest caveat:** only the POSIX port is actually verified. The two MCU
ports contain a real implementation guarded by `#if`s *and* a host-buildable
stub path, and because the stub path is what the host build compiles, **a green
host build tells you nothing about the real MCU path.** See §7.

Installed target: only `hml::hml_port_posix` is exported by `install.cmake` — an
MCU port is a static library you link into your firmware image, not something
you `find_package`. See [install.md](install.md).

---

## 5. Adding a port: steps

1. **Create `ports/<name>/`** with a `CMakeLists.txt` producing a static library
   `hml_port_<name>`.
2. **Implement the seven functions** from §2. Start by copying
   `ports/posix/hml_port_posix.c` — it is the smallest complete implementation
   and the only one that is tested.
3. **Add the tag** to `hml_port.h` if it is a new platform family, and make sure
   the branch order cannot be captured by `__linux__` on your build host.
4. **Wire it into the top-level `CMakeLists.txt`** alongside the other
   `add_subdirectory(ports/...)` calls.
5. **Do not add `#ifdef` to `src/core`.** If you need a new capability, add a
   function to the port and declare it in `include/hml/`.
6. **Port the clock first and test it.** Write a test that samples
   `hml_clock_us()` in a loop and asserts it is non-decreasing and roughly the
   right rate. A bad clock silently breaks every timeout in the control plane
   and poisons the latency histogram.
7. **Test the socket layer** with a loopback send/receive, including the
   `HML_ERR_AGAIN`-on-would-block mapping. Do this before touching the data
   plane; the sequence guarantees in §2.2 are untestable otherwise.
8. **Then run T0.** Use the quickstart in the [README](../README.md) against
   your target.

### 5.1 Build-rule shape

Follow `ports/posix/CMakeLists.txt`. The important properties:

* The port library is **static** and links `hml_core` (or is linked alongside
  it); the installed export is `hml::hml_port_posix`.
* On MCU ports, keep the real path behind a guard and provide a **host-buildable
  stub** so the tree still configures and tests on a Linux CI box. This is what
  the two existing MCU ports do.
* Guard the whole directory with the same `HML_BUILD_TESTS` / option pattern
  the other ports use, so a cross-build does not try to build host tools.

---

## 6. The clock-domain trap (read before trusting latency)

The latency tap is one line in `src/core/hml_reasm.c:177`:

```c
uint64_t now = hml_clock_us();
if (s->final_hdr.ts_us == 0u || now < s->final_hdr.ts_us) {
  r->lat.skew_samples++;
} else {
  (void)hml_latency_record(&r->lat, now - s->final_hdr.ts_us);
}
```

The sender stamps `ts_us` with **its own** `hml_clock_us()`; the receiver
subtracts it from **its own**. There is no PTP, no NTP, no sync of any kind.

Therefore:

* **Loopback on one board:** both ends read the same counter, so the delta is
  real. This is the only configuration where the numbers mean anything.
* **Two boards, unsynchronised:** the delta is dominated by the clock offset.
  You will get a plausible-looking number that is simply wrong.
* **MCU to host:** same problem, and worse, since the clocks have different
  epochs.

The code does the honest thing when it detects this — an unstamped or
backwards-timestamped frame is counted in `skew_samples` and **excluded** from
the histogram rather than clamped into it — and `hml_host` refuses to print
`PASS`/`FAIL` unless `skew_samples == 0`. So a misconfigured deployment shows
`verdict=TBD` rather than a fake green.

**If you need real two-ended latency, you must add time sync yourself** and
either re-stamp before recording or subtract a measured offset. Out of scope
here. Also note `p99` currently equals `max` in practice, because the 64-bucket
histogram's tail is unresolved — see [backpressure.md](backpressure.md) §4.

---

## 7. The MCU reality check

Before you start, confirm the target can hold the memory.

**`hml_reasm_open(r, 0)` allocates `2 * HML_MAX_FRAME_SIZE` = 2 MiB of heap**
(staging plus gather) by default, plus roughly 9 KiB of fixed per-context state
(two slots, each with a 92-byte presence bitmap and 735-entry offset/length
tables for `HML_MAX_FRAGS` = 735).

**There is no static or caller-buffer reassembly API.** This is the blocker, and
it is a design gap, not a configuration problem:

* If your MCU has **less than ~2 MiB of free heap**, `v0.2.0-ship` does not fit,
  full stop. Many small MCUs have less total RAM than the allocation.
* The obvious workaround — call `hml_reasm_open(r, cap)` with a smaller `cap` —
  trades memory for correctness: a frame larger than `cap` is **poisoned and
  dropped whole** (`frames_dropped++`), never delivered. So `cap` is effectively
  a *maximum frame size you promise never to exceed*, not a buffer knob. Set it
  and then guarantee it at the sender with
  `hml_stream_cfg.max_frame_bytes`.
* `max_frame_bytes` also caps the stream (`HML_ERR_TOOLARGE` past it), so this
  is a coherent configuration — but it means **MCU frames must be smaller than
  1 MiB**, and typically far smaller.

Also budget for:

* The **8-deep** MCU receive queue (~11 KiB of datagrams), whose overflow is
  unaccounted — it shows up only as reassembly `gaps`.
* FreeRTOS **config knobs** documented at the top of `ports/esp_idf/hml_port_esp.c`:
  `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE`, `CONFIG_LWIP_TCPIP_MBOX_SIZE`, and
  `CONFIG_LWIP_SO_RCVBUF`.
* **Task stack depth.** No measured figures exist for any port. Measure
  `hml_control_poll` and the demux loop on your target; this is unverified
  everywhere.

The honest summary: **T0 on a desktop is not evidence that T0 fits on your
MCU**, and the two MCU ports in the tree have never been run on real hardware or
compiled by their own toolchains. Plan for a porting effort, not a config flip.
The first thing to build is the static-buffer reassembly API.

---

## 8. Checklist for a new port

- [ ] `hml_clock_us` non-decreasing, correct rate
- [ ] all five `hml_sock_*` functions implemented, honest error mapping
- [ ] non-blocking sends; `HML_ERR_AGAIN` on would-block, **never** `HML_OK`
- [ ] `hml_sock_recv` honours `timeout_ms < 0 / 0 / > 0`
- [ ] `SO_RCVBUF` sized deliberately
- [ ] bounded RX queue sized deliberately, with an overflow counter
- [ ] no `#ifdef` added to `src/core` (`test_core_no_ifdef` enforces it)
- [ ] builds under the **real** target toolchain, not just the host stub
- [ ] T0 quickstart passes on the target: `hml_gen` -> `hml_host`, hashes match
- [ ] reassembly memory fits the target's heap (see §7)
- [ ] stack depth for `hml_control_poll` measured
- [ ] documented: FreeRTOS/lwIP config knobs, any clock caveat

---

## See also

* [limitations.md](limitations.md) — non-goals and the MCU blockers.
* [backpressure.md](backpressure.md) — overflow policy, the RX queue, latency.
* [public_api.md](public_api.md) — the full port-facing API.
* [install.md](install.md) — how the POSIX port is installed and consumed.
* [final_partitioning.md](final_partitioning.md) — the component split, and the link-time port contract.
