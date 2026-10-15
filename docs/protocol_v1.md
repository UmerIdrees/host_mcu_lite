# Host MCU Lite (`hml`) — Wire Protocol v1 (Normative)

This is the normative description of every byte `Host MCU Lite` (`hml`) puts on a
wire. It is transcribed from the frozen headers in `include/hml/` and the
codecs in `src/`, not from a design intent:

| Section | Frozen source of truth |
| --- | --- |
| [2. Frame datagram (T0)](#2-frame-datagram-t0-udp) | `include/hml/hml_protocol.h`, `src/protocol/hml_protocol.c` |
| [3. Destination config](#3-destination-config-hml_dest_cfg-40-bytes) | `src/protocol/hml_protocol.c` |
| [4. EOF model](#4-eof-model) | `include/hml/hml_protocol.h`, `src/core/hml_stream.c`, `src/core/hml_reasm.c` |
| [5. Control message](#5-control-message-path-a) | `include/hml/hml_ctrl.h`, `src/control/hml_ctrl_codec.c`, `src/control/hml_control.c` |
| [6. Discovery beacon](#6-discovery-beacon) | `include/hml/hml_discovery.h`, `src/control/hml_discovery.c` |
| [7. RoCEv2 T1a](#7-rocev2-t1a-datagram) | `src/transports/rocev2/hml_roce_priv.h`, `src/transports/rocev2/hml_rocev2.c`, `host/roce_demux/hml_roce_demux.c` |
| [8. CRC primitives](#8-crc-primitives) | `include/hml/hml_crc.h`, `src/protocol/hml_crc.c` |

Every byte-offset table below is also pinned by a CTest golden vector; the test
that owns each table is named in its caption. Run
`ctest --test-dir build --output-on-failure` to check them. Build and test
instructions are in [testing.md](testing.md).

**Status: pre-1.0. `HML_PROTO_VERSION` is 1 and the ABI is not frozen** — see
[§9 Versioning policy](#9-versioning-policy).

---

## 1. Version and magic table

Three record types, each with its own 4-byte ASCII magic and its own version
constant. A receiver that sees a magic it does not know rejects the datagram.

| Record | Magic symbol | Wire value (u32 BE) | ASCII | Version symbol | Value | Parser |
| --- | --- | --- | --- | --- | --- | --- |
| Data frame (T0, and the payload basis of T1a) | `HML_MAGIC_FRAME` | `0x484D4C31` | `HML1` | `HML_PROTO_VERSION` | 1 | `hml_proto_frame_parse` |
| Control message | `HML_MAGIC_CTRL` | `0x484D4C43` | `HMLC` | `HML_CTRL_VERSION` | 1 | `hml_ctrl_parse` |
| Discovery beacon | `HML_MAGIC_BEACON` | `0x484D4C42` | `HMLB` | `HML_DISC_VERSION` | 1 | `hml_beacon_parse` |

Library version (independent of all three): `HML_VERSION_STRING` = `"0.1.0"`,
`HML_VERSION_MAJOR/MINOR/PATCH` = `0/1/0`, in `include/hml/hml_types.h`.

Default UDP ports (`include/hml/hml_protocol.h`):

| Constant | Port | Used by |
| --- | --- | --- |
| `HML_DISC_PORT` | 55100 | discovery beacon (UDP broadcast) |
| `HML_DATA_PORT` | 55101 | T0 framed data |
| `HML_CTRL_PORT` | 55102 | Path A control request/response |
| `HML_ROCE_PORT` | 4791 | IANA RoCEv2 UDP port. **Privileged** (< 1024): needs root or `CAP_NET_BIND_SERVICE`. Every test uses the unprivileged mirror `HML_ROCE_TEST_PORT` = 14791. |

---

## 2. Frame datagram (T0 UDP)

A frame is fragmented; one **fragment** is one UDP datagram. All multi-byte
fields are **big-endian**.

```
off  size  field
  0     4  magic          u32   HML_MAGIC_FRAME
  4     1  version        u8    HML_PROTO_VERSION
  5     1  flags          u8    HML_FLAG_START | HML_FLAG_END
  6     2  stream_id      u16
  8     4  seq            u32   one value per fragment
 12     2  frag_idx       u16   0-based, per frame
 14     2  frag_cnt       u16   0 = "not signalled" (see §4)
 16     8  ts_us          u64   source monotonic microseconds, per device
 24     2  payload_len    u16   <= HML_MAX_FRAG_PAYLOAD (1428)
 26     2  hdr_crc16      u16   CRC-16/CCITT over bytes [0, 26)
 28   len  payload
28+len   2  payload_crc16 u16   CRC-16/CCITT over the payload bytes ONLY
```

`HML_FRAME_HDR_SIZE` = 28, `HML_TRAIL_CRC_SIZE` = 2, so
**datagram size = 28 + payload_len + 2**. The offsets are also exported as the
`HML_OFF_*` enum so the codec and the tests cannot drift.

Two CRCs, two different jobs (pinned by `tests/m0_m1/test_crc.c` and
`tests/m0_m1/test_frame_codec.c`):

* `hdr_crc16` protects the header. The producer computes it over bytes
  `[0, 26)` — everything up to but excluding the CRC field itself.
* `payload_crc16` protects the payload. Coverage is the `payload_len` bytes
  only, **not** the header. A payload byte that changes with a matching trailing
  CRC still parses, by design: the header is intact, so the record is
  well-formed, and the two CRCs together catch every single-bit flip in either
  region.

### MTU arithmetic

At MTU 1500 with IPv4 + UDP there are `1500 − 20 − 8 = 1472` bytes of UDP
payload available, and a frame datagram needs `28 + payload_len + 2`:

```
28 + 1428 + 2 = 1458  <= 1472   (14 bytes of slack)
```

`HML_MAX_FRAG_PAYLOAD` is therefore 1428 and `HML_MAX_FRAG_SIZE` is 1458. The
same numbers appear on the wire as `1458 + 20 + 8 = 1486` bytes on an IPv4
link, so **no T0 datagram is ever fragmented by IP**.

### Limits

| Constant | Value | Meaning |
| --- | --- | --- |
| `HML_MAX_FRAG_PAYLOAD` | 1428 | max payload bytes per datagram |
| `HML_MAX_FRAG_SIZE` | 1458 | `28 + 1428 + 2` |
| `HML_MAX_FRAME_SIZE` | 1 MiB (`1 << 20`) | max frame; `hml_reasm_open` / `hml_stream_open` clamp to it |
| `HML_MAX_FRAGS` | 735 | `HML_MAX_FRAME_SIZE / HML_MAX_FRAG_PAYLOAD + 1` |
| `HML_DEFAULT_MTU` | 1500 | `include/hml/hml_config.h` |

### Golden vector

`tests/m0_m1/test_frame_codec.c` pins this exact 28-byte header for
`stream_id=0x1234, seq=0xDEADBEEF, frag_idx=3, frag_cnt=7,
ts_us=0x0102030405060708, flags=START|END, payload_len=0`:

```
48 4D 4C 31  01 03 12 34  DE AD BE EF  00 03 00 07
01 02 03 04 05 06 07 08  00 00  D3 73
```

`hdr_crc16` = `0xD373` and `hml_crc16(header, 26) == 0xD373` is asserted, so the
CRC coverage boundary is tested, not just the value.

---

## 3. Destination config (`hml_dest_cfg`, 40 bytes)

`HML_CFG_SIZE` = 40. This is the RDMA-window description the control plane
carries, and the only thing the transports and the host demux need besides the
port. All multi-byte fields big-endian.

| off | size | field | C member | Notes |
| --- | --- | --- | --- | --- |
| 0 | 1 | `kind` | `hml_xport_kind` | `0` = `HML_XPORT_UDP`, `1` = `HML_XPORT_ROCEV2`. Any other value is rejected. |
| 1 | 1 | `pad0` | — | **Must be 0** on the wire; non-zero is rejected. |
| 2 | 2 | `udp_port` | `udp_port` | data port for UDP, or `dest->udp_port` (4791 in production) for RoCEv2 |
| 4 | 4 | `ipv4` | `ipv4` | network byte order, i.e. exactly what `hml_get_be32` yields |
| 8 | 4 | `qp` | `qp` | RoCEv2 queue-pair number |
| 12 | 4 | `psn` | `psn` | RoCEv2 initial PSN (only the low 24 bits are used) |
| 16 | 4 | `rkey` | `rkey` | RoCEv2 remote key |
| 20 | 8 | `remote_addr` | `remote_addr` | RoCEv2 remote window base VA, u64 |
| 28 | 4 | `frame_size` | `frame_size` | bytes per EOF-delimited frame. **Zero is refused** by the RoCEv2 transport and by the host demux, because the T1a meta slot is derived from it. |
| 32 | 2 | `window_len` | `window_len` | remote window length in **units of 4 KiB** |
| 34 | 1 | `hop_limit` | `hop_limit` | becomes the IPv4 TTL in T1a; 0 means "use 64" |
| 35 | 1 | `pad1` | `rsvd[0]` | **Must be 0**; non-zero is rejected. |
| 36 | 4 | `rsvd[1..4]` | `rsvd[1..4]` | **Must be 0**; any non-zero byte is rejected. |

Trailing bytes beyond `HML_CFG_SIZE` in a received buffer are ignored, not
fatal (`hml_proto_cfg_parse` only requires `len >= 40`).

Golden vector, `tests/m0_m1/test_frame_codec.c`:
`kind=UDP, ipv4=127.0.0.1, udp_port=55101, qp=0x11223344, psn=0x55667788,
rkey=0x99AABBCC, remote_addr=0x0011223344556677, frame_size=9000,
window_len=64, hop_limit=64`:

```
00 00 D7 3D  7F 00 00 01  11 22 33 44  55 66 77 88
99 AA BB CC  00 11 22 33 44 55 66 77  00 00 23 28
00 40        40 00        00 00 00 00
```

---

## 4. EOF model

**A frame ends where a `HML_FLAG_END` fragment ends.** There is no length
prefix, no trailer and no in-band "bytes remaining" field: the receiver learns
the frame is complete from the flag, and it learns how long it is by summing
the payloads it actually received.

Three cases, all implemented:

1. **Data fragment with `HML_FLAG_END`.** The normal close. For a multi-fragment
   frame the final fragment carries both payload and the flag.
2. **EOF-only flush.** A fragment with `payload_len == 0` and
   `HML_FLAG_END` set, with no data of its own. It contributes no bytes; the
   reassembler fires the EOF callback with `frame_bytes == 0` so a consumer can
   be woken without a data frame. `frames_ok` is deliberately **not**
   incremented for it, so "count EOF" and "count complete frames" stay
   separable. Emitted by `hml_stream_flush_eof()` and by `hml_gen --eof-only`.
3. **Implicit frame boundary (T1a).** The RoCEv2 path has no flags on the wire,
   so a frame is closed by the dedicated EOF metadata write — see
   [§7.4](#74-eof-metadata-record-20-bytes).

`frag_cnt` is **not signalled** by the framer in v1: `hml_stream_send_frag()`
always writes `frag_cnt = 0`, meaning "not signalled". `frag_idx` *is* correct
and is what reassembly, gap accounting and out-of-order tolerance use. A
receiver must therefore rely on `HML_FLAG_END`, not on `frag_cnt`.

### Reassembly rules the wire implies

Implemented in `src/core/hml_reasm.c` and pinned by
`tests/m0_m1/test_reasm.c`:

* **Layout rule.** All fragments but the last carry the same payload length
  `chunk`, so fragment *i* lands at `i * chunk` and the frame is
  `(n-1) * chunk + len(last)` bytes. A frame that does not satisfy this is
  dropped and counted (`gaps++`, `frames_dropped++`), never delivered.
* **Frame identity.** `hml_stream` hands out one sequence number per fragment,
  so every fragment of a frame satisfies `seq == base + frag_idx`. `base` is
  derived from the first fragment seen (`base = seq - frag_idx`), which works
  for reversed delivery and for a lost first fragment. A fragment that violates
  the check belongs to another frame and is never merged: it opens a new window
  and the old one is dropped. This is what stops two frames from silently
  mixing when both of their boundary markers are lost, and it wraps cleanly at
  2^32.
* **Late stragglers.** A frame that reaches `HML_FLAG_END` with missing indices
  is held `pending` rather than dropped, so reversed delivery still
  reassembles. The loss is confirmed — `gaps++` and `frames_dropped++` — as
  soon as a datagram proves the stragglers will not arrive, i.e. a fragment of
  the next frame.
* **Duplicates are idempotent.** A repeated `frag_idx` is ignored; a frame is
  never delivered twice and no bytes are doubled.
* **Concurrency.** Two in-flight frame windows (`HML_REASM_SLOTS` = 2), one per
  `stream_id`; the oldest window is the victim if a third stream appears.
* **Overflow.** A frame larger than the reassembly cap is *poisoned*: the
  remaining fragments are swallowed and the frame is dropped, not delivered
  truncated.

---

## 5. Control message (Path A)

One opcode-based request/response datagram. Fixed 12-byte header, then payload,
then a trailing CRC-16. All multi-byte fields big-endian.
`HML_CTRL_MAX_PAYLOAD` = 512, so the largest control datagram is
`12 + 512 + 2 = 526` bytes.

| off | size | field | Notes |
| --- | --- | --- | --- |
| 0 | 4 | `magic` | `HML_MAGIC_CTRL` (`HMLC`) |
| 4 | 1 | `version` | `HML_CTRL_VERSION` = 1 |
| 5 | 1 | `op` | `HML_OP_*` |
| 6 | 2 | `flags` | `HML_CTRL_FLAG_*` |
| 8 | 2 | `len` | payload length, `<= HML_CTRL_MAX_PAYLOAD` |
| 10 | 2 | `rsvd` | always 0 — pad that keeps the header fixed at 12 bytes |
| 12 | `len` | `payload` | opcode-specific, see below |
| 12+len | 2 | `crc16` | `hml_crc16` over bytes `[0, 12+len)`, i.e. header **and** payload |

Total datagram = `12 + len + 2`. `hml_ctrl_parse` requires the buffer length to
equal that total **exactly** — a short datagram and trailing junk are both
rejected (fail closed).

### Opcodes and payload shapes

| Op | Symbol | Request payload | Response payload |
| --- | --- | --- | --- |
| `0x00` | `HML_OP_NOP` | — | — (ACK) |
| `0x01` | `HML_OP_PING` | opaque (client sends a 4-byte token) | echoed verbatim (4 bytes) |
| `0x02` | `HML_OP_SET_DEST` | `hml_dest_cfg`, exactly `HML_CFG_SIZE` = 40 bytes | — (ACK) |
| `0x03` | `HML_OP_GET_DEST` | — | `hml_dest_cfg`, 40 bytes; `ERR` + 1-byte `HML_ERR_STATE` if no dest was ever set |
| `0x04` | `HML_OP_START` | — | — (ACK) |
| `0x05` | `HML_OP_STOP` | — | — (ACK) |
| `0x06` | `HML_OP_RESET` | — | — (ACK); clears the stored dest and calls the stop callback |
| `0x07` | `HML_OP_GET_CAPS` | — | 12 bytes: `uuid` u32 \| `caps` u32 \| `fw_ver` u32 |
| `0x08` | `HML_OP_STATUS` | — | 1 byte: `0` stopped, `1` streaming |
| `0x09` | `HML_OP_POKE` | `addr` u32 \| `val` u32 | `addr` u32 \| `rval` u32 |
| `0x0A` | `HML_OP_READBACK` | `addr` u32 | `addr` u32 \| `val` u32 |
| `0xFF` | `HML_OP_ERROR` | — (device-generated) | 1 byte = `hml_status` |

An unknown opcode is answered with `op = HML_OP_ERROR`, `FLAG_ERR`, and a
1-byte `HML_ERR_NOTSUP` payload, so a client can never hang waiting for a reply
that will not come. Wrong payload length for `SET_DEST` / `POKE` / `READBACK`
is `ERR` + `HML_ERR_PROTO`; a missing or refusing callback is `ERR` +
`HML_ERR_NOTSUP`; an address the device rejects is `ERR` + `HML_ERR_STATE`.

### Flags and response semantics

| Bit | Symbol | Meaning |
| --- | --- | --- |
| 0x0001 | `HML_CTRL_FLAG_REQ` | set on a request |
| 0x0002 | `HML_CTRL_FLAG_RESP` | set on a response |
| 0x0004 | `HML_CTRL_FLAG_ACK` | success; set with `RESP` |
| 0x0008 | `HML_CTRL_FLAG_ERR` | failure; set with `RESP` |

**Responses echo `op` and `flags` because the frozen header carries no
sequence field.** The 12-byte header has no request id — the u16 at offset 10
is `rsvd` and is always zero. A client therefore correlates a reply to a
request *by opcode alone*: `hml_client.c` accepts a datagram only if it parses,
carries `FLAG_RESP`, and has either the same `op` as the outstanding request or
`op == HML_OP_ERROR`. Anything else arriving in the meantime is discarded and
the wait continues until the deadline, which is `HML_ERR_TIMEOUT`.

Consequences, stated rather than hidden: the control plane is strictly
serialised (one outstanding request per client), and two clients talking to one
device at once can in principle see each other's replies. A sequence field is
the fix and it is deliberately not in v1 — the header is frozen and
`HML_CTRL_HDR_SIZE` is asserted at 12 by
`tests/m2/test_ctrl_codec.c`.

Malformed control datagrams (bad magic, bad version, wrong length, bad CRC) are
**dropped silently**: no response, no state change, counted in
`hml_control_bad_count()`. The device cannot learn the sender's address through
`hml_sock_recv()`, so responses go to the `reply_ip` / `reply_port` configured
at open time — the client must bind a known port and the device must be told
it.

### Golden vector

`tests/m2/test_ctrl_codec.c`: `op = HML_OP_PING`,
`flags = REQ|ACK (0x0005)`, `len = 0`.

```
header  48 4D 4C 43  01 01  00 05  00 00  00 00
crc16   16 58
```

---

## 6. Discovery beacon

28 bytes, `HML_DISC_SIZE`. Broadcast to `HML_DISC_PORT` (55100). All
multi-byte fields big-endian.

| off | size | field | Notes |
| --- | --- | --- | --- |
| 0 | 4 | `magic` | `HML_MAGIC_BEACON` (`HMLB`) |
| 4 | 1 | `version` | `HML_DISC_VERSION` = 1 |
| 5 | 1 | `flags` | reserved, 0 |
| 6 | 1 | `ttl` | hop budget; always 1 for a direct LAN peer |
| 7 | 1 | `rsvd` | reserved, 0 |
| 8 | 4 | `uuid` | device-unique id |
| 12 | 4 | `caps` | `HML_CAP_*` bitmask |
| 16 | 4 | `fw_ver` | `major<<16 \| minor<<8 \| patch` |
| 20 | 6 | `ts_us` | **48-bit** big-endian, low 48 bits of the u64 monotonic clock |
| 26 | 2 | `crc16` | `hml_crc16` over bytes `[0, 26)` |

`HML_CAP_*` bits: `HML_CAP_UDP` 1<<0, `HML_CAP_ROCEV2` 1<<1,
`HML_CAP_BRIDGE` 1<<2, `HML_CAP_GPU` 1<<3, `HML_CAP_MCU` 1<<4.

### Why `ts_us` is 48 bits, not 64

The frozen header comment lists a `u64 ts_us`, which would make the record
**30 bytes** and the CRC would have to cover 28 of them. Two constraints win:

1. `HML_DISC_SIZE` is fixed at **28**, and a fixed-size record is what lets a
   discovery listener bound and dispatch on length alone.
2. The CRC must cover *exactly* the first 26 bytes, which only works if
   `ts_us` occupies 6.

So `ts_us` is encoded 48-bit big-endian. The cost is 2^48 microseconds of
monotonic uptime ≈ 8.9 years, which is far past any beacon use — and the
record stays exactly 28 bytes. `hml_beacon_parse` masks the read value with
`0x0000FFFFFFFFFFFF` and accepts **only** `len == HML_DISC_SIZE` (a longer
buffer is a protocol error, not a "beacon with padding").

Note the split of responsibility: `hml_beacon_parse` checks magic, length and
CRC but **not** `version`; `hml_client_discover()` compares
`b.version != HML_DISC_VERSION` and ignores the beacon if it differs.

### Golden vector

`tests/m2/test_discovery.c`: `version=1, flags=0, ttl=1, uuid=42, caps=3,
fw=0x00010000, ts_us=0x000001000002`.

```
48 4D 4C 42  01 00 01 00  00 00 00 2A  00 00 00 03
00 01 00 00  00 00 01 00 00 02  AB 47
```

`crc16` = `0xAB47` over bytes `[0, 26)`.

---

## 7. RoCEv2 T1a datagram

The T1a emitter is `src/transports/rocev2/hml_rocev2.c`. It is modelled on the
HSB Emulator `RoCEv2Transmitter`: **no libibverbs, no ConnectX**, just a UDP
socket carrying a hand-rolled IP/UDP/BTH/RETH/ICRC datagram. Total size is
`60 + payload_len`, `payload_len <= HML_ROCE_MAX_PAYLOAD` = 1440, so
`60 + 1440 = 1500` and nothing fragments at MTU 1500.

| off | size | field | encoding |
| --- | --- | --- | --- |
| 0 | 1 | ver/IHL | `0x45` — IPv4, IHL 5, no options |
| 1 | 1 | DSCP/ECN | `0x00` |
| 2 | 2 | IP total length | BE, `60 + payload_len` |
| 4 | 2 | identification | BE, +1 per emitted datagram |
| 6 | 2 | flags / fragment offset | `0x4000` = don't fragment |
| 8 | 1 | time to live | `dest->hop_limit` (0 ⇒ 64) |
| 9 | 1 | protocol | 17 (UDP) |
| 10 | 2 | IP header checksum | ones-complement 16-bit, **computed** |
| 12 | 4 | source IPv4 | BE, `127.0.0.1` when the dest is loopback, else `0.0.0.0` |
| 16 | 4 | destination IPv4 | BE, `dest->ipv4` |
| 20 | 2 | UDP source port | BE, **mirrors** the destination port |
| 22 | 2 | UDP destination port | BE, `dest->udp_port` (4791 in production) |
| 24 | 2 | UDP length | BE, `40 + payload_len` |
| 26 | 2 | UDP checksum | `0` — legal for IPv4 (RFC 768) |
| 28 | 12 | BTH | see below |
| 40 | 16 | RETH | see below |
| 56 | *n* | RDMA write payload | |
| 56+*n* | 4 | ICRC | **little-endian**, see below |

### 7.1 BTH — 12 bytes at offset 28

Field positions are the real InfiniBand Base Transport Header layout, not a
project-invented order.

| BTH rel. off | size | field | value written |
| --- | --- | --- | --- |
| 0 | 1 | OpCode | `0x2A` = transport-service UC (`0b001 << 5`) \| RDMA (`0x0A`). The host demux also accepts `0x0A` (RC). |
| 1 | 1 | flags | `0x00` — RDMA Write: SE=0, DNC=0, Fenced=0 |
| 2 | 1 | reserved | `0x00` |
| 3 | 3 | PSN | **24-bit, big-endian** — low 24 bits of the PSN counter, incremented once per emitted datagram, masked with `0x00FFFFFF` |
| 6 | 2 | AETH | `0x0000` — SYN-ACK, no syndrome |
| 8 | 4 | reserved | `0x00000000` |

### 7.2 RETH — 16 bytes at offset 40

| RETH rel. off | size | field | value written |
| --- | --- | --- | --- |
| 0 | 8 | Virtual Address | BE u64 = `dest->remote_addr + <offset in the frame>` |
| 8 | 4 | Remote Key (rkey) | BE u32 = `dest->rkey` |
| 12 | 4 | Remote Segment Length | BE u32 = `payload_len`, **D bit = 0** |

### 7.3 ICRC — 4 bytes at offset 56+n

`ICRC = hml_crc32_ieee(BTH .. end of payload)`. Coverage **excludes** the IP and
UDP headers and the ICRC field itself, which is the RoCEv2 rule. The 32-bit
value is transmitted **little-endian** (InfiniBand puts the CRC on the wire
least-significant byte first) while every other multi-byte field here is
big-endian. The emitted value is bit-identical to `hml_crc32_ieee()`; a
table-driven twin exists only for speed, and
`tests/m3/test_roce_hdr.c` pins the two against each other, including the
canonical `hml_crc32_ieee("123456789") == 0xCBF43926` anchor.

The IP header checksum is *not* optional here: the emitter computes the
ones-complement 16-bit sum over the 20-byte IPv4 header and
`tests/m3/test_roce_hdr.c` checks it against an independent reference
implementation.

**Source-IP note.** `hml_sock` is an ordinary `SOCK_DGRAM` socket, so on the
wire the *kernel* supplies its own outer IPv4/UDP header and this whole buffer —
including the crafted IP/UDP bytes — rides as the UDP payload. The receiver
therefore gets this exact buffer back from `hml_sock_recv()` and the demux
strips the 20/8-byte IP/UDP prefix of it. The crafted IP header is
byte-exact and is what the golden-vector tests assert; it is also exactly what
a raw-socket / `IP_HDRINCL` sender would put on the wire.

### 7.4 EOF metadata record (20 bytes)

There is no trailer and no implicit length, so the receiver cannot know a frame
is complete. T1a therefore reserves a **meta slot**: one dedicated 20-byte RDMA
WRITE to

```
VA = dest->remote_addr + dest->frame_size
```

immediately past the data window. The address is derived purely from
`hml_dest_cfg`, so the host demux recognises it with **zero out-of-band state
and no extra control message**. Exactly one metadata packet per frame is
emitted, on the `HML_FLAG_END` data fragment; `flush()` is only a safety net for
a stream that sets END without a trailing data fragment.

The record is serialized **little-endian** — it is host-local RDMA memory, not a
wire struct. `HML_ROCE_META_LEN` = 20.

| off | size | field |
| --- | --- | --- |
| 0 | 4 | `magic` = `HML_MAGIC_FRAME` |
| 4 | 2 | `stream_id` |
| 6 | 2 | `flags` (`HML_FLAG_*` of the last fragment) |
| 8 | 4 | `frame_seq` |
| 12 | 4 | `frame_bytes` |
| 16 | 4 | `frame_crc32` = CRC-32/IEEE over the assembled frame payload |

`4 + 2 + 2 + 4 + 4 + 4 = 20`. (The "16 bytes" figure in the early M3 notes was
a miscount; the explicit field list is authoritative.)

The demux delivers a frame only when the magic matches, `frame_bytes` is within
both `frame_size` and the window, and the CRC-32 of the assembled window
matches; otherwise the window is zeroed and `meta_drops` is counted. A meta
whose `frame_seq` repeats the last delivered frame is ignored, so a frame is
never delivered twice.

**Both T1a deviations are correct by construction and by golden-vector test,
not confirmed on a NIC.** Re-validate them against a ConnectX packet capture
before making any interoperability claim — see
[soft_roce_lab.md §9](soft_roce_lab.md).

---

## 8. CRC primitives

From `include/hml/hml_crc.h` and `src/protocol/hml_crc.c`:

| Function | Polynomial | Init | Final xor | Coverage in this protocol | Check vector |
| --- | --- | --- | --- | --- | --- |
| `hml_crc16` | CCITT `0x1021` (MSB-first) | `0xFFFF` | none | frame `hdr_crc16` over `[0,26)`, frame `payload_crc16` over the payload only, control `crc16` over `[0, 12+len)`, beacon `crc16` over `[0,26)` | `hml_crc16("123456789") == 0x29B1` |
| `hml_crc32_ieee` | IEEE 802.3, reflected `0xEDB88320` | `0xFFFFFFFF` | `~crc` | RoCEv2 ICRC (BTH..end-of-payload, stored little-endian) and the T1a frame CRC in the meta record | `hml_crc32_ieee("123456789") == 0xCBF43926` |

Empty-buffer values, asserted by `tests/m0_m1/test_crc.c`:
`hml_crc16(NULL, 0) == 0xFFFF` and `hml_crc32_ieee(NULL, 0) == 0x00000000`.

---

## 9. Versioning policy

Three independent version tags plus one library tag, all currently at 1 (frame,
control, beacon) and `0.1.0` (library):

| Tag | Where | Bump when |
| --- | --- | --- |
| `HML_PROTO_VERSION` | byte 4 of every frame datagram | the frame header, the EOF model or the CRC coverage changes |
| `HML_CTRL_VERSION` | byte 4 of every control message | the control header, opcode set or payload shape changes |
| `HML_DISC_VERSION` | byte 4 of every beacon | the beacon layout or the meaning of a `HML_CAP_*` bit changes |
| `HML_VERSION_*` / `HML_VERSION_STRING` | library, `include/hml/hml_types.h` | any library release, including one that changes no bytes on a wire |

Rules the code already enforces:

* **Library and protocol versions move independently.** A patch release that
  changes no wire byte does not touch `HML_PROTO_VERSION`, and a wire change
  does not have to be a major library release. M7's pass criterion is exactly
  this: "version tags protocol + library independently".
* **A version mismatch is a hard reject, never a soft accept.** `hml_proto_frame_parse`
  and `hml_ctrl_parse` compare the version byte against the compiled constant
  and return `HML_ERR_PROTO` on any difference. There is no forward-compat
  path and no "try the next version" logic. A v2 peer therefore fails closed
  against a v1 build rather than being misparsed.
* **Magic identifies the record type, version identifies the revision.** The
  three magics (`HML1`, `HMLC`, `HMLB`) let a listener classify a datagram
  before it parses it, which is what lets one UDP port family carry three
  different layouts. A new *record type* gets a new magic; a new *revision* of
  an existing record keeps its magic and bumps its version constant.
* **Mismatched layout fields are reserved, not reused.** `hml_dest_cfg` has
  three reserved zero regions (`pad0`, `pad1`, `rsvd[1..4]`) and the control
  header has a `rsvd` u16. `hml_proto_cfg_parse` *rejects* a non-zero byte in
  any of them, so a future v2 that starts using them is immediately visible as
  a protocol error to a v1 peer instead of being silently misread. That is
  deliberate: fail loud, not fail compatible.

**Not frozen yet.** `0.1.0` is a preview. The C ABI (struct layout, symbol
names, enum values) may change in `0.x`; the M7 milestone is where the symbol
freeze is meant to happen. Nothing in this repository should assume the
`hml_*` ABI is stable before a tag after M7.

---

## 10. Rejected on parse

Every parser in this library fails closed: a datagram that does not match the
spec exactly is dropped, counted, and never delivered. There is no partial
accept anywhere.

### Frame datagram — `hml_proto_frame_parse` → `src/core/hml_reasm.c`

| Condition | Return | Counter |
| --- | --- | --- |
| `datagram == NULL` or `out == NULL` | `HML_ERR_ARG` | — |
| `len < 28` (truncated header) | `HML_ERR_PROTO` | `reasm.proto_fail` |
| `magic != HML_MAGIC_FRAME` | `HML_ERR_PROTO` | `reasm.proto_fail` |
| `version != HML_PROTO_VERSION` | `HML_ERR_PROTO` | `reasm.proto_fail` |
| `payload_len > 1428` | `HML_ERR_PROTO` | `reasm.proto_fail` |
| `hdr_crc16` mismatch | `HML_ERR_CRC` | `reasm.crc_fail` |
| `payload` requested but `len < 28 + payload_len + 2` (truncated body) | `HML_ERR_PROTO` | `reasm.proto_fail` |
| `payload_crc16` mismatch | `HML_ERR_CRC` | `reasm.crc_fail` |
| `frag_idx >= HML_MAX_FRAGS`, or `frag_cnt != 0 && frag_idx >= frag_cnt` | `HML_ERR_PROTO` | `reasm.proto_fail` |
| frame does not satisfy the uniform-chunk layout rule | — | `reasm.gaps` + `reasm.frames_dropped` |
| frame larger than the reassembly cap | — | `reasm.frames_dropped` (poisoned, never delivered) |

Note: a caller that passes `payload == NULL` gets a structure-only check — magic,
version, length, header CRC — and the trailing CRC is then not verified.

### Destination config — `hml_proto_cfg_parse`

| Condition | Return |
| --- | --- |
| `len < 40` | `HML_ERR_PROTO` |
| `kind` is not 0 or 1 | `HML_ERR_PROTO` |
| `pad0` (offset 1) non-zero | `HML_ERR_PROTO` |
| `pad1` (offset 35) non-zero | `HML_ERR_PROTO` |
| any of `rsvd[1..4]` (offsets 36..39) non-zero | `HML_ERR_PROTO` |
| `len > 40` (trailing bytes) | accepted, extras ignored |

### Control message — `hml_ctrl_parse` / `hml_control.c`

| Condition | Result |
| --- | --- |
| `len < 14` (not even an empty message) | `HML_ERR_PROTO` → dropped, `bad_count++` |
| `magic != HML_MAGIC_CTRL` | `HML_ERR_PROTO` → dropped, `bad_count++` |
| `version != HML_CTRL_VERSION` | `HML_ERR_PROTO` → dropped, `bad_count++` |
| `len > 512` | `HML_ERR_PROTO` → dropped, `bad_count++` |
| `len != 12 + len_field + 2` (truncated **or** trailing junk) | `HML_ERR_PROTO` → dropped, `bad_count++` |
| `crc16` mismatch | `HML_ERR_CRC` → dropped, `bad_count++` |
| unknown `op` | **not dropped**: answered `op = HML_OP_ERROR`, `FLAG_ERR`, payload = `HML_ERR_NOTSUP` |
| wrong payload length for `SET_DEST` / `POKE` / `READBACK` | answered `ERR` + `HML_ERR_PROTO`, no state change |
| refused address / callback | answered `ERR` + `HML_ERR_STATE` or `HML_ERR_NOTSUP` |
| request with no reply port configured | response is built and then not sent (silent) |

### Discovery beacon — `hml_beacon_parse` / `hml_client_discover`

| Condition | Result |
| --- | --- |
| `len != 28` (shorter **or** longer) | `HML_ERR_PROTO` |
| `magic != HML_MAGIC_BEACON` | `HML_ERR_PROTO` |
| `crc16` mismatch | `HML_ERR_CRC` |
| `version != HML_DISC_VERSION` | **not checked by the codec**; `hml_client_discover` ignores the beacon and keeps waiting |

### RoCEv2 T1a demux — `hml_roce_rx_poll` / `host/roce_demux/hml_roce_demux.c`

Checked in this order, so an integrity failure is never masked by a later
protocol check.

| Condition | Counter |
| --- | --- |
| `n < 20` | `proto_err` |
| IP version ≠ 4, `IHL < 5`, `IHL > 60`, or `n < IHL + 8` (options are skipped, not refused) | `proto_err` |
| IP protocol ≠ 17 | `proto_err` |
| UDP destination port ≠ `cfg->udp_port` | `proto_err` |
| UDP length field < 8 | `proto_err` |
| BTH+RETH+ICRC does not fit (`paylen < 32`) | `proto_err` |
| **ICRC mismatch** (payload bit flip *or* ICRC bit flip) | `icrc_err` |
| BTH OpCode not `0x2A` (UC) and not `0x0A` (RC) | `proto_err` |
| RETH length ≠ actual payload length (no immediate, no intra-datagram padding) | `proto_err` |
| RETH rkey ≠ `cfg->rkey` (check active when `cfg->rkey != 0`) | `proto_err` |
| VA below `window_base`, or a write that runs past `window_len_bytes`, and VA is not exactly the meta slot | `proto_err` |
| PSN goes **backwards** (>= 2^23 delta) | silently ignored, **no counter** — a stale or duplicated packet is not an error |
| PSN jumps **forward** by more than 1 | `psn_gaps++`, the packet is still processed |
| meta write length ≠ 20 | `meta_drops`, window zeroed |
| meta magic ≠ `HML_MAGIC_FRAME` | `meta_drops`, window zeroed |
| meta `frame_bytes` > `frame_size` or > `window_len_bytes` | `meta_drops`, window zeroed |
| meta frame CRC-32 ≠ CRC of the assembled window | `meta_drops`, window zeroed |
| meta `frame_seq` equals the last delivered frame's | ignored — a frame is never delivered twice |

`hml_roce_rx_open` refuses up front: `window_len_bytes == 0`,
`window_len_bytes > 16 MiB`, `udp_port == 0`, and `frame_size == 0` (which
would alias the meta slot onto the first data write). The emitter
(`roce_open`) refuses `frame_size == 0` for the same reason, and propagates any
open failure so the core can fall back to T0.
