# Public API reference (`include/hml/*.h`)

Scope: the 15 headers under `include/hml/`, which is exactly what
`install(DIRECTORY include/hml ...)` installs. Every signature below is
transcribed from the header; nothing is inferred, renamed or invented.

Two of them are new since 0.2.0: `hml_export.h` (the visibility macros, below)
and `hml_partition.h` (the machine-checkable port contract). Neither adds a
runtime symbol; both are header-only and free of any libc dependency beyond
`<stdint.h>`, `<stddef.h>` and `<stdbool.h>`.

Headers that exist in the tree but are **not** public, and are not installed:
`src/control/hml_control.h`, `src/control/hml_client.h`,
`src/bridge/hml_bridge.h`, `src/bridge/hml_bridge_stub.h`,
`src/bridge/hml_bridge_control.h`, `src/accel/hml_accel.h`,
`src/accel/hml_accel_backend.h`, `host/hml_host/hml_host.h`,
`host/roce_demux/hml_roce_demux.h`, `host/roce_gen/hml_roce_gen.h`,
`src/transports/rocev2/hml_roce_priv.h`, `src/transports/udp/hml_udp_priv.h`,
`adapters/synthetic/hml_synth_priv.h`. They use `HML_API` in places but are
internal to the project.

## `hml_types.h` — status codes, version, linkage macro

Every other header includes this first.

```c
/* hml_types.h includes hml_export.h, which defines HML_API. */
#define HML_VERSION_STRING "0.2.0"
#define HML_VERSION_MAJOR 0
#define HML_VERSION_MINOR 2
#define HML_VERSION_PATCH 0
```

`HML_API` is no longer empty. It is defined in `hml_export.h` as:

| Platform | `HML_API` |
| --- | --- |
| GCC/Clang >= 4 | `__attribute__((visibility("default")))` |
| Windows/Cygwin, building the DLL | `__declspec(dllexport)` |
| Windows/Cygwin, consuming it | `__declspec(dllimport)` |
| `HML_NO_VISIBILITY` defined | *(empty)* |

This is what makes the shared arm usable: the shared objects are built with
`C_VISIBILITY_PRESET hidden` and `VISIBILITY_INLINES_HIDDEN ON`, so only
`HML_API` symbols are exported. Define `HML_NO_VISIBILITY` to opt out entirely
and keep the old no-decoration behaviour.

Two related macros:

* `HML_INLINE` — `static inline`, used for the header helpers in `hml_port.h`.
  It is deliberately *not* `static HML_API inline`: a visibility attribute on a
  `static` function is ignored by GCC, which under `-Werror=attributes` is a
  build failure. Some private headers under `src/bridge/` still use the old
  spelling; they are not ours to edit, so `hml_export.h` suppresses exactly that
  one diagnostic and nothing else.
* `HML_HIDDEN` — the inverse, for internals that must not be exported.

```c
typedef enum hml_status {
  HML_OK = 0,
  HML_ERR_ARG,       /* bad argument / null ptr */
  HML_ERR_NOMEM,     /* allocation or pool exhaustion */
  HML_ERR_IO,        /* underlying socket/port error */
  HML_ERR_PROTO,     /* protocol violation (bad magic/version/len) */
  HML_ERR_AGAIN,     /* would block / no data */
  HML_ERR_TIMEOUT,   /* deadline exceeded */
  HML_ERR_NOTSUP,    /* feature not available on this build/port */
  HML_ERR_BUSY,      /* resource busy, retry later */
  HML_ERR_CRC,       /* integrity check failed */
  HML_ERR_SEQ,       /* out-of-order or stale sequence */
  HML_ERR_TOOLARGE,  /* frame/datagram exceeds configured max */
  HML_ERR_STATE,     /* invalid operation for current state */
  HML_ERR_UNKNOWN    /* internal / unspecified */
} hml_status;
```

The numeric values are ABI and are used on the wire: the control server's
`HML_OP_ERROR` response carries one `hml_status` byte as its payload, and
`hml_client` reads it back into `hml_client_last_device_status()`.

The header is `extern "C"`-guarded, so C++ consumers need no wrapper.

## `hml_config.h` — build-time defaults

No functions. Four macros, each overridable with `-D` at build time.

| Macro | Default | Meaning |
| --- | --- | --- |
| `HML_DEFAULT_MTU` | `1500u` | Ethernet MTU the fragment size is sized for |
| `HML_DEFAULT_MAX_FRAME` | `(1u<<20)` | 1 MiB, matches `HML_MAX_FRAME_SIZE` |
| `HML_DEFAULT_FRAG_PAYLOAD` | `1428u` | matches `HML_MAX_FRAG_PAYLOAD` |
| `HML_LATENCY_BUDGET_US` | `40000u` | written p99 EOF-latency budget for the T0 loopback CI path, in microseconds. `0` disables the budget |

`HML_LATENCY_BUDGET_US` is a **loopback smoke-test threshold, not a product
target**. See [latency.md](latency.md) and
[backpressure.md](backpressure.md).

## `hml_crc.h` — checksums

```c
HML_API uint16_t hml_crc16(const void *data, size_t n);
HML_API uint32_t hml_crc32_ieee(const void *data, size_t n);
```

* `hml_crc16`: CCITT-16, poly `0x1021`, init `0xFFFF`, no final xor. Used over
  the frame datagram payload and over the control message. Check vector:
  `hml_crc16("123456789", 9) == 0x29B1`.
* `hml_crc32_ieee`: IEEE 802.3 CRC-32, the polynomial RoCEv2 ICRC uses. Coverage
  is the RoCEv2 rule — everything except the IP and UDP headers and the ICRC field
  itself. Check vector: `hml_crc32_ieee("123456789", 9) == 0xCBF43926`.

`hml_crc32_ieee` is the bitwise reference. The RoCEv2 emitter additionally
carries a table-driven twin (`hml_roce_crc32_update` / `_done` in
`src/transports/rocev2/hml_rocev2.c`) because the bitwise loop costs about
100 us per KiB on the reference host, which is far too slow for a 1.4 KiB
per-packet data path. `tests/m3/test_roce_hdr.c` pins the two against each
other, so they are bit-identical by test, not by assertion.

## `hml_port.h` — platform detection, clock, byte order

```c
HML_API uint64_t hml_clock_us(void);

HML_INLINE uint16_t hml_bswap16(uint16_t v);
HML_INLINE uint32_t hml_bswap32(uint32_t v);
HML_INLINE uint64_t hml_bswap64(uint64_t v);

HML_INLINE void     hml_put_be16(uint8_t *p, uint16_t v);
HML_INLINE uint16_t hml_get_be16(const uint8_t *p);
HML_INLINE void     hml_put_be32(uint8_t *p, uint32_t v);
HML_INLINE uint32_t hml_get_be32(const uint8_t *p);
HML_INLINE void     hml_put_be64(uint8_t *p, uint64_t v);
HML_INLINE uint64_t hml_get_be64(const uint8_t *p);
```

* `hml_clock_us()` is monotonic, per-device, in microseconds. **Exactly one port
  per binary may define it** — see [porting.md](porting.md). On POSIX it is
  `CLOCK_MONOTONIC`; there is no wall-clock or NTP discipline anywhere.
* The `hml_put_be*` / `hml_get_be*` helpers are byte-at-a-time, so they are safe
  on an unaligned pointer. All of them are `static inline` in the header: they
  cost nothing in code size and are not exported symbols.
* The header also defines exactly one of `HML_PORT_POSIX` or
  `HML_PORT_FREERTOS`, from an explicit `-D` or from the compiler's own macros
  (`__linux__` / `__APPLE__` / `__unix__` versus `FREERTOS` / `ESP32` /
  `ESP_PLATFORM`). The `HML_PORT_*` macros drive the duplicate-symbol guards in
  the port sources. They are the *only* place a platform conditional is allowed
  to influence a public header; `src/core/` has none, and `test_core_no_ifdef`
  enforces that.
* **`HML_NO_POSIX` is the bare-metal opt-out.** Define it and this header never
  auto-tags as `HML_PORT_POSIX`, so a cross-compiled firmware cannot pick up
  POSIX socket code from the host's `__linux__`. Combining `HML_NO_POSIX` with
  an explicit `HML_PORT_POSIX` is a configuration error and raises `#error`
  rather than silently picking one. A `-DHML_NO_POSIX=ON` install stamps it via
  `$<INSTALL_INTERFACE:>`; see [install.md](install.md) and
  [final_partitioning.md](final_partitioning.md).

## `hml_sock.h` — the UDP socket port contract

This is the second half of the frozen port interface, alongside
`hml_clock_us()`.

```c
typedef struct hml_sock hml_sock_t;

HML_API hml_status hml_sock_udp_create(hml_sock_t **out, uint32_t bind_ip, uint16_t bind_port,
                                       bool broadcast);
HML_API hml_status hml_sock_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port);
HML_API hml_status hml_sock_send(hml_sock_t *s, const void *buf, size_t n);
HML_API hml_status hml_sock_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n, int timeout_ms);
HML_API void hml_sock_close(hml_sock_t *s);
```

* IPv4 only. One `hml_sock_t` handles exactly one socket.
* `bind_port == 0` leaves the socket unbound (ephemeral source port). `broadcast`
  enables `SO_BROADCAST`; the discovery beacon is the only user.
* `hml_sock_set_dest` sets the default destination used by `hml_sock_send`.
* `hml_sock_recv`: `timeout_ms < 0` blocks, `0` polls, `> 0` is a bounded wait.
  Returns `HML_OK` with `*n > 0`, `HML_ERR_AGAIN` (poll found nothing) or
  `HML_ERR_TIMEOUT` (the wait expired), or `HML_ERR_IO`.
* `hml_sock_send` maps `EAGAIN` / `EWOULDBLOCK` to `HML_ERR_AGAIN`. That is the
  only backpressure signal the send path has — see
  [backpressure.md](backpressure.md).

Two contract facts a port must respect:

1. **`hml_sock_recv` does not report the peer address.** The control server
   therefore cannot learn where to answer, and must be told with
   `hml_control_cfg.reply_ip` / `reply_port`. See
   [control_security.md](control_security.md).
2. **A port may legitimately answer `HML_ERR_NOTSUP` for every socket call.**
   The FreeRTOS and ESP-IDF stubs do exactly that on a host build, and
   `test_ports_api` asserts it.

## `hml_protocol.h` — frame codec, destination config, constants

```c
HML_API void hml_proto_frame_fill(const hml_frame_hdr *h, uint8_t *dst);
HML_API hml_status hml_proto_frame_parse(const uint8_t *datagram, size_t len,
                                         hml_frame_hdr *out, const uint8_t **payload);
HML_API void hml_proto_cfg_fill(const hml_dest_cfg *cfg, uint8_t *dst);
HML_API hml_status hml_proto_cfg_parse(const uint8_t *src, size_t len, hml_dest_cfg *out);
```

Types:

```c
typedef struct hml_frame_hdr {
  uint16_t stream_id;
  uint32_t seq;
  uint16_t frag_idx;
  uint16_t frag_cnt;
  uint64_t ts_us;
  uint8_t  flags;        /* HML_FLAG_* */
  uint16_t payload_len;
} hml_frame_hdr;

typedef enum hml_xport_kind {
  HML_XPORT_UDP = 0,
  HML_XPORT_ROCEV2 = 1
} hml_xport_kind;

typedef struct hml_dest_cfg {
  hml_xport_kind kind;
  uint32_t ipv4;          /* network-byte-order dest address */
  uint16_t udp_port;      /* data port (UDP T0) or 4791 for RoCEv2 */
  uint16_t pad;
  uint32_t qp;            /* RoCEv2 Queue Pair number */
  uint32_t psn;           /* RoCEv2 initial PSN */
  uint32_t rkey;          /* RoCEv2 remote rkey */
  uint64_t remote_addr;   /* RoCEv2 remote memory window base VA */
  uint32_t frame_size;    /* bytes per EOF-delimited frame (implicit EOF for T1a) */
  uint16_t window_len;    /* remote window length in units of 4KiB */
  uint8_t  hop_limit;
  uint8_t  rsvd[5];
} hml_dest_cfg;
```

Constants:

| Macro | Value | Note |
| --- | --- | --- |
| `HML_PROTO_VERSION` | `1u` | wire protocol version, independent of the library version |
| `HML_MAGIC_FRAME` | `0x484d4c31u` | `"HML1"` |
| `HML_MAGIC_CTRL` | `0x484d4c43u` | `"HMLC"` |
| `HML_MAGIC_BEACON` | `0x484d4c42u` | `"HMLB"` |
| `HML_FLAG_START` | `0x01u` | |
| `HML_FLAG_END` | `0x02u` | this is what closes a frame |
| `HML_FRAME_HDR_SIZE` | `28` | |
| `HML_TRAIL_CRC_SIZE` | `2` | |
| `HML_MAX_FRAG_PAYLOAD` | `1428u` | |
| `HML_MAX_FRAG_SIZE` | `1458` | header + payload + trailing CRC |
| `HML_MAX_FRAME_SIZE` | `1u << 20` | 1 MiB |
| `HML_MAX_FRAGS` | `(HML_MAX_FRAME_SIZE / HML_MAX_FRAG_PAYLOAD + 1)` | 735 on this build |
| `HML_DATA_PORT` | `55101u` | |
| `HML_CTRL_PORT` | `55102u` | |
| `HML_DISC_PORT` | `55100u` | |
| `HML_ROCE_PORT` | `4791u` | IANA RoCEv2 UDP port; privileged, tests use 14791 |
| `HML_CFG_SIZE` | `40u` | serialized size of `hml_dest_cfg` |

Header field offsets, as an enum: `HML_OFF_MAGIC` 0, `HML_OFF_VERSION` 4,
`HML_OFF_FLAGS` 5, `HML_OFF_STREAM` 6, `HML_OFF_SEQ` 8, `HML_OFF_FRAG_IDX` 12,
`HML_OFF_FRAG_CNT` 14, `HML_OFF_TS` 16, `HML_OFF_PAYLOAD_LEN` 24,
`HML_OFF_HDR_CRC` 26, `HML_OFF_PAYLOAD` 28.

Behaviour that matters:

* `hml_proto_frame_fill` writes exactly `HML_FRAME_HDR_SIZE` bytes; `dst` must
  have room. It computes `hdr_crc16` over bytes `[0,26)`, i.e. everything before
  the CRC field.
* `hml_proto_frame_parse` validates magic, version, a declared
  `payload_len` of at most `HML_MAX_FRAG_PAYLOAD`, and the header CRC over bytes
  `[0,26)`. Returns `HML_ERR_CRC` for a CRC mismatch and `HML_ERR_PROTO` for
  everything else.
* **The `payload` argument switches the depth of validation.** With
  `payload != NULL` the parser additionally requires the datagram to be at least
  `28 + payload_len + 2` bytes and **verifies the trailing payload CRC-16**,
  returning `HML_ERR_CRC` on mismatch. With `payload == NULL` it is a
  structure-only check: the header CRC is still verified, but a truncated
  datagram is *not* rejected. `hml_reasm_feed_datagram` always passes a real
  pointer, so the reassembled path checks both CRCs.
* The header CRC covers only the header, and the payload CRC only the payload, so
  neither region is trusted blind.
* Both `hml_proto_cfg_fill` and `hml_proto_cfg_parse` operate on exactly
  `HML_CFG_SIZE` bytes, all fields big-endian.
* The byte-level wire spec, including exactly what every parser rejects, is
  [protocol_v1.md](protocol_v1.md).

## `hml_transport.h` — the pluggable transport vtable

```c
typedef struct hml_xport_ops hml_xport_ops;

typedef struct hml_xport {
  const hml_xport_ops *ops;
  void *impl;             /* transport private data (allocated by open) */
} hml_xport_t;

typedef struct hml_xport_ops {
  const char *name;       /* "udp", "rocev2" */
  hml_status (*open)(hml_xport_t *x, const hml_dest_cfg *dest, uint16_t stream_id);
  hml_status (*send_frag)(hml_xport_t *x, const hml_frame_hdr *hdr,
                          const uint8_t *payload, size_t payload_len);
  hml_status (*flush)(hml_xport_t *x);
  void (*close)(hml_xport_t *x);
} hml_xport_ops;

HML_API hml_status hml_transport_register(const hml_xport_ops *ops);
HML_API const hml_xport_ops *hml_transport_lookup(const char *name);
```

* `send_frag()` is called once per logical fragment. The transport owns datagram
  shaping: the UDP transport re-serializes the HML header; the RoCEv2 transport
  wraps the payload in BTH/RETH/ICRC instead.
* `flush()` forces buffered output out. It is a no-op for UDP; for RoCEv2 it is a
  safety net that emits a missing end-of-frame metadata write.
* `close()` releases `impl`.
* **The registry holds at most 8 entries** (`HML_TRANSPORT_SLOTS`), is
  registration-once, and is **not locked**. `hml_transport_register` rejects a
  duplicate pointer or a duplicate `name` with `HML_ERR_BUSY`, and a full
  registry with `HML_ERR_NOMEM`. `hml_transport_lookup` returns `NULL` for an
  unknown or empty name.
* A transport is **thread-affine to the stream that owns it**: v1 is
  single-threaded per stream, and `hml_stream` does not lock.

## `hml_stream.h` — the framer

```c
typedef struct hml_stream hml_stream_t;

typedef struct hml_stream_cfg {
  uint16_t stream_id;
  size_t   max_frame_bytes; /* clamp; <= HML_MAX_FRAME_SIZE */
  uint32_t seq_start;       /* initial seq for fragments emitted by this stream */
  uint32_t mtu_effective;   /* informational; >= HML_MAX_FRAG_PAYLOAD+overhead */
} hml_stream_cfg;

typedef void (*hml_eof_cb)(void *ctx, uint16_t stream_id, uint32_t frame_seq,
                           const uint8_t *frame, size_t frame_bytes,
                           const hml_frame_hdr *final_hdr);

HML_API hml_status hml_stream_open(hml_stream_t **out, const hml_stream_cfg *cfg,
                                   hml_xport_t *xport);
HML_API hml_status hml_stream_close(hml_stream_t *s);
HML_API hml_status hml_stream_send_frag(hml_stream_t *s, const uint8_t *data, size_t len,
                                        bool first, bool last, uint64_t ts_us);
HML_API hml_status hml_stream_flush_eof(hml_stream_t *s, uint64_t ts_us);
HML_API hml_status hml_stream_set_eof_cb(hml_stream_t *s, hml_eof_cb cb, void *ctx);
HML_API uint32_t hml_stream_next_seq(const hml_stream_t *s);
```

Semantics, from `src/core/hml_stream.c`:

* **Fragmentation is the caller's job.** Every `hml_stream_send_frag()` emits
  exactly one fragment. The stream does not split a buffer for you.
* `first == true` sets `HML_FLAG_START` and resets the fragment index to 0 and
  the running frame byte count. `last == true` sets `HML_FLAG_END`.
* `frag_cnt` is **never signalled** (always 0 on the wire), so a receiver must
  rely on `HML_FLAG_END` to close a frame. `frag_idx` is still emitted correctly.
* `len > HML_MAX_FRAG_PAYLOAD` is `HML_ERR_TOOLARGE`, as is a frame that would
  exceed `cfg->max_frame_bytes` (0 means `HML_MAX_FRAME_SIZE`).
* **On a failed send, no counters advance**, so the caller can retry a
  byte-identical packet.
* `hml_stream_open` **adopts** the already-open `hml_xport_t *`: closing the
  stream closes the transport. It requires `ops->send_frag` and `ops->close` to
  be non-NULL, else `HML_ERR_ARG`.
* `hml_stream_flush_eof` emits a zero-payload `HML_FLAG_END` fragment — an
  "EOF-only flush" that wakes the consumer with `frame_bytes == 0` without
  contributing any fragment.
* `hml_stream_set_eof_cb` stores a callback that the **send** path never
  invokes. It exists for symmetry with `hml_reasm_set_eof_cb`; the EOF signal in
  this library is a wire event, not a local call.
* `mtu_effective` is informational only; nothing validates it.

## `hml_reasm.h` — the receiver-side reassembler

```c
typedef struct hml_reasm_ctx hml_reasm_ctx_t;

typedef struct hml_reasm_stats {
  uint32_t frames_ok;
  uint32_t frames_dropped;
  uint32_t frags_recv;
  uint32_t gaps;        /* missing fragment detected */
  uint32_t crc_fail;
  uint32_t proto_fail;
} hml_reasm_stats;

typedef void (*hml_reasm_eof_cb)(void *ctx, uint16_t stream_id, uint32_t frame_seq,
                                 const uint8_t *frame, size_t frame_bytes,
                                 const hml_frame_hdr *final_hdr);

HML_API hml_status hml_reasm_open(hml_reasm_ctx_t **out, size_t max_frame_bytes);
HML_API size_t hml_reasm_ctx_size(void);
HML_API hml_status hml_reasm_open_buffers(hml_reasm_ctx_t **out, void *ctx_mem, size_t ctx_mem_size,
                                         uint8_t *staging_buf, uint8_t *gather_buf,
                                         size_t capacity);
HML_API void hml_reasm_close(hml_reasm_ctx_t *r);
HML_API hml_status hml_reasm_feed_datagram(hml_reasm_ctx_t *r, const uint8_t *dg, size_t len);
HML_API void hml_reasm_set_eof_cb(hml_reasm_ctx_t *r, hml_reasm_eof_cb cb, void *ctx);
HML_API void hml_reasm_reset(hml_reasm_ctx_t *r);
HML_API void hml_reasm_get_stats(const hml_reasm_ctx_t *r, hml_reasm_stats *st);
HML_API void hml_reasm_get_latency(const hml_reasm_ctx_t *r, hml_latency *out);
```

* `hml_reasm_open_buffers`: allows embedded MCU callers or callers who wish to
  avoid heap allocation to pass caller-supplied staging and gather buffers.
  If `ctx_mem` is provided (at least `hml_reasm_ctx_size()` bytes), reassembly
  operates with zero heap allocations.


Semantics, from `src/core/hml_reasm.c`:

* **There are exactly 2 slots** (`HML_REASM_SLOTS`), so **two streams can be
  reassembled concurrently**. A third stream evicts the slot with the lowest
  `seq_base`, counting a `frames_dropped` if that frame was still in flight.
* Frame identity is `seq == seq_base + frag_idx`, with
  `seq_base = seq - frag_idx` derived from the first fragment that arrives. This
  makes a lost first fragment harmless and stops two frames silently merging
  when both boundaries are lost. The subtraction wraps at 2^32 like the
  sequence itself.
* A fragment that violates the identity check starts a new frame window; the old
  one is dropped and counted.
* Fragments are staged in **arrival order** and gathered into a second buffer in
  **index order** when the frame closes. A single-fragment frame is already in
  order and is delivered from the staging buffer.
* **v1 requires uniform fragmentation**: every fragment but the last must have
  the same payload length. A frame that violates this is dropped, with `gaps++`
  and `frames_dropped++`, and is never delivered.
* A frame that reaches `HML_FLAG_END` with missing indices becomes *pending* and
  waits for stragglers, which is what makes reversed delivery work. The loss is
  confirmed (`gaps++` and `frames_dropped++`) as soon as a datagram proves the
  stragglers will not arrive — that is, a fragment of the next frame.
* A zero-payload `HML_FLAG_END` datagram with nothing in flight is an
  **EOF-only flush**: the callback fires with `frame_bytes == 0` and
  `frames_ok` is **not** incremented, so a host can count EOF without a data
  frame.
* Exactly **one callback per assembled frame**.
* `hml_reasm_open(r, 0)` allocates **`2 * HML_MAX_FRAME_SIZE` = 2 MiB of heap**
  (staging plus gather), plus a fixed per-context state of roughly 9 KiB (two
  slots, each with a 92-byte presence bitmap and a 735-entry offset table and
  length table). This is the MCU blocker: there is no static or caller-buffer
  variant. See [limitations.md](limitations.md).
* `hml_reasm_feed_datagram` counts `crc_fail` for any `HML_ERR_CRC` from the
  parse (header CRC or payload CRC) and `proto_fail` for every other parse
  rejection, including an out-of-range `frag_idx` or
  `frag_idx >= frag_cnt`.
* `hml_reasm_reset` clears every in-flight frame but keeps the buffers and the
  counters.
* `hml_reasm_get_stats` and `hml_reasm_get_latency` are NULL-safe: a NULL
  context yields a zeroed struct.

## `hml_latency.h` — the EOF-latency histogram

```c
#define HML_LAT_BUCKETS 64
#define HML_LAT_DEFAULT_BUCKET_US 50u /* covers 0..3150us in 50us steps */

typedef struct hml_latency {
  uint64_t n;          /* recorded samples (skew excluded) */
  uint64_t sum_us;
  uint64_t min_us;
  uint64_t max_us;
  uint32_t bucket_us;  /* 0 = use HML_LAT_DEFAULT_BUCKET_US */
  uint32_t skew_samples;  /* negative deltas: clock domains differ */
  uint32_t overflow;      /* samples above the top bucket (kept in max_us) */
  uint32_t hist[HML_LAT_BUCKETS];
} hml_latency;

HML_API hml_status hml_latency_init(hml_latency *l, uint32_t bucket_us);
HML_API hml_status hml_latency_record(hml_latency *l, uint64_t us);
HML_API hml_status hml_latency_percentile(const hml_latency *l, unsigned pct, uint64_t *out_us);
HML_API void hml_latency_reset(hml_latency *l);
```

Rules, from `src/core/hml_latency.c`:

* Bucket `i` covers `[i*b, (i+1)*b)` for `i` in `0..62`. The top bucket `63` is
  the tail: a sample `>= b*63` increments `overflow` and is kept only in
  `n` / `sum_us` / `min_us` / `max_us`, never in `hist`.
* `hml_latency_init(l, 0)` selects `HML_LAT_DEFAULT_BUCKET_US`, and sets
  `min_us = UINT64_MAX` so an empty histogram never reports 0 as its minimum.
* Percentiles are **nearest-rank**: the smallest bucket boundary `v` such that at
  least `pct`% of samples are `<= v`. `pct == 0` returns `min_us`, `pct == 100`
  returns `max_us`.
* If the rank walk terminates in the top bucket, the result is the **exact
  `max_us`**, not a bucket boundary. The tail is never an interpolation. This is
  why `p99` frequently equals `max` in practice.
* `n == 0` returns `HML_ERR_AGAIN`; `pct > 100` or a NULL argument returns
  `HML_ERR_ARG`.
* `hml_latency_reset` zeroes everything, preserving `bucket_us`.

The clock-domain caveat and the measured numbers are in
[latency.md](latency.md); how the reassembler taps it is in
[backpressure.md](backpressure.md).

## `hml_sensor.h` — the sensor HAL vtable

```c
typedef struct hml_sensor hml_sensor_t;

typedef struct hml_sensor_ops {
  const char *name;
  hml_status (*open)(hml_sensor_t *s, void *param);
  hml_status (*acquire)(hml_sensor_t *s, uint8_t *dst, size_t cap, size_t *out);
  hml_status (*release)(hml_sensor_t *s);
  void (*close)(hml_sensor_t *s);
} hml_sensor_ops;

typedef struct hml_sensor {
  const hml_sensor_ops *ops;
  void *impl;
} hml_sensor_t;
```

This header is **types only — there is not one function declared in it**. It is
the Emulator-style `open` / `acquire` / `release` shape, kept sensor-agnostic so
no device-specific code lives in core. `acquire()` fills `dst` with one
payload-chunk and writes the count to `*out`; `release()` signals the end of that
unit. `cap` is the caller's buffer size and the implementer is responsible for
not exceeding it.

`adapters/synthetic/hml_synthetic.c` is the one implementation in-tree; its
header (`hml_synth_priv.h`) is private and not installed.

## `hml_ctrl.h` — control message codec

```c
#define HML_CTRL_VERSION 1u
#define HML_CTRL_MAX_PAYLOAD 512u

#define HML_OP_NOP        0x00
#define HML_OP_PING       0x01
#define HML_OP_SET_DEST   0x02
#define HML_OP_GET_DEST   0x03
#define HML_OP_START      0x04
#define HML_OP_STOP       0x05
#define HML_OP_RESET      0x06
#define HML_OP_GET_CAPS   0x07
#define HML_OP_STATUS     0x08
#define HML_OP_POKE       0x09
#define HML_OP_READBACK   0x0A
#define HML_OP_ERROR      0xFF

#define HML_CTRL_FLAG_REQ 0x0001
#define HML_CTRL_FLAG_RESP 0x0002
#define HML_CTRL_FLAG_ACK  0x0004
#define HML_CTRL_FLAG_ERR  0x0008

typedef struct hml_ctrl_msg {
  uint8_t  op;
  uint16_t flags;
  const uint8_t *payload; /* points into a backing buffer if parsed */
  uint16_t payload_len;
} hml_ctrl_msg;

HML_API void hml_ctrl_fill(const hml_ctrl_msg *m, uint8_t *dst, uint16_t max_dst, uint16_t *out_n);
HML_API hml_status hml_ctrl_parse(const uint8_t *buf, size_t len, hml_ctrl_msg *out,
                                  const uint8_t **payload);
```

Codec behaviour, from `src/control/hml_ctrl_codec.c`:

* Layout: `[magic u32 BE][version u8][op u8][flags u16 BE][len u16 BE][rsvd u16 BE][payload][crc16 BE]`,
  a **fixed 12-byte header** and a total size of `12 + len + 2`. `rsvd` is always
  written as 0.
* `hml_ctrl_fill` is **fail-safe**: if `max_dst` is too small it writes **nothing
  at all** and leaves `*out_n == 0`. A caller that ignores `*out_n` cannot send a
  truncated message.
* `hml_ctrl_parse` requires `len` to equal `12 + payload_len + 2` **exactly** —
  truncation and trailing junk are both rejected. It also rejects a wrong magic,
  a wrong version, and a `payload_len` above `HML_CTRL_MAX_PAYLOAD`, and returns
  `HML_ERR_CRC` on a CRC mismatch.
* The CRC covers bytes `[0, 12 + payload_len)`, i.e. header plus payload.
* `hml_ctrl_msg.payload` after a successful parse **points into `buf`**; it is
  never copied. `buf` must outlive the message.
* This header defines the *codec* only. The control **server** and **client**
  live in `src/control/` and are not part of the installed surface. The
  transport, trust model and gaps: [control_security.md](control_security.md).

## `hml_discovery.h` — beacon codec and capability bits

```c
#define HML_DISC_VERSION 1u
#define HML_DISC_SIZE 28u

#define HML_CAP_UDP    (1u<<0)
#define HML_CAP_ROCEV2 (1u<<1)
#define HML_CAP_BRIDGE (1u<<2)
#define HML_CAP_GPU    (1u<<3)
#define HML_CAP_MCU    (1u<<4)

typedef struct hml_beacon {
  uint8_t  version;
  uint8_t  flags;
  uint8_t  ttl;
  uint32_t uuid;
  uint32_t caps;
  uint32_t fw_ver;
  uint64_t ts_us;
} hml_beacon;

HML_API void hml_beacon_fill(const hml_beacon *b, uint8_t *dst);
HML_API hml_status hml_beacon_parse(const uint8_t *buf, size_t len, hml_beacon *out);
```

Codec behaviour, from `src/control/hml_discovery.c`:

* Layout: `[magic u32 BE][version u8][flags u8][ttl u8][rsvd u8][uuid u32 BE][caps u32 BE][fw_ver u32 BE][ts_us 48-bit BE][crc16 BE]`,
  **exactly `HML_DISC_SIZE == 28` bytes**. `hml_beacon_fill` always writes 28
  bytes and gives no length parameter.
* **`ts_us` is encoded as 48 bits, not 64.** The frozen header comment lists a
  full `u64`, which would make the record 30 bytes, but `HML_DISC_SIZE` is fixed
  at 28 and the CRC must cover exactly the first 26 bytes. 48 bits of
  microseconds is about 8.9 years of monotonic uptime. The field order and
  offsets match the documentation; only the width is narrower.
* `hml_beacon_parse` accepts **only** `len == HML_DISC_SIZE`; anything else is
  `HML_ERR_PROTO`. It validates magic and CRC (`HML_ERR_CRC` on mismatch) and
  zeroes `out` before filling it.
* The capability bits are a **self-declared advertisement** by the device. They
  are not verified by anything. In particular `HML_CAP_ROCEV2` and
  `HML_CAP_GPU` are claims, not measurements.
* The beacon is broadcast to `255.255.255.255` by default, which means it is
  visible to every host on the broadcast domain. See
  [control_security.md](control_security.md).

## `hml_export.h` — visibility and linkage macros

Included by `hml_types.h`, so every consumer gets it transitively. It defines
`HML_API`, `HML_HIDDEN` and `HML_INLINE`; see the `hml_types.h` section above
for the per-platform expansion and the reason `HML_INLINE` exists separately
from `HML_API`.

It also carries a narrow diagnostic suppression: a visibility attribute on a
`static` function is ignored by GCC, so a private header written as
`static HML_API inline` fails under `-Werror=attributes`. Two such headers exist
under `src/bridge/` and are not ours to edit, so the suppression is scoped to
that one warning. Nothing else is suppressed.

## `hml_partition.h` — the port contract, made checkable

Header-only. It describes what a firmware port owes hml, in a form a linker can
verify instead of a comment a human has to notice.

```c
/* the seven symbols, in one X-macro list */
#define HML_PORT_FN_LIST(X)                                                        \
  X(uint64_t, hml_clock_us, (void))                                                \
  X(hml_status, hml_sock_udp_create, (hml_sock_t **out, uint32_t bind_ip,          \
                                       uint16_t bind_port, bool broadcast))         \
  X(hml_status, hml_sock_set_dest, (hml_sock_t *s, uint32_t ip, uint16_t port))    \
  X(hml_status, hml_sock_send, (hml_sock_t *s, const void *buf, size_t n))         \
  X(hml_status, hml_sock_recv, (hml_sock_t *s, void *buf, size_t cap,              \
                                size_t *n, int timeout_ms))                        \
  X(void,      hml_sock_close, (hml_sock_t *s))

/* expands to the six function-pointer fields of hml_port_contract */
typedef struct hml_port_contract { HML_PORT_FN_LIST(HML_PORT_FIELD) } hml_port_contract;
```

`cmake/hml_firmware.cmake` configures `hml_port_contract.c` from
`cmake/hml_port_contract.c.in`, whose body is a table of real function
addresses:

```c
const hml_port_contract hml_port_contract_check = {
  HML_PORT_FN_LIST(HML_PORT_ENTRY)
};
```

Add that generated file to your port's sources and a missing port symbol is a
link error naming it, rather than a crash on the first datagram. A port must
not define `hml_port_contract_check` itself — that is a duplicate symbol. The
object is unreferenced data, so `-u hml_port_contract_check` may be needed
against a linker with `--gc-sections`.

Three informational macros:

| Macro | Value | Set by |
| --- | --- | --- |
| `HML_LINKAGE_NAME` | `"static"` / `"shared"` | `HML_PARTITION_SHARED`, which the `_shared` targets put on their interface |
| `HML_HAS_POSIX_PORT` | `0` / `1` | `hml_port.h`'s platform tag |
| `HML_LIBC_REQUIREMENTS` | a string | nothing; it is documentation |

`HML_HAS_POSIX_PORT` derives from `HML_PORT_POSIX`, which only `hml_port.h`
defines, so this header includes `hml_port.h` itself. Without that include the
answer would depend on include order — a real bug this header used to have, and
the examples now `_Static_assert` the value they require.

## Quick index

| Header | Declares |
| --- | --- |
| `hml_types.h` | `hml_status`, version macros (includes `hml_export.h` for `HML_API`) |
| `hml_export.h` | `HML_API`, `HML_HIDDEN`, `HML_INLINE` (no runtime symbols) |
| `hml_partition.h` | `HML_PORT_FN_LIST`, `hml_port_contract`, `HML_LINKAGE_NAME`, `HML_HAS_POSIX_PORT` (no runtime symbols) |
| `hml_config.h` | build-time defaults, `HML_LATENCY_BUDGET_US` |
| `hml_crc.h` | `hml_crc16`, `hml_crc32_ieee` |
| `hml_port.h` | `hml_clock_us`, byte-order helpers, port detection |
| `hml_sock.h` | `hml_sock_*` (port contract) |
| `hml_protocol.h` | frame/config codecs, `hml_frame_hdr`, `hml_dest_cfg`, wire constants |
| `hml_transport.h` | `hml_xport` / `hml_xport_ops` vtable, registry |
| `hml_stream.h` | framer: `hml_stream_open` … `hml_stream_next_seq` |
| `hml_reasm.h` | reassembler: `hml_reasm_open` … `hml_reasm_get_latency` |
| `hml_latency.h` | `hml_latency` histogram and percentiles |
| `hml_sensor.h` | `hml_sensor` / `hml_sensor_ops` types only, no functions |
| `hml_ctrl.h` | control codec, opcodes, flags |
| `hml_discovery.h` | beacon codec, `HML_CAP_*` |
