# The file-stream sensor adapter (`hml_filestream`)

A second `hml_sensor_ops` implementation, beside the synthetic one, so the demo
path is not synthetic-only. It replays frames from a plain file on disk instead
of a live sensor, and it is also the writer for the same format — so a capture
and a replay are the same operation seen from the two ends, and they compose:
record what a live sender emits, then put that recording back on the wire with
the frame lengths, the FNV-1a hashes and the CRC-16s unchanged.

Like every other adapter here it is deliberately thin. It moves payload bytes
between a `FILE *` and the HAL and never touches a socket: the framing stays in
`hml_stream`, the datagram shaping stays in `src/transports/udp`, reassembly and
both CRCs stay in `src/core` and `src/protocol`. What the adapter adds is a
length-delimited container and an integrity story for a file on a disk that may
have been lying around for a week.

* Library: `adapters/filestream/hml_filestream.{h,c}`
* CLI: `adapters/filestream/hml_filestream_cli.c`, installed as the
  `hml_filestream` binary
* Build: `adapters/filestream/CMakeLists.txt`, wired in by the existing guarded
  `add_subdirectory(adapters/filestream)` in the root `CMakeLists.txt`

## 1. File format

One record per frame, repeated to end of file. No header, no index, no trailer.

| off | size | field | encoding |
| --- | --- | --- | --- |
| 0 | 4 | magic | ASCII `HMLF` = `48 4d 4c 46` |
| 4 | 4 | `frame_len` | `u32` **little-endian** |
| 8 | `frame_len` | payload | exactly `frame_len` bytes of frame data |

`frame_len` is little-endian; the record header is a fixed 8 bytes. A file of
three 9000-byte frames is therefore `3 * (8 + 9000) = 27024` bytes, and its first
header is:

```
$ od -A d -t x1 -N 16 cap.hmlf
0000000 48 4d 4c 46 28 23 00 00 22 be e3 9d 99 4f 56 76
0000016
         \_ "HMLF"  \_ 0x2328 = 9000 LE  \_ first payload bytes
```

The payload is the *frame* — the reassembled bytes, with no HML header and no
per-fragment trailer — so a file is transport- and tier-agnostic. The same file
replays over T0 UDP and would replay over T1a, because the tier lives in the
transport, not in the record.

## 2. Integrity: what `hml_crc16` covers, and what it does not

The record header carries **no CRC field**, so integrity is two-layered and both
layers are reported rather than assumed.

**Layer 1 — the reader fails closed on the layout.** Anything that does not match
the record shape is rejected, named by record index, and never partially
delivered:

| Condition | Status | Message shape |
| --- | --- | --- |
| short record header (< 8 bytes) | `HML_ERR_CRC` | `record 7: truncated record header` |
| short payload (file ends mid-record) | `HML_ERR_CRC` | `record 1: payload truncated, 2984 of 9000 bytes present, crc=0xaea3 over the 2984 bytes read` |
| magic is not `HMLF` | `HML_ERR_PROTO` | `record 1: bad magic 58585858, expected "HMLF"` |
| `frame_len == 0` | `HML_ERR_PROTO` | `record 0: zero-length frame` |
| `frame_len` over the `--max-frame` limit | `HML_ERR_TOOLARGE` | `record 0: frame_len 9000 exceeds the 100 byte limit` |
| file cannot be opened | `HML_ERR_IO` | `cannot open 'x.hmlf': No such file or directory` |

A short payload is classified `HML_ERR_CRC` rather than `HML_ERR_IO` on
purpose: it is the record's integrity claim — "these `frame_len` bytes are
here" — that did not hold, and the CRC-16 of the bytes that *were* present is
printed with the error so the damage is visible.

**Layer 2 — `hml_crc16` is published, and that is where its real job is.**
`hml_crc16()` (CCITT-16, poly `0x1021`, init `0xFFFF`, from
`include/hml/hml_crc.h` — the same primitive the T0 datagram payload CRC uses)
is computed over every payload on read and on write, and reported twice:

* per frame, as `crc=0x…` on both the `send` and the `listen` line, so the value
  that came out of a file can be compared with the value that came off the wire
  without shipping the bytes;
* as a chained whole-file `file_crc=0x…` in each summary. `hml_crc16` is
  one-shot, so the chain folds a per-record digest — `(previous chain, this
  record's payload CRC, this record's length)` — instead of re-reading the file.
  It is reset on every `--repeat` rewind, so one pass of a soak run and a single
  plain pass produce comparable values.

**The honest limit.** Because the header has no CRC field, a payload whose
`frame_len` is intact but whose *contents* were altered after the file was
written is **not** detected by the reader alone. The reader has nothing to
compare against. What it does give you is the per-frame CRC and hash of whatever
it read, so `cmp` against a reference, or a diff of two `send` runs, catches it
immediately — the demo in §4 does exactly that (`cmp` on two files, hash
diffing between three processes). If you need the reader to self-reject
bit-rot, add a CRC field to the record header; the format is 8 bytes today
specifically because that is what was specified, and changing it is a
deliberate decision, not a bug fix.

## 3. CLI

```
hml_filestream send <file> [--host A.B.C.D] [--port N] [--frames N] [--repeat]
                     [--rate N] [--stream-id N] [--seq-start N] [--max-frame N]
hml_filestream listen <file> [--port N] [--frames N] [--repeat] [--timeout-ms N]
                      [--max-frame N]
```

| Option | Applies to | Default | Meaning |
| --- | --- | --- | --- |
| `<file>` | both | — | `send`: the HMLF file to replay. `listen`: the HMLF file to create (truncated). |
| `--host A.B.C.D` | send | `127.0.0.1` | destination IPv4 |
| `--port N` | both | `55101` (`HML_DATA_PORT`) | UDP data port. `listen` must be given one, since it has to bind. |
| `--frames N` | both | `0` = until the file ends | stop after N frames |
| `--repeat` | both | off | `send`: rewind at end of file and keep going. `listen`: keep listening after the frame budget instead of exiting. |
| `--rate N` | send | `0` = as fast as possible | frames per second |
| `--stream-id N` | send | `1` | stream id |
| `--seq-start N` | send | `0` | first fragment sequence number |
| `--max-frame N` | both | `1048576` (`HML_MAX_FRAME_SIZE`) | reject records longer than N |
| `--timeout-ms N` | listen | `0` = wait forever | give up after N ms with no datagram |

Output is one line per frame, in the shape `hml_gen` prints for the synthetic
adapter so the two can be diffed, plus a summary:

```
sent frame=0 bytes=9000 crc=0x75a3 hash=0x08...     (send)
rx   frame=0 seq=0  bytes=9000 crc=0x75a3 hash=0x08...   (listen)
sent frames=3 bytes=27000 file_crc=0x071e records=3 rewinds=0
rx frames=3 bytes=27000 dgrams=21 eof_only=0 file_crc=0x071e records=3
reasm frames_ok=3 frames_dropped=0 frags_recv=21 gaps=0 crc_fail=0 proto_fail=0
```

* `crc` = `hml_crc16` over the frame, i.e. the HMLF record's payload
* `hash` = FNV-1a 32, the same `hml_fnv1a32()` `hml_host` prints as `hash=`
* `seq` on the receive side is the **base** fragment sequence of the frame, so it
  advances by the fragment count (7 for a 9000-byte frame at 1428 B/fragment),
  not by one

Every failure path ends with a non-zero exit status and a message naming the
record. A write error, a malformed record, a short payload and an oversized
record all stop the run; none of them is reported as a clean finish.

## 4. Worked example

All four steps below run on one box, on loopback, with no hardware, no root and
no network. Ports are in the ephemeral range and every process is bounded by
`timeout 10`.

### 4.1 Generate a file by recording a live stream

```sh
timeout 10 ./build/adapters/filestream/hml_filestream listen cap.hmlf \
    --port 55321 --frames 3 --timeout-ms 3000 &
sleep 0.4
timeout 10 ./build/adapters/synthetic/hml_gen --port 55321 --frames 3 --size 9000 --seed 1
wait
```

```
# hml_gen (the synthetic sender)
sent frame=0 bytes=9000 hash=0xb6bc71f8
sent frame=1 bytes=9000 hash=0xf1f38a8f
sent frame=2 bytes=9000 hash=0x652abe0a

# hml_filestream listen (the capture)
rx frame=0 seq=0  bytes=9000 crc=0x75a3 hash=0xb6bc71f8
rx frame=1 seq=7  bytes=9000 crc=0x898c hash=0xf1f38a8f
rx frame=2 seq=14 bytes=9000 crc=0x3a92 hash=0x652abe0a
rx frames=3 bytes=27000 dgrams=21 eof_only=0 file_crc=0x071e records=3
reasm frames_ok=3 frames_dropped=0 frags_recv=21 gaps=0 crc_fail=0 proto_fail=0
```

Every FNV-1a hash the sender printed is the hash the receiver computed from the
bytes that arrived, and the frame lengths match. `cap.hmlf` is 27024 bytes.

### 4.2 Replay the file into the library's own receiver

```sh
timeout 10 ./build/host/hml_host/hml_host --port 55322 --frames 3 --verbose &
sleep 0.4
timeout 10 ./build/adapters/filestream/hml_filestream send cap.hmlf \
    --host 127.0.0.1 --port 55322
wait
```

```
# hml_filestream send (the file is the sensor now)
sent frame=0 bytes=9000 crc=0x75a3 hash=0xb6bc71f8
sent frame=1 bytes=9000 crc=0x898c hash=0xf1f38a8f
sent frame=2 bytes=9000 crc=0x3a92 hash=0x652abe0a
sent frames=3 bytes=27000 file_crc=0x071e records=3 rewinds=0

# hml_host (no adapter involved on this side at all)
frame seq=0  bytes=9000 hash=0xb6bc71f8
frame seq=7  bytes=9000 hash=0xf1f38a8f
frame seq=14 bytes=9000 hash=0x652abe0a
summary frames=3 bytes=27000 eof=3 last_hash=0x652abe0a
reasm frames_ok=3 frames_dropped=0 frags_recv=21 gaps=0 crc_fail=0 proto_fail=0
```

`file_crc=0x071e` is the same value in §4.1's writer and §4.2's reader, and the
three CRCs are the ones the file stored, replayed byte for byte.

### 4.3 A replay is a re-capture

`send` into `listen` reproduces the file exactly, so a capture/replay/capture
loop is stable and diffable:

```
rx frames=3 bytes=27000 dgrams=21 eof_only=0 file_crc=0x071e records=3
$ cmp cap.hmlf rt.hmlf && echo identical
identical
```

### 4.4 Soak: more frames than the file holds

`--repeat` rewinds at end of file; `--frames` is the budget, so the two compose
into a soak run over a short recording:

```sh
timeout 10 ./build/adapters/filestream/hml_filestream send cap.hmlf \
    --port 55324 --frames 5 --repeat
```

```
sent frame=0 bytes=9000 crc=0x75a3 hash=0xb6bc71f8
sent frame=1 bytes=9000 crc=0x898c hash=0xf1f38a8f
sent frame=2 bytes=9000 crc=0x3a92 hash=0x652abe0a
sent frame=3 bytes=9000 crc=0x75a3 hash=0xb6bc71f8
sent frame=4 bytes=9000 crc=0x898c hash=0xf1f38a8f
sent frames=5 bytes=45000 file_crc=0x69c7 records=5 rewinds=1
```

`rewinds=1` is the one pass over the 3-frame file; the loop stops on the frame
budget, not on end of file. Run it under a real duration with `--frames 0
--repeat` to soak indefinitely, and kill it with SIGINT or SIGTERM (both are
handled; an interrupted run exits non-zero rather than reporting success).

## 5. What this adapter is not

* **T0 only.** The transport is `hml_udp_ops()`. A file has no notion of a
  queue pair, and `send` deliberately does not expose `--xport`: reach T1a with
  `hml_gen --xport rocev2` or `hml_gen_roce` instead.
* **Not a frame validator.** `send` replays whatever the records say. The
  library's own parsers do the validating, and the adapter does not pre-parse
  frame headers to check them.
* **Not a random-access container.** Records are read strictly sequentially, and
  `--repeat` is a `fseek` to offset 0. There is no index, so a 1-hour capture
  cannot be seeked into; a capture split into per-run files is the intended
  pattern.
* **No bit-rot detection on intact-length payloads.** See §2. This is the one
  real gap, and it is a property of the specified record header, not of the
  implementation.
* **Not covered by `ctest`.** There is no `tests/` target for this adapter; the
  verification is the command-line round trip in §4, not an assertion in the
  suite. The adapter is covered by the build being clean under `-Werror` and by
  those runs, nothing more.

## 6. Related: `hml_gen --xport rocev2`

The same synthetic sender reaches T1a from one CLI. `--xport udp` is T0 (the
default) and `--xport rocev2` is T1a; the flag only picks which
`hml_xport_ops` the transport registry hands back, so the BTH/RETH/ICRC bytes
and the 20-byte end-of-frame metadata record are still built in exactly one
place, `src/transports/rocev2`. The RDMA window fields default to the ones
`hml_host_roce` uses, so the two interoperate with no flags at all:

```sh
timeout 10 ./build/host/roce_demux/hml_host_roce --frames 3 --verbose &
sleep 0.4
timeout 10 ./build/adapters/synthetic/hml_gen --xport rocev2 --frames 3 --size 9000
wait
```

```
# hml_gen --xport rocev2
sent frame=0 bytes=9000 hash=0xb6bc71f8 crc=0xd35dba0f
sent frame=1 bytes=9000 hash=0xf1f38a8f crc=0x031f6466
sent frame=2 bytes=9000 hash=0x652abe0a crc=0x535d1f7e
sent frames=3 xport=rocev2 port=14791

# hml_host_roce
hml_host_roce: listening udp/14791 window=0x100000 len=65536 frame=9000 rkey=0xdeadbeef icrc=on
frame seq=6  bytes=9000 crc=0xd35dba0f
frame seq=13 bytes=9000 crc=0x031f6466
frame seq=20 bytes=9000 crc=0x535d1f7e
stats pkts=24 icrc_err=0 writes=21 bytes=27000 eof_frames=3 psn_gaps=0 proto_err=0 meta_drops=0
host frames=3 bytes=27000 bad_len=0 bad_crc=0
```

`crc=` on the sender is the CRC-32 the frame's metadata record carries and the
receiver prints; `hash=` is the FNV-1a a T0 receiver would print for the same
frame. Port 14791 is the unprivileged mirror of the IANA RoCEv2 port 4791;
pass `--port 4791` as root to use the real one. Additional flags:
`--rkey`, `--window-base`, `--qp`, `--psn`, `--frame-size`; see
`hml_gen --help`.

This proves the packets are self-consistent with our own demux. It is **not**
evidence of interoperation with a real RoCEv2 NIC or a stock Holoscan
`RoceReceiverOp` — see `docs/testing.md` §8 for that gap.
