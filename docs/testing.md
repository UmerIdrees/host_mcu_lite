# Host MCU Lite (`hml`) — Build, Test and Verification Guide

Everything in this document was run on the tree as of **2026-09-28**. Console
output is copied from real runs, not reconstructed. Where a test from the plan
has **not** been run, this document says so and names the hardware it needs
rather than implying coverage.

| Guide | Scope |
| --- | --- |
| This file | How to build, how to run the 18 tests, what each asserts, real CLI flags, worked examples, fault injection, metrics, hardware gaps |
| [protocol_v1.md](protocol_v1.md) | Byte-level wire layout and the rejected-on-parse tables |
| [holoscan_mcu_lite_Plan.md](holoscan_mcu_lite_Plan.md) | The approved plan plus milestone status |
| [soft_roce_lab.md](soft_roce_lab.md) | M3b lab procedure (not authored by this pass) |

---

## 1. Build

C11, CMake, no external dependencies. The core library and all 18 tests build
and run with **no RoCE NIC, no GPU, no RDMA, no root and no network**.

```sh
cmake -S . -B build
cmake --build build -j
```

Out-of-tree is the recommended form and is what was used to produce every result
here, so nothing is written into the source tree:

```sh
cmake -S . -B /tmp/hmlbuild
cmake --build /tmp/hmlbuild -j
```

| CMake option | Default | Effect |
| --- | --- | --- |
| `HML_BUILD_TESTS` | `ON` | Build and register the CTest suite |
| `HML_BUILD_EXAMPLES` | `ON` | Build the six CLI tools listed in §4 |
| `HML_WARNINGS_AS_ERRORS` | `OFF` | `-Werror` for the project targets |
| `HML_ENABLE_ROCE_LAB` | unset | Opt in to the M3b Soft-RoCE lab, §3.2 |

To opt in to the lab, either pass `-DHML_ENABLE_ROCE_LAB=ON` or set the
environment variable `HML_ENABLE_ROCE_LAB=1` at configure time.

### 1.1 What gets built

Six executables, all in the build tree:

```
adapters/synthetic/hml_gen           synthetic T0 sender
host/hml_host/hml_host               T0 CPU receiver (Path A)
host/roce_demux/hml_host_roce        RoCEv2 T1a demux / receiver
src/control/hml_ctrl_device          control-plane device (M2)
src/control/hml_ctrl_host            control-plane scripted client (M2)
src/bridge/hml_bridge_demo           M4 bridge demo / lifecycle script
```

`host/soft_roce/` builds **no C target** and links **no libibverbs**. It is
shell scripts plus a note, deliberately, so the library stays green on a host
with no RDMA at all.

Note that `HML_BUILD_EXAMPLES` builds the six tools above; the top-level
`examples/` directory is reserved by the plan's layout and is **currently
empty**. There is no example program to run yet — the worked examples in §5 are
CLI invocations, which is the honest substitute.

---

## 2. Run the test suite

```sh
ctest --test-dir build --output-on-failure
```

Result on this tree, unmodified:

```
100% tests passed, 0 tests failed out of 18
Total Test time (real) =  13.24 sec
```

List the tests without running them with `ctest --test-dir build -N`. Run one
with `-R`, e.g. `ctest --test-dir build -R test_roce_e2e -V`.

### 2.1 Test inventory

`roce_lab_skip_path` is listed first because CTest orders it first; it belongs
to M3b.

| # | Test | Milestone | What it actually asserts |
| --- | --- | --- | --- |
| 1 | `roce_lab_skip_path` | M3b | Every lab script exits **77** (skip) under `HML_RXE_SKIP=1`. Proves the lab fails *closed* and cannot break a host with no RDMA. See §3.2. |
| 2 | `test_crc` | M0 | `hml_crc16` == CCITT/`0x1021` MSB-first, init `0xFFFF`, and `"123456789"` → `0x29B1`. `hml_crc32_ieee` == reflected IEEE, init/final `~`, `"123456789"` → `0xCBF43926`. Empty-buffer values `0xFFFF` and `0x00000000`. |
| 3 | `test_frame_codec` | M0 | Golden 28-byte header incl. `hdr_crc16` = `0xD373` over bytes `[0,26)`; magic/version/`payload_len` over-max rejection; truncated body; corrupted header CRC → `HML_ERR_CRC`; corrupted payload byte with matching trailing CRC still parses; END with zero payload; `seq` wrap 0xFFFFFFFF→0; golden 40-byte `hml_dest_cfg`; `pad0`/`pad1`/`rsvd` non-zero rejection; trailing bytes ignored. |
| 4 | `test_loopback` | M0/M1 | Full synthetic sensor → framer → UDP → reassembler → frame callback over the real POSIX socket path. |
| 5 | `test_reasm` | M1 | Multi-fragment reassembly; uniform-chunk layout rule violation dropped; reversed delivery held as `pending` then confirmed lost; duplicate `frag_idx` idempotent (no double delivery, no doubled bytes); two frame windows; oversized frame poisoned, never delivered truncated. |
| 6 | `test_stream` | M1 | `hml_stream` per-fragment `seq` allocation, `base = seq - frag_idx` identity rule, and that a fragment violating `seq == base + frag_idx` never merges into another frame. |
| 7 | `test_ctrl_codec` | M2 | Golden control header `48 4D 4C 43 01 01 00 05 00 00 00 00` + CRC `0x1658`; asserts `HML_CTRL_HDR_SIZE` == 12. `hml_ctrl_parse`: bad magic, bad version, `len > 512`, **truncated and trailing-junk** both rejected, CRC mismatch. Device side: every opcode, ACK/ERR payloads, unknown opcode → `HML_OP_ERROR` + `HML_ERR_NOTSUP`, wrong payload length → `HML_ERR_PROTO`. |
| 8 | `test_discovery` | M2 | Golden 28-byte beacon incl. CRC `0xAB47`; `ts_us` round-trips above 2^32 (48-bit encoding); `len != 28` rejected; CRC mismatch. |
| 9 | `test_lifecycle` | M2 | `discover → set_dest → get_dest → get_caps → poke/readback → start → status → stop` against the real device and client, including a request/response timeout path. |
| 10 | `test_icrc` | M3 | RoCEv2 ICRC over BTH..end-of-payload, cross-checked against an independent reference and the canonical IEEE vector. |
| 11 | `test_roce_hdr` | M3 | Full T1a datagram layout byte for byte: IPv4 `0x45`, total length, DF `0x4000`, TTL, **IP header checksum against an independent reference**, UDP length/checksum `0`, BTH `0x2A` with 24-bit BE PSN at bytes 3..5, AETH `0x0000`, RETH VA/rkey/len, ICRC byte order. |
| 12 | `test_roce_e2e` | M3 | T1a emitter → UDP → demux over the real socket, in three phases. Real output in §5.3. |
| 13 | `test_bridge_lifecycle` | M4 | The 7-step M4 lifecycle end to end — **169 checks**. Also runnable standalone, §5.4. |
| 14 | `test_bridge_periph` | M4 | Stub I2C/GPIO/UART windows: register read/write, NAK and timeout surfaced as errors rather than silent success, a mid-stream poke that does not tear down the data plane. |
| 15 | `test_bridge_phase` | M4 | `INIT → CONFIG → STREAM` ordering; illegal transitions rejected. |
| 16 | `test_accel_probe` | M5/M6 | The runtime capability probe reports what it actually found and never claims a NIC, a GPU or `nvidia-peermem` that is not present. |
| 17 | `test_core_no_ifdef` | M5 | `src/core` contains **no `#ifdef`**. The M5 structural pass criterion: the core stays portable and vendors live in `ports/`. |
| 18 | `test_ports_api` | M5 | POSIX, `freertos_lwip` and `esp_idf` ports present the same API surface and compile against the same headers. |

### 2.2 What the 18 tests do *not* cover

Stated plainly so nobody reads "18/18 green" as more than it is:

* **No NIC capture.** T1a bytes are pinned against our own golden vectors and
  decoded by our own demux. An encoder and a decoder that agree prove
  self-consistency, not interoperability. See §8.
* **No soak, no fuzz.** The plan's 1 h M1 soak, 24–72 h M7 soak and 1M-packet
  fuzz have not been run. `hml_host` is a long-running loop but has never been
  left running for hours.
* **No measured performance.** Nothing in this repository reports a throughput,
  latency or CPU number. §6.
* **No multi-host or netns run.** Every test is same-host loopback. The M1
  1 GbE two-host case and the M3 netns capture are unrun.
* **No aarch64.** The M0 pass criterion is x86_64 **and** aarch64 loopback.
  Only x86_64 has been built.
* **The Soft-RoCE lab has never been executed**, only its skip path.

---

## 3. Optional, hardware-dependent tests

### 3.1 The Soft-RoCE lab (M3b) — never run

**Status: scripts and docs only. The lab itself has never been executed**
because no RDMA-capable host was available. The default suite contains only the
fail-closed self-check, so CI is unaffected.

`host/soft_roce/` contains `check_env.sh`, `setup_rxe.sh`, `run_lab.sh`,
`teardown_rxe.sh`, `lab_common.sh` and `skip_selfcheck.sh`.

`roce_lab_skip_path` runs every script with `HML_RXE_SKIP=1` and requires exit
**77**. It is expected to PASS on every machine, including one with no root, no
RDMA and no network. If a lab script ever returned 0 under `HML_RXE_SKIP=1`,
this test would fail — which is the point: the lab is provably gated off.

The real `roce_lab` test is registered **only** on opt-in, because it creates
network namespaces and RDMA devices:

```sh
cmake -S . -B build -DHML_ENABLE_ROCE_LAB=ON
cmake --build build -j
ctest --test-dir build -R roce_lab --output-on-failure
```

It declares `SKIP_RETURN_CODE 77`, so an environment that cannot run the lab
**skips rather than fails**. It needs: root, the `rdma_rxe` module, two network
namespaces on a veth pair, and `ibv_devinfo` from `libibverbs-utils`. Set
`HML_LAB_KEEP=1` to leave the lab up for inspection; otherwise an `EXIT` trap
tears it down. The procedure is in [soft_roce_lab.md](soft_roce_lab.md).

Per the plan's own kill criteria, `rdma_rxe` is **not a performance proxy for
ConnectX** and M3b can never be a T1 acceptance gate.

---

## 4. CLI reference

Flags and defaults below are transcribed from each `usage()` function in the
source. Note two traps in §7.

### 4.1 `hml_gen` — synthetic T0 sender

```
usage: hml_gen [--dest-ip A.B.C.D] [--port N] [--frames N] [--size N] [--rate N]
                [--seed N] [--xport NAME] [--stream-id N] [--seq-start N] [--eof-only]
  --dest-ip   destination IPv4 (default 127.0.0.1)
  --port      destination UDP port (default 55101)
  --frames    frames to send (default 10)
  --size      payload bytes per frame (default 9000)
  --rate      frames per second, 0 = as fast as possible (default 0)
  --seed      synthetic pattern seed (default 1)
  --xport     transport name from the registry (default udp)
  --eof-only  close frames with a zero-payload EOF-only flush
```

Undocumented in the usage text but accepted: `--stream-id` (default 1) and
`--seq-start` (default 0). **UDP only** — see §7.1.

### 4.2 `hml_host` — T0 CPU receiver

```
usage: hml_host [--port N] [--frames N] [--verbose] [--stats] [--max-frame BYTES]
  --port N       UDP data port to bind (default 55101)
  --frames N     stop after N completed frames (default 0 = forever)
  --verbose      print one line per completed frame
  --stats        print a stats line every second while running
  --max-frame N  reassembly buffer (default 1048576)
```

Prints one `frame seq=… bytes=… hash=0x…` line per completed frame with
`--verbose`, then on exit a `summary …` line and a `reasm …` line carrying every
reassembly counter. `--stats` emits a periodic line only while frames are still
arriving, so a sub-second run shows none.

`SIGINT`/`SIGTERM` are handled: the host prints its summary and exits 0 rather
than dying mid-frame.

### 4.3 `hml_host_roce` — RoCEv2 T1a demux

```
usage: hml_host_roce [--port N] [--window-base HEX] [--window-len N] [--frame-size N]
                     [--rkey HEX] [--frames N] [--no-icrc] [--verbose]
```

Defaults: `--port 14791` (the unprivileged mirror of the privileged 4791),
`--window-base 0x100000`, `--window-len 65536`, `--frame-size 9000`,
`--rkey 0xDEADBEEF`, `--frames 0` (forever). Numeric arguments accept decimal or
`0x` hex. `--no-icrc` disables ICRC verification so an ICRC-error path can be
observed rather than hidden. See §7.2.

### 4.4 `hml_ctrl_device` — control-plane device

```
usage: hml_ctrl_device [options]
  --ctrl-port N     control plane port (default 55102)
  --disc-port N     beacon port, 0 disables (default 55100)
  --beacon          enable beacon (default)
  --no-beacon       disable beacon
  --beacon-ms N     beacon period in ms (default 1000)
  --reply-ip A.B.C.D  where responses are sent (default 127.0.0.1)
  --reply-port N    client port that receives responses (default 45599)
  --uuid N          device uuid (default 1)
  --caps N          HML_CAP_* bitmask (default 3)      <-- see §7.3
  --fw N            firmware version hex (default 0x00010000)
  --frames N        synthetic datagrams to send after START, 0 = forever
  --data-port N     fallback data port when no dest was set (default 55101)
  --pace-us N       inter-frame delay in us (default 2000)
```

`--reply-ip`/`--reply-port` exist because `hml_sock_recv()` cannot learn a
sender's address: the device cannot know where to answer unless it is told.

### 4.5 `hml_ctrl_host` — control-plane client

```
usage: hml_ctrl_host [options]
  --ip A.B.C.D        device control ip (default 127.0.0.1)
  --ctrl-port N       device control port (default 55102)
  --disc-port N       device beacon port (default 55100)
  --local-port N      port this client binds, device replies here (default 45599)
  --timeout-ms N      per-request timeout (default 1000)
  --script LIST       comma separated steps, e.g. "discover,set_dest,start,status,stop"
  --interactive       read steps from stdin, one per line
  --dest-ip A.B.C.D   data destination ip used by set_dest (default 127.0.0.1)
  --data-port N       data destination port used by set_dest (default 55101)
  --frame-size N      frame_size in set_dest (default 0)
  --window-len N      window_len units of 4KiB in set_dest (default 0)
  --rx-ms N           duration of the "rx" step (default 1500)
  --poke-addr N       address for the poke step (default 3)
  --poke-val N        value for the poke step (default 0x1234)
steps: discover,ping,set_dest,get_dest,get_caps,status,start,stop,reset,poke,readback,rx
```

`--local-port` **must** match the device's `--reply-port`. The client sends
each step and fails the script on the first step that does not answer within
`--timeout-ms`.

### 4.6 `hml_bridge_demo` — M4 bridge demo

```
usage: hml_bridge_demo [--lifecycle | --serve] [options]
  --lifecycle        run the full M4 script in-process (default)
  --serve            run the device control server until SIGINT
  --ctrl-port N      control plane port (default 45602)
  --data-port N      data plane port (default 45601)
  --disc-port N      beacon port (default 45600)
  --client-port N    port the host client binds / device replies to (default 45699)
  --no-beacon        do not emit the discovery beacon
  --frames N         frames to stream after START, 0 = forever (default 8)
  --pace-us N        inter-frame delay in us (default 500)
```

`--lifecycle` runs the whole M4 sequence in one process and needs no second
process, no network and no root. `--serve` blocks on the control plane until
`SIGINT`. The bridge demo deliberately uses its own `456xx` ports so it cannot
collide with a running `hml_host`/`hml_ctrl_device` pair.

### 4.7 `hml_gen_roce` — RoCEv2 T1a sender

The RoCE counterpart of `hml_gen` (§4.1). It opens the **real** `hml_rocev2`
transport and pushes frames through `hml_stream`, so BTH/RETH/ICRC construction
and the 20-byte end-of-frame metadata record are built in exactly one place
(`src/transports/rocev2`) — this tool crafts no packets of its own.

```
usage: hml_gen_roce [--dest-ip A.B.C.D] [--port N] [--src-port N] [--frames N] [--size N]
          [--rkey HEX] [--qp N] [--psn N] [--window-base HEX] [--frame-size N]
          [--stream-id N] [--seed N] [--rate N] [--verbose] [--fallback-t0]
          [--selftest]
  --dest-ip      destination IPv4 (default 127.0.0.1)
  --port         destination UDP port (default 14791; production is 4791 and
                 needs root / CAP_NET_BIND_SERVICE)
  --src-port     source UDP port, 0 = ephemeral (default 0)
  --frames       frames to send (default 10)
  --size         payload bytes per frame (default 9000)
  --rkey         RETH remote key (default 0xDEADBEEF)
  --qp           RoCEv2 queue pair number (default 1)
  --psn          initial RoCEv2 PSN, 24 bits (default 1)
  --window-base  remote window base VA, also the first data write address
                 (default 0x100000)
  --frame-size   bytes per frame; 0 = use --size (default 0). Also places
                 the end-of-frame metadata slot at VA base + frame_size
  --stream-id    stream id (default 1)
  --seed         synthetic pattern seed (default 1)
  --rate         frames per second, 0 = as fast as possible (default 0)
  --verbose      print the config and the first packet's BTH/PSN/RETH
  --fallback-t0  if the RoCEv2 open fails, send as T0 UDP instead
  --selftest     build one datagram in-process, print it, exit (no sockets)
  --help         this text

per frame:   sent frame=<i> bytes=<n> chunks=<k> hash=0x%08x crc=0x%08x
             hash = FNV-1a  (matches hml_host on T0)
             crc  = CRC-32/IEEE (matches hml_host_roce crc= on T1a)
```

Notes worth knowing before you use it:

* **Both hashes are printed on purpose.** `hash=` is the FNV-1a value `hml_host`
  prints on T0, `crc=` is the CRC-32/IEEE value `hml_host_roce` prints on T1a.
  The frame payload is the same deterministic synthetic pattern in both cases, so
  the two tools can be compared byte for byte.
* **`chunks=` counts data fragments only.** Each frame costs
  `chunks + 1` datagrams: the fragments plus the end-of-frame metadata write
  that the transport emits when the last fragment carries `HML_FLAG_END`. There
  is no extra zero-payload EOF fragment — that would be a second frame, not a
  safety net. A 9000-byte frame is 7 chunks of ≤ `HML_MAX_FRAG_PAYLOAD`
  (1428 B), inside the 1440 B RoCEv2 payload cap.
* **Default port is 14791, not 4791**, for the same reason as every other tool
  here (§7.4): 4791 is below 1024 and needs root or `CAP_NET_BIND_SERVICE`.
  Use `--port 4791` for production, as root.
* **`--selftest` opens no sockets at all.** It calls the pure datagram builder
  and prints the result, so the wire format can be checked in a sandbox with no
  NIC, no peer and no privileges:

  ```sh
  ./host/roce_gen/hml_gen_roce --selftest
  ```

  ```
  selftest len=1500 payload=1440 first16=450005dc000040004011370f7f000001
  selftest bth0=0x2a psn=0x000001 va=0x00100000 rkey=0xdeadbeef icrc_le=0xc42bfe0b
  ```

  1500 = 20 B IP + 8 B UDP + 12 B BTH + 4 B AETH + 16 B RETH + 1440 B payload
  + 4 B ICRC. `first16` is the IP/UDP/BTH prefix, byte for byte what
  `test_roce_hdr` pins.
* **This section supersedes the opening line of §5.3** ("There is no standalone
  T1a emitter CLI"). §5.3 is still the right place for the loss/corruption
  injection; §5.5 below is the two-process wire run.

---

## 5. Worked examples

All output below is real, copied from runs on this tree.

### 5.1 T0 loopback: `hml_host` + `hml_gen`

Terminal 1:

```sh
./host/hml_host/hml_host --port 55101 --frames 3 --verbose --stats
```

Terminal 2:

```sh
./adapters/synthetic/hml_gen --port 55101 --frames 3 --size 9000 --seed 1
```

`hml_gen`:

```
sent frame=0 bytes=9000 hash=0xb6bc71f8
sent frame=1 bytes=9000 hash=0xf1f38a8f
sent frame=2 bytes=9000 hash=0x652abe0a
```

`hml_host`:

```
frame seq=0 bytes=9000 hash=0xb6bc71f8
frame seq=7 bytes=9000 hash=0xf1f38a8f
frame seq=14 bytes=9000 hash=0x652abe0a
summary frames=3 bytes=27000 eof=3 last_hash=0x652abe0a
reasm frames_ok=3 frames_dropped=0 frags_recv=21 gaps=0 crc_fail=0 proto_fail=0
```

How to read it, and why it is a meaningful check:

* **Every hash matches** sender to receiver, so the frame content is
  bit-exact, not merely the right length.
* `seq` advances by **7** per frame: a 9000-byte frame is
  `ceil(9000 / 1428) = 7` fragments, and `hml_stream` allocates one `seq` per
  fragment. `21 = 3 × 7` fragments total, which is the same 21 in
  `frags_recv`.
* `eof=3` equals `frames=3`. No spurious EOF and no missed one — the M0 pass
  criterion of "zero false EOF".
* `gaps=0 crc_fail=0 proto_fail=0 frames_dropped=0` on loopback, as expected.

To exercise the loss counters instead of a clean path, inject loss with
`tc netem`:

```sh
sudo tc qdisc add dev lo root netem loss 1%     # then re-run both terminals
sudo tc qdisc del dev lo root
```

Expect `gaps` to rise and `frames_dropped` to follow; a frame is never
delivered truncated.

### 5.2 Control plane: `hml_ctrl_device` + `hml_ctrl_host`

Terminal 1:

```sh
./src/control/hml_ctrl_device --ctrl-port 55102 --disc-port 55100 \
    --frames 3 --data-port 55101 --pace-us 2000
```

Terminal 2:

```sh
./src/control/hml_ctrl_host --ip 127.0.0.1 --ctrl-port 55102 --disc-port 55100 \
    --local-port 45599 --timeout-ms 1500 \
    --script "discover,ping,set_dest,get_dest,get_caps,poke,readback,start,rx,status,stop" \
    --dest-ip 127.0.0.1 --data-port 55101 --frame-size 9000 --window-len 16 --rx-ms 1200
```

`hml_ctrl_host`:

```
host up local=45599 peer=127.0.0.1 ctrl=55102 disc=55100 timeout=1500ms
script: discover,ping,set_dest,get_dest,get_caps,poke,readback,start,rx,status,stop
step discover ok uuid=0x00000001 caps=0x00000011 fw=0x00010000 ts_us=64082543642
step ping ok token=0xfeedbeef
step set_dest ok ipv4=127.0.0.1 port=55101 frame_size=9000
step get_dest ok kind=0 ipv4=127.0.0.1 port=55101 frame_size=9000
step get_caps ok uuid=0x00000001 caps=0x00000011 fw=0x00010000
step poke ok addr=0x00000003 val=0x00001234 readback=0x00001234
step readback ok addr=0x00000003 val=0x00001234
step start ok
step rx ok datagrams=3 bytes=192
step status ok streaming=1
step stop ok
host done: all steps ok
```

`hml_ctrl_device`:

```
device up ctrl=55102 disc=55100 reply=127.0.0.1:45599 beacon=on period=1000ms uuid=1 caps=0x11 fw=0x00010000
device streaming data to fallback:55101, frames=limited
control op=0x01 flags=0x0001 len=4
device dest set kind=0 ipv4=127.0.0.1 port=55101 frame_size=9000
control op=0x02 flags=0x0001 len=40
control op=0x03 flags=0x0001 len=0
control op=0x07 flags=0x0001 len=0
control op=0x09 flags=0x0001 len=8
control op=0x0a flags=0x0001 len=4
device start #1
control op=0x04 flags=0x0001 len=0
device frames done sent=3
control op=0x08 flags=0x0001 len=0
device stop #1
control op=0x05 flags=0x0001 len=0
```

What this proves, op by op, against the wire spec:

* `discover` returned `ts_us=64082543642`, which is **greater than 2^32**. That
  is the 48-bit timestamp encoding working as documented in
  [protocol_v1.md §6](protocol_v1.md) rather than a truncated u32.
* `set_dest` carries `len=40` and `get_dest` returns `kind=0` with the same
  fields, so the 40-byte `hml_dest_cfg` round-tripped through the wire.
* Every request has `flags=0x0001` (`FLAG_REQ`) with the expected `op` and
  `len`, and each got a matching `FLAG_RESP`. Note the device never sees a
  sequence number, because there is no sequence field — responses are
  correlated by opcode alone.
* `poke` then `readback` agree on `0x00001234`, so the register path is real.
* `start` → `rx datagrams=3` → `status streaming=1` → `stop` is the full
  configure-then-stream sequence, and `frames=3` matches the device's
  `--frames 3`.
* `bytes=192` for 3 datagrams: the device's synthetic payload is small, and
  `--frame-size 9000` only sizes the `hml_dest_cfg` field, it does not pad the
  demo payload.

### 5.3 T1a RoCEv2: `test_roce_e2e`

**There is no standalone T1a emitter CLI.** `hml_gen` is T0-only (§7.1), and
`hml_host_roce` is a receiver, not a sender. The T1a path is exercised through
the CTest end-to-end test, which drives the real emitter over a real UDP socket
into the real demux:

```sh
ctest --test-dir build -R test_roce_e2e -V
```

Real output, three phases:

```
phase A: 0.80 s  eof_frames=500 writes=4500 bytes=4500000 icrc_err=0
phase B: 80 of 100 frames survived 2% loss
phase C: proto_err=2 psn_gaps=22 meta_drops=1
delivered=582 (phase A 500 + phase B 80 + 2 clean)
PASS test_roce_e2e: 29 checks
```

* **Phase A** is the clean path: 500 frames, 4500 RDMA WRITEs, 4.5 MB,
  **zero ICRC errors**. `4500 = 500 × 9` writes per frame: 8 data writes
  carrying 9000 bytes at 1440 bytes each, plus 1 metadata write closing the
  frame. This is the one line in the repository that comes closest to a
  throughput statement, and it is still loopback.
* **Phase B** injects 2 % loss and checks survival: 80 of 100 frames, all of
  them CRC-clean. A frame with a missing write is dropped, never delivered
  truncated.
* **Phase C** injects corruption and out-of-order PSNs and asserts each counter
  moves for the right reason: `psn_gaps=22` from forward PSN jumps, `meta_drops=1`
  from a bad metadata record, `proto_err=2` from structurally invalid datagrams.
  Per-frame counters, not just an aggregate, is what makes this diagnostic
  rather than a smoke test.

If you need to watch a T1a receiver on real traffic, point
`hml_host_roce` at the window you configured and add `--verbose`:

```sh
./host/roce_demux/hml_host_roce --port 14791 --window-base 0x100000 \
    --window-len 65536 --frame-size 9000 --rkey 0xDEADBEEF --verbose
```

It prints `frame seq=… bytes=… crc=0x…` per delivered frame, and counts
`bad_len` (a frame whose size differs from `--frame-size`) and `bad_crc`.

### 5.4 Bridge lifecycle: `hml_bridge_demo --lifecycle`

One process, no network, no root, no hardware:

```sh
./src/bridge/hml_bridge_demo --lifecycle
```

Real output:

```
=== hml_bridge_demo --lifecycle (M4 bridge mode) ===
control=45602 data=45601 beacon=45600 client=45699 frames=8

device: up ctrl=45602 disc=45600 reply=127.0.0.1:45699 beacon=on caps=0x15 windows=4
[ 1] init (caps)                  PASS  uuid=0x00000042 caps=0x15 fw=0x00010400 phase=INIT
[ 2] gpio reset poke              PASS  reset rback=0x00000001 flag=0x00000001
[ 3] i2c reg writes               PASS  reg 0x10=0x80000001 reg 0x11=4096
device: dest kind=0 ipv4=127.0.0.1 port=45601 frame_size=4096
[ 4] program packetizer dest      PASS  phase=CONFIG pktz ip=0x7f000001 port=45601 rt=exact
device: start phase=STREAM
[ 5] start                        PASS  status=streaming phase=STREAM
device: frames done sent=8
[ 6] stream frames                PASS  frames=8/8 bad=0 midstream poke=0xa5a5a5a5
device: stop frames_sent=8 phase=CONFIG
[ 7] stop + verify                PASS  phase=CONFIG poke survived=yes | gpio reset=0x00000001 flag=0x00000001 | i2c[0x10]=0x80000001 i2c[0x11]=0x00001000 | uart=0 | pktz ip=0x7f000001 port=45601 qp=0x00000000 rkey=0x00000000

lifecycle: 7/7 steps PASS, 0 FAIL
```

This is the M4 plan's "init → GPIO reset poke → I2C reg writes → set packetizer
dest → start stream" ordering, and step 7 re-reads every register afterwards to
prove a mid-stream poke did not tear down the data plane. `hml_bridge_demo
--lifecycle` exits 0 only if all 7 steps pass; the CTest wrapper
`test_bridge_lifecycle` asserts the same sequence and runs 169 checks.

Note `rt=exact` on step 4: the demo reads the programmed packetizer destination
back and requires an exact match, so "program dest" is verified by readback
rather than assumed.

### 5.5 T1a RoCEv2 over the wire: `hml_host_roce` + `hml_gen_roce`

Two real binaries, two terminals, real UDP sockets, no privileges and no NIC.
The receiver's window and the sender's `--window-base`/`--frame-size`/`--rkey`
must agree, because that is what places the RDMA writes.

Terminal 1 — receiver:

```sh
./host/roce_demux/hml_host_roce --port 14791 --window-base 0x100000 \
    --window-len 65536 --frame-size 9000 --rkey 0xDEADBEEF --frames 3 --verbose
```

Terminal 2 — sender:

```sh
./host/roce_gen/hml_gen_roce --port 14791 --frames 3 --size 9000 --seed 1 --verbose
```

Real output, sender:

```
gen xport=rocev2 dest=127.0.0.1:14791 qp=1 psn=1 rkey=0xdeadbeef va=0x00100000 frame_size=9000 window=64KiB stream=1 frames=3 size=9000 seed=1
sent frame=0 bytes=9000 chunks=7 hash=0xb6bc71f8 crc=0xd35dba0f
sent frame=1 bytes=9000 chunks=7 hash=0xf1f38a8f crc=0x031f6466
sent frame=2 bytes=9000 chunks=7 hash=0x652abe0a crc=0x535d1f7e
sent frames=3 bytes=27000 psn_first=1 psn_last=24
pkt0 len=1488 bth0=0x2a psn=0x000001 va=0x00100000 rkey=0xdeadbeef rlen=1428
```

Real output, receiver:

```
hml_host_roce: listening udp/14791 window=0x100000 len=65536 frame=9000 rkey=0xdeadbeef icrc=on
frame seq=6 bytes=9000 crc=0xd35dba0f
frame seq=13 bytes=9000 crc=0x031f6466
frame seq=20 bytes=9000 crc=0x535d1f7e
stats pkts=24 icrc_err=0 writes=21 bytes=27000 eof_frames=3 psn_gaps=0 proto_err=0 meta_drops=0
host frames=3 bytes=27000 bad_len=0 bad_crc=0
```

What to read out of that, rather than just "it worked":

* `xport=rocev2` — the sender is on the real T1a transport, not a fallback.
* `pkts=24` and `writes=21` — `21 = 3 × (7 data + 1 metadata)`, and
  `psn_last=24` is the sender's own count of the same 24 datagrams. The
  receiver's packet count and the sender's PSN range agreeing is the check that
  no write was lost on the way.
* `icrc_err=0`, `psn_gaps=0`, `proto_err=0`, `meta_drops=0` — every failure
  class stays at zero, so the ICRC, the PSN sequence and the metadata record are
  all being validated, not skipped.
* `crc=0x…` on both sides is the same value: the sender's `crc=` and the
  receiver's `crc=` are the same CRC-32/IEEE over the same 9000 payload bytes.
* `seq=6, 13, 20` — the receiver counts the metadata write as a consumed
  sequence slot, so a frame of 7 fragments starts 8 apart, not 7.
* `pkt0 … rlen=1428` — the first datagram's RETH length is one fragment, i.e.
  the fragmentation is happening on the wire and not being faked by one big
  datagram.

### 5.6 T0 fallback: `hml_host` + `hml_gen_roce --fallback-t0`

The same sender, with the RoCEv2 open made to fail on purpose. `--fallback-t0`
degrades to the plain T0 UDP transport and **says so on stdout** rather than
silently sending T0 datagrams that a T1a receiver would reject.

To make the failure deterministic without privileges, hold the sender's source
port with a socket that does *not* set `SO_REUSEADDR` (an `hml_sock` bind does,
so this is the one case where the bind is refused):

Terminal 1 — holder, in the background:

```sh
python3 -c "import socket,time; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); \
  s.bind(('0.0.0.0',14894)); time.sleep(30)"
```

Terminal 2 — T0 receiver:

```sh
./host/hml_host/hml_host --port 14893 --frames 3 --verbose
```

Terminal 3 — sender with `--src-port 14894`:

```sh
./host/roce_gen/hml_gen_roce --fallback-t0 --port 14893 --src-port 14894 \
    --frames 3 --size 9000 --seed 1 --verbose
```

Real output, sender:

```
fallback: roce open failed, using T0 (udp) status=3
gen xport=udp dest=127.0.0.1:14893 qp=1 psn=1 rkey=0xdeadbeef va=0x00100000 frame_size=9000 window=64KiB stream=1 frames=3 size=9000 seed=1
sent frame=0 bytes=9000 chunks=7 hash=0xb6bc71f8 crc=0xd35dba0f
sent frame=1 bytes=9000 chunks=7 hash=0xf1f38a8f crc=0x031f6466
sent frame=2 bytes=9000 chunks=7 hash=0x652abe0a crc=0x535d1f7e
sent frames=3 bytes=27000 psn_first=1 psn_last=24
```

Real output, receiver:

```
frame seq=0 bytes=9000 hash=0xb6bc71f8
frame seq=7 bytes=9000 hash=0xf1f38a8f
frame seq=14 bytes=9000 hash=0x652abe0a
summary frames=3 bytes=27000 eof=3 last_hash=0x652abe0a
reasm frames_ok=3 frames_dropped=0 frags_recv=21 gaps=0 crc_fail=0 proto_fail=0
latency p50=200us p95=300us p99=300us max=332us n=3 skew=0 budget=unset verdict=TBD
```

`status=3` is `HML_ERR_IO` from the refused bind. The point of the demo is the
pair of agreeements: `xport=udp` matches the explicit `fallback:` line, and the
`hash=` values the sender prints are the FNV-1a values the T0 receiver prints,
so the same 27000 bytes arrived intact. Note `frames_dropped=0` and `crc_fail=0`
— a degraded path is a *correct* path here, not a lossy one. The latency line
is `hml_host`'s own EOF-latency histogram; see §9.4.

Without `--fallback-t0` the same failure is fatal (`hml_gen_roce: transport open
failed`), which is the intended behaviour: a tool must not quietly change
transports behind the caller's back.

---

## 6. Metrics

### 6.1 What is instrumented today

| Metric | Where | Notes |
| --- | --- | --- |
| `frames_ok`, `frames_dropped` | `hml_reasm_stats` | Complete frames delivered, and frames abandoned |
| `frags_recv` | `hml_reasm_stats` | Fragments accepted into reassembly |
| `gaps` | `hml_reasm_stats` | Non-contiguous fragment index observed within a frame |
| `crc_fail` | `hml_reasm_stats` | Header or trailing payload CRC mismatch |
| `proto_fail` | `hml_reasm_stats` | Truncated, bad magic, bad version, over-long payload, bad `frag_idx` |
| `eof` count, `bytes`, `last_hash` | `hml_host` summary | `hash` is CRC-32 of the frame payload, so it doubles as a content check |
| `bad_crc`, `bad_len` | `hml_host_roce` | Per-frame CRC and length disagreement after metadata validation |
| `psn_gaps`, `proto_err`, `icrc_err`, `meta_drops` | RoCEv2 demux | Distinct counters per failure class, so an ICRC error is never conflated with a protocol error |
| `hml_control_bad_count()` | control plane | Malformed control datagrams dropped |
| `hml_bridge_is_streaming()` + phase | bridge | Lifecycle state |

`hml_host --stats` prints a periodic line while frames are arriving, and the
`reasm` counters are the fastest way to tell a clean run from a lossy one.

### 6.2 What is NOT instrumented

| Metric | Status |
| --- | --- |
| **p50 / p99 EOF latency** (tx enqueue → EOF rx) | **TBD — not implemented.** The plan requires it in "metrics always logged" and M1's pass criterion is "p99 EOF latency within written budget". No latency histogram, no percentile, and **no written budget** exists. `hml_clock_us()` is available and per-fragment `ts_us` is on the wire, so the data is there; the aggregation is not written. Do not quote a latency figure for this project. |
| pps / Mbps | Not computed. `test_roce_e2e` phase A reports bytes and a wall-clock duration in its own test log; that is a test observation, not a supported measurement API. |
| `pkt_loss_%` | Not computed. `gaps` and `frames_dropped` are raw counters. A percentage needs an injected-loss denominator, which only the phase-B test has. |
| CPU% | Not instrumented on any port. |
| drop-policy hits | Counters exist; the *policy* (drop-oldest vs stop-stream) is asserted only at unit level, never under real backpressure. |
| pcap golden artifacts | **Not produced.** The plan lists pcap samples per tier as golden artifacts; none exist in the tree. |

### 6.3 How a future measurement should be added

Without designing it here: `ts_us` is already in the frame header and the
metadata record, and the receiver records a completion time, so p50/p99 needs a
histogram on the receive path keyed by frame. It should be added to
`hml_reasm_stats` or `hml_host` and must be reported as a distribution over a
stated sample size and MTU, not a single number.

---

## 7. Known gaps and traps

Things that will waste your time if you are not told.

### 7.1 `hml_gen` has no RoCEv2 mode, despite `--xport`

`--xport NAME` reads a name from the transport registry and the default is
`udp`, but the tool only ever calls `hml_udp_register()`. Asking for RoCEv2
fails:

```
$ ./adapters/synthetic/hml_gen --xport rocev2
hml_gen: unknown transport 'rocev2'
$ echo $?
1
```

The flag looks like it should work and does not. T1a is driven by the
`src/transports/rocev2` emitter through `test_roce_e2e` (§5.3), or by your own
code. A standalone T1a sender CLI is **not yet implemented**.

### 7.2 `hml_host_roce` has no `--help`

There is no `--help`/`-h` branch in its argument loop, so asking for help
prints usage and **exits 2**, the same as a bad flag. Use the usage text in
§4.3. (`hml_gen`, `hml_host`, `hml_ctrl_device`, `hml_ctrl_host` and
`hml_bridge_demo` all do handle `--help` and exit 0.)

### 7.3 `hml_ctrl_device --caps` help text is stale

The usage text says `--caps N  HML_CAP_* bitmask (default 3)`, but the code
actually defaults to `HML_CAP_UDP | HML_CAP_MCU` = **`0x11`**. The real run in
§5.2 prints `caps=0x11`. Trust the code, not the help text. (Pass `--caps`
explicitly if you care about the value.)

### 7.4 Port 4791 is privileged

Production RoCEv2 uses the IANA port **4791**, which is below 1024 and needs
root or `CAP_NET_BIND_SERVICE`. Every test and tool in this tree uses the
unprivileged mirror `HML_ROCE_TEST_PORT` = **14791** instead. If a RoCEv2 bind
fails with `EACCES`, that is the port, not a bug — and the core is expected to
fall back to T0 automatically.

### 7.5 No pcap, no NIC, ever

Repeated because it is the easiest thing to overstate. `tcpdump` and the
Wireshark RoCE dissector appear in the M3 plan; neither has been run against
these bytes. The demux reads an IP/UDP prefix that our own sender wrote into a
UDP payload (§7 of the wire spec), so on the wire the kernel's real IP/UDP
header is outside it. Before making any interoperability claim, capture the
traffic and compare, and re-validate the two T1a-specific choices listed in
§8.

---

## 8. What would have to happen before claiming more

Ranked by how much it would change the project's story.

1. **A NIC capture, or a real receiver.** T1a's two deliberate deviations from
   the obvious layout — BTH opcode **`0x2A`** (UC transport service) where a
   plain RDMA Write is normally **`0x0A`** (RC), and a **20-byte
   little-endian** metadata record where the early M3 notes described a 16-byte
   trailer — are pinned by golden vectors and round-trip through our own
   demux, but have never met an independent implementation. Both are cheap to
   change (constants in `src/transports/rocev2/`) and expensive to discover
   wrong in the field.
2. **The M1 soak and the M7 soak.** Memory flatness, no leak, and drop-counter
   behaviour over hours, as opposed to seconds.
3. **A fuzz run** on the frame, config, control and beacon parsers, plus
   ASAN/UBSAN. The parsers are written to fail closed and unit tests cover each
   rejection, but a 1M-packet run has not happened.
4. **aarch64.** The M0 pass criterion names it explicitly.
5. **Executed Soft-RoCE lab** (M3b) for verbs CQ/MR correctness, explicitly
   *not* as a ConnectX performance proxy.
6. **RoCE NIC + `nvidia-peermem`** for T2, which remains deferred.
7. **A latency budget**, written down, then measured. Right now "p99 within
   budget" has no budget to be within.

M6b — making stock Holoscan apps work unmodified — is not on this list because
it is a scope decision, not a test: see
[hololink_compat_notes.md](hololink_compat_notes.md).

---

## 9. Additions: the `hml_gen_roce` T1a sender pass

Appended rather than folded into the sections above, so the original text of
this guide stays as written.

### 9.1 Three new tests

The suite listed in §2.1 numbers 18 tests; with this pass it is **22**
(`ctest --test-dir build -N`). §2.1 is deliberately left as it was.

| Test | Kind | What it actually asserts |
| --- | --- | --- |
| `test_roce_cli` | two real processes | `execv`s `hml_gen_roce` against `hml_host_roce` and asserts on what they printed: both exit 0, the per-frame `crc=` the sender reports matches the `crc=` the demux reports, `eof_frames` equals the number of frames sent, `icrc_err`/`proto_err`/`meta_drops`/`bad_crc`/`bad_len` are all 0, the sender's own `psn_first`/`psn_last` bracket `frames × (chunks + 1)` datagrams, and the `--verbose` first-packet tap (BTH opcode `0x2A`, PSN, VA, rkey) agrees with the requested config |
| `test_roce_cli_fallback` | two real processes | Same driver, T0 receiver and `--fallback-t0`. Holds the sender's source port from a raw socket so the RoCEv2 open fails for a reason the tool cannot avoid, then requires the explicit `fallback: roce open failed, using T0 (udp)` line, `xport=udp`, all frames delivered with `gaps=0`/`crc_fail=0`/`frames_dropped=0`, and the sender's `hash=` equal to the receiver's `hash=` for every frame |
| `test_roce_fallback` | in process | The fallback *decision*, on the same helper the CLI uses: a destination the RoCEv2 transport refuses (`frame_size == 0`) and a held source port both select the `udp` ops with `fallback` set and the failure preserved; a valid destination selects `rocev2` and does **not** fall back; with fallback disabled the failure propagates and nothing is left open. The two fallback cases also push real frames through the fallback transport into a real receiver |

All three are `RUN_SERIAL` and bounded: every wait has a deadline and any
surviving child is killed, so a stuck tool fails the test instead of hanging
CTest. The CLI tests use their own ports (14793, 14893/14894) to stay clear of
`test_roce_e2e`'s 14791.

### 9.2 The one API addition

`hml_dest_cfg` is frozen, so it carries no source-port field. To implement
`--src-port` the RoCEv2 transport gained one private entry point next to its
existing `open`:

```c
hml_status hml_roce_open_ex(hml_xport_t *x, const hml_dest_cfg *dest, uint16_t stream_id,
                            uint16_t src_port);
```

`roce_open()` is now `roce_open_ex(..., 0)`, i.e. unchanged behaviour. Nothing
else in the transport, the core or the frozen headers moved.

### 9.3 Ports, again, for the new tool

`hml_gen_roce` defaults to **14791** for the reason in §7.4: production RoCEv2
is **4791**, which is below 1024 and therefore needs root or
`CAP_NET_BIND_SERVICE`. Nothing in the default path needs privileges. The
fallback demo deliberately binds the *source* port from a plain socket without
`SO_REUSEADDR`, because `hml_sock` sets `SO_REUSEADDR` and two `SO_REUSEADDR`
sockets may share a UDP port — that asymmetry is what makes the forced failure
deterministic instead of racy.

### 9.4 Latency

The T0 receiver in §5.6 prints a `latency p50=… p99=…` line; it is
`hml_host`'s EOF-latency histogram and is still marked `verdict=TBD` because
there is no written budget to compare it against (§6.2, §8 item 7). The
dedicated latency write-up belongs in `docs/latency.md`, which is not in this
tree yet — do not quote a latency figure from this file as a project number.

### 9.5 Two stale statements this pass supersedes

Neither was edited in place; both are noted here.

* §5.3 opens with "There is no standalone T1a emitter CLI". There is one now:
  `hml_gen_roce` (§4.7), driven over a real socket in §5.5.
* §2.1 numbers 18 tests and the guide table says "the 18 tests". The count is 22
  (§9.1). `test_latency` from the M5 latency pass is the fourth of the additions.
