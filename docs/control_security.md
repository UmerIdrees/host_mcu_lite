# Control plane: trust model and security posture

**The short version: the control plane is completely unauthenticated and
unencrypted. Anyone who can reach UDP 55102 on a device can reprogram its
destination and read and write its registers. Do not expose an HML device to an
untrusted network, and do not treat `hml_ctrl_probe` as a debugging tool you can
point at a production device.**

This document is the honest security inventory for `v0.2.0-ship`. It states
what is actually implemented, what is explicitly absent, and what that means.

---

## 1. Scope: three channels, one trust level

HML has three UDP channels. Two of them are untrusted-by-network design (the
data plane) and one is a plain-text remote-management channel that is **not**
safe on an untrusted network.

| Channel | Port | Bind | Auth | Encryption | Integrity |
| --- | --- | --- | --- | --- | --- |
| Data (`hml_gen`, `hml_host`) | `HML_DATA_PORT` 55101 | `HML_IP_ANY` | none | none | CRC-16 per fragment |
| Control (`hml_control`, `hml_ctrl_probe`) | `HML_CTRL_PORT` 55102 | `HML_IP_ANY` | **none** | **none** | CRC-16 per message |
| Discovery beacon | `HML_DISC_PORT` 55100 | broadcast to 255.255.255.255 | none | none | CRC-16 |

Ports are defined in `include/hml/hml_protocol.h:48`. The bind address is
**`HML_IP_ANY` (0.0.0.0)** for both the control server
(`src/control/hml_control.c:77`) and the discovery socket, so a device listens on
every interface it has, including any Wi-Fi or USB-network interface. There is no
option to bind control to loopback only.

A grep of the whole tree for `auth`, `token`, `tls`, `ssl`, `encrypt`, `hmac`,
`nonce`, `secret` and `password` finds **no authentication, encryption or secret
handling of any kind.** The only "token" in the codebase is the 32-bit PING echo
value (`0xfeedbeef` in `hml_ctrl_host.c`), a liveness check with no security
value.

---

## 2. What an unauthenticated attacker can do

The control server dispatches on the opcode byte in a received message
(`handle_req`, `src/control/hml_control.c:228`). There is no handshake, no
session, no source filtering, and no rate limit. **A single unauthenticated
datagram is enough.** The available opcodes:

| Opcode | Value | Effect | Validation |
| --- | --- | --- | --- |
| `HML_OP_NOP` | 0x00 | ACK | none needed |
| `HML_OP_PING` | 0x01 | echoes the payload | none needed |
| `HML_OP_SET_DEST` | 0x02 | **reprograms the packetizer destination** — the IP, port, QP, RKEY, PSN and frame size the device streams to | payload must be exactly `HML_CFG_SIZE` and parse cleanly |
| `HML_OP_GET_DEST` | 0x03 | reads back the destination | `HML_ERR_STATE` until SET_DEST |
| `HML_OP_START` | 0x04 | **starts the data stream** | **ACKed unconditionally** |
| `HML_OP_STOP` | 0x05 | stops the stream | ACKed unconditionally |
| `HML_OP_RESET` | 0x06 | stops the stream, clears the destination | ACKed unconditionally |
| `HML_OP_GET_CAPS` | 0x07 | reads UUID, caps, firmware version | none needed |
| `HML_OP_STATUS` | 0x08 | reads the streaming flag | none needed |
| `HML_OP_POKE` | 0x09 | **writes a 32-bit device register** (addr + val) | payload exactly 8 bytes; device callback must succeed |
| `HML_OP_READBACK` | 0x0A | **reads a 32-bit device register** | payload exactly 4 bytes; device callback must succeed |

The realistic attack surface, in order of severity:

1. **`HML_OP_POKE` — arbitrary register write.** Whatever the device's `on_poke`
   callback maps an address to is reachable by anyone. On the bridge device that
   is a 256-entry I2C/SPI/UART register file: writing there can change sensor
   configuration, actuator state, or anything the callback chooses to allow.
   A device whose callback maps address ranges to power, pins, or DMA registers
   is fully exposed. There is no access-control list, no read-only region, and
   no audit log.
2. **`HML_OP_READBACK` — register and memory disclosure.** Reads back whatever
   the callback exposes. This leaks device state and any secrets the callback
   does not itself filter.
3. **`HML_OP_SET_DEST` — traffic redirection.** Repoints the outbound stream at
   the attacker's address. Data the device was going to send elsewhere is sent
   to the attacker instead. This is both a confidentiality and an availability
   problem.
4. **`HML_OP_START` / `HML_OP_STOP` / `HML_OP_RESET` — denial of service.** Stop
   or reset a live stream at will. Because these are ACKed unconditionally, a
   device cannot refuse them at the protocol level (see §5).

**None of these require any prior knowledge of the device beyond its IP and the
fact that it speaks HML.** The control port is fixed and well-known.

---

## 3. What the CRC does and does not buy you

Every control message and every discovery beacon carries a **CRC-16**
(`hml_crc16`, CRC-16/CCITT, polynomial `0x1021`, init `0xFFFF`) over the
message. This is a **checksum for detecting corruption and desynchronisation,
not a security mechanism.**

* A CRC has no key. An attacker who wants a valid message computes the CRC.
* It provides **no confidentiality**: the message is plaintext on the wire.
* It provides **no authentication**: nothing binds the message to a sender
  identity. It is a checksum, not a MAC.
* It provides **no replay protection**: a captured, still-valid message can be
  resent. There is no nonce, sequence number, or freshness field in the control
  protocol that the server checks. A captured `SET_DEST` replays forever.

The CRC's real job is what the code says it is: a device on a lossy link should
not act on a half-received or bit-flipped message, and a receiver that joins
mid-stream should reject garbage. `hml_control.c` drops a message whose length
does not match exactly or whose CRC fails, and counts it via
`hml_control_bad_count()`. That is a **reliability** property.

---

## 4. Discovery beacon: passive but informative

The 28-byte beacon (`hml_beacon`, `include/hml/hml_discovery.h`) is broadcast to
255.255.255.255:55100 roughly once per second. Layout, all big-endian:

```text
magic u32 | version u8 | flags u8 | ttl u8 | rsvd u8 | uuid u32
caps u32 | fw_ver u32 | ts_us u48 | crc16 u16          = 28 bytes
```

* `hml_beacon_parse` accepts **only** `HML_DISC_SIZE` (28) bytes and validates
  magic, version, and CRC.
* The timestamp is 48 bits and wraps after ~8.9 years — deliberately, so the
  record stays 28 bytes.
* `ttl` is a hop budget and is always 1 for a direct LAN peer; there is no
  routing, so it is informational.

**Security posture:** the beacon is a passive announcement and a client-only
listener, so it does not by itself grant control. But it **fingerprintable**:
`uuid`, `caps` and `fw_ver` let an attacker identify exactly which device
firmware is on the network and what features it exposes, and it hands them the
control port to aim at. Broadcasting means it is visible to every host on the
subnet, including hosts that have no business knowing an HML device is there.
There is **no way to disable the beacon** other than setting `disc_port = 0`
(`hml_control.c:83`), and `hml_ctrl_probe --discover` is the client that listens.

---

## 5. Protocol-level weaknesses worth knowing

* **`START`/`STOP`/`SET_DEST` are ACKed unconditionally.** The `hml_control`
  callbacks for these three ops return `void` (`hml_bridge_control.h`), so the
  server cannot learn that a device refused. A device that wants to reject a
  `START` (e.g. one still in INIT) has to enforce the phase itself and log it.
  The client therefore sees `ACK` even when the device did nothing. This is an
  API-shape limitation of the frozen control API, documented in
  `hml_bridge_control.h`, and it means **`ACK` is not a guarantee of effect.**
* **No replay or freshness check.** As noted, a captured `SET_DEST` or `POKE`
  replays indefinitely.
* **No source-address filtering.** The server answers whoever sent the request.
  Responses go to `c->cfg.reply_ip:reply_port`; the server does not verify that
  a response was solicited by the peer it is talking to.
* **The client's request/response matcher** (`xchg` in `hml_client.c:107`)
  correlates on **the opcode byte alone**. `hml_ctrl_msg` carries `op`, `flags`
  and a payload — there is no sequence number, transaction ID, or nonce in the
  control message at all. So a stray or replayed response to any *earlier*
  request of the same opcode is accepted as if it answered the current one, and
  an attacker who can inject onto the path can forge a response. This is a
  correctness hazard under injection and a security hazard under a shared
  segment; there is no MAC to prevent either.
* **No rate limiting or resource bounds on the server.** `hml_control_poll`
  (`src/control/hml_control.c:366`) serves exactly **one** datagram per call and
  returns, so a flood is naturally bounded by how fast the host calls `poll` —
  but nothing throttles, back-pressures or drops abusive peers, and the beacon
  is still emitted on every poll.

---

## 6. Bottom line

`v0.2.0-ship` treats the control plane as a **trusted-LAN, lab-instrument**
surface, which is the only assumption under which the current design is
defensible. Concretely:

* Intended deployment: a direct, physically controlled link (USB, a dedicated
  Ethernet point-to-point between two devices you own), or a lab network you
  control.
* **Not** intended: any network where other hosts are untrusted, including a
  shared office LAN, a hotel or conference network, or anything reachable from
  the internet.

If a deployment ever needs an untrusted network in the path, it must add, at
minimum, an authenticated and encrypted control channel (DTLS or an
HMAC-challenge-response over TLS, or simply a signed session key) plus replay
protection, **before** `v0.2.0-ship` code is exposed to it. That work is out of
scope for this release and is recorded as such in
[ship_checklist.md](ship_checklist.md) and [limitations.md](limitations.md).

The data plane's per-fragment CRC is a corruption check, not confidentiality;
if the data itself is sensitive, it needs its own protection. See
[limitations.md](limitations.md).

---

## See also

* [public_api.md](public_api.md) — exact signatures and status codes.
* [limitations.md](limitations.md) — non-goals, including "no TLS / no auth".
* [ship_checklist.md](ship_checklist.md) — the open security items.
* [protocol_v1.md](protocol_v1.md) — full on-wire layout of control and
  discovery.
