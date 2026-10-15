/*
 * M3 / T1a -- golden wire-format vectors for the socket-crafted RoCEv2 emitter.
 *
 * Asserts the EXACT byte layout of one UC RDMA WRITE datagram and the PSN
 * progression of the live transport, and anchors the CRC implementation against
 * the canonical hml_crc32_ieee("123456789") check value.
 */

#include "hml_t_m3.h"

#include <string.h>

#include "hml/hml_crc.h"
#include "hml/hml_port.h"
#include "hml/hml_protocol.h"
#include "hml/hml_sock.h"
#include "hml/hml_transport.h"
#include "hml_roce_priv.h"

#define PORT HML_ROCE_TEST_PORT /* 14791, unprivileged */
#define WIN_BASE 0x100000ull
#define WIN_LEN 65536u
#define FRAME_SIZE 9000u
#define RKEY 0xDEADBEEFu

static const uint32_t LOOPBACK = 0x7F000001u;

/* Independent internet-checksum over a header whose checksum field is zeroed,
 * used to anchor the IP header checksum built by the emitter. */
static uint16_t ref_ip_csum16(const uint8_t *h, size_t n) {
  uint32_t sum = 0;
  for (size_t i = 0; i < n; i += 2) {
    if (i == 10 || i == 11) continue; /* the checksum field itself */
    uint32_t w = (uint32_t)h[i] << 8;
    if (i + 1 < n) w |= h[i + 1];
    sum += w;
  }
  while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
  return (uint16_t)(~sum);
}

static void test_crc_anchor(void) {
  const char *v = "123456789";
  uint32_t got = hml_crc32_ieee(v, 9);
  T_CHECK(got == 0xCBF43926u, "hml_crc32_ieee(\"123456789\") = 0x%08X, want 0xCBF43926", got);
  T_CHECK(hml_crc16(v, 9) == 0x29B1u, "hml_crc16 anchor broken: 0x%04X", hml_crc16(v, 9));

  /* Streaming twin must agree with the one-shot reference bit for bit. */
  uint8_t buf[997];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i * 37u + 11u);
  uint32_t st = HML_ROCE_CRC32_INIT;
  st = hml_roce_crc32_update(st, buf, 1);
  st = hml_roce_crc32_update(st, buf + 1, 300);
  st = hml_roce_crc32_update(st, buf + 301, sizeof(buf) - 301);
  T_CHECK(hml_roce_crc32_done(st) == hml_crc32_ieee(buf, sizeof(buf)),
          "streaming CRC32 != one-shot: 0x%08X vs 0x%08X", hml_roce_crc32_done(st),
          hml_crc32_ieee(buf, sizeof(buf)));
}

static void test_golden_datagram(void) {
  static const uint64_t VA = 0x0000000000123456ull;
  static const uint32_t PSN = 0x00ABCDEFu;
  const uint32_t PSN24 = 0x00ABCDEFu & 0x00FFFFFFu; /* 0xABCDEF */
  const size_t LEN = 32;
  uint8_t payload[1500];
  for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(0x40u + (i & 0xFFu));

  hml_roce_wire_cfg w;
  memset(&w, 0, sizeof(w));
  w.src_ip = LOOPBACK;
  w.dst_ip = LOOPBACK;
  w.udp_port = PORT;
  w.ttl = 42;
  w.ip_id = 0x1234;
  w.psn = PSN;
  w.va = VA;
  w.rkey = RKEY;

  uint8_t dg[HML_ROCE_DG_MAX];
  size_t n = hml_roce_build_datagram(dg, sizeof(dg), &w, payload, LEN);

  T_CHECK(n == HML_ROCE_WIRE_OVERHEAD + LEN, "datagram length %zu, want %u", n,
          (unsigned)(HML_ROCE_WIRE_OVERHEAD + LEN));
  if (n == 0) return;

  /* ---- IPv4 header (20 bytes) ---- */
  T_CHECK(dg[0] == 0x45, "IP ver/IHL = 0x%02X, want 0x45", dg[0]);
  T_CHECK(dg[1] == 0x00, "IP DSCP/ECN = 0x%02X, want 0x00", dg[1]);
  T_CHECK(hml_get_be16(dg + 2) == (uint16_t)(60 + LEN), "IP total length = %u, want %u",
          hml_get_be16(dg + 2), (unsigned)(60 + LEN));
  T_CHECK(hml_get_be16(dg + 4) == 0x1234, "IP id = 0x%04X", hml_get_be16(dg + 4));
  T_CHECK(hml_get_be16(dg + 6) == 0x4000, "IP flags/frag = 0x%04X, want 0x4000 (DF)",
          hml_get_be16(dg + 6));
  T_CHECK(dg[8] == 42, "IP TTL = %u, want 42", dg[8]);
  T_CHECK(dg[9] == 17, "IP protocol = %u, want 17 (UDP)", dg[9]);
  T_CHECK(hml_get_be16(dg + 10) == ref_ip_csum16(dg, 20), "IP header checksum = 0x%04X, want 0x%04X",
          hml_get_be16(dg + 10), ref_ip_csum16(dg, 20));
  T_CHECK(hml_get_be32(dg + 12) == LOOPBACK, "IP src = 0x%08X, want 127.0.0.1",
          hml_get_be32(dg + 12));
  T_CHECK(hml_get_be32(dg + 16) == LOOPBACK, "IP dst = 0x%08X, want 127.0.0.1",
          hml_get_be32(dg + 16));

  /* ---- UDP header (8 bytes) ---- */
  T_CHECK(hml_get_be16(dg + 20) == PORT, "UDP sport = %u, want %u", hml_get_be16(dg + 20), PORT);
  T_CHECK(hml_get_be16(dg + 22) == 14791, "UDP dport = %u, want 14791 (not the privileged 4791)",
          hml_get_be16(dg + 22));
  T_CHECK(hml_get_be16(dg + 24) == (uint16_t)(8 + 12 + 16 + LEN + 4), "UDP length = %u, want %u",
          hml_get_be16(dg + 24), (unsigned)(8 + 12 + 16 + LEN + 4));
  T_CHECK(hml_get_be16(dg + 26) == 0, "UDP checksum = 0x%04X, want 0 (allowed for IPv4)",
          hml_get_be16(dg + 26));

  /* ---- BTH (12 bytes) ---- */
  T_CHECK(dg[28] == 0x2A, "BTH opcode = 0x%02X, want 0x2A (UC<<5 | RDMA)", dg[28]);
  T_CHECK(dg[29] == 0x00, "BTH flags = 0x%02X, want 0x00 (RDMA Write, no immediate)", dg[29]);
  T_CHECK(dg[30] == 0x00, "BTH reserved = 0x%02X, want 0x00", dg[30]);
  T_CHECK(dg[31] == (uint8_t)(PSN24 >> 16), "PSN byte 0 = 0x%02X, want 0x%02X", dg[31],
          (unsigned)(PSN24 >> 16));
  T_CHECK(dg[32] == (uint8_t)(PSN24 >> 8), "PSN byte 1 = 0x%02X, want 0x%02X", dg[32],
          (unsigned)(PSN24 >> 8));
  T_CHECK(dg[33] == (uint8_t)PSN24, "PSN byte 2 = 0x%02X, want 0x%02X", dg[33], (unsigned)PSN24);
  T_CHECK(hml_get_be16(dg + 34) == 0x0000, "BTH AETH = 0x%04X, want 0x0000", hml_get_be16(dg + 34));
  T_CHECK(dg[36] == 0 && dg[37] == 0 && dg[38] == 0 && dg[39] == 0, "BTH trailing pad not zero");

  /* ---- RETH (16 bytes) ---- */
  T_CHECK(hml_get_be64(dg + 40) == VA, "RETH VA = 0x%016llX, want 0x%llX",
          (unsigned long long)hml_get_be64(dg + 40), (unsigned long long)VA);
  T_CHECK(dg[45] == 0x12 && dg[46] == 0x34 && dg[47] == 0x56,
          "RETH VA is not big-endian: ...%02x %02x %02x", dg[45], dg[46], dg[47]);
  T_CHECK(dg[48] == 0xDE && dg[49] == 0xAD && dg[50] == 0xBE && dg[51] == 0xEF,
          "RETH rkey is not big-endian: %02x %02x %02x %02x", dg[48], dg[49], dg[50], dg[51]);
  T_CHECK(hml_get_be32(dg + 48) == RKEY, "RETH rkey = 0x%08X, want 0x%08X", hml_get_be32(dg + 48),
          RKEY);
  T_CHECK(hml_get_be32(dg + 52) == (uint32_t)LEN, "RETH length = %u, want %zu",
          hml_get_be32(dg + 52), LEN);

  /* ---- payload ---- */
  T_CHECK(memcmp(dg + 56, payload, LEN) == 0, "payload bytes not copied verbatim");

  /* ---- ICRC: little-endian CRC-32/IEEE over BTH..end-of-payload ---- */
  uint32_t icrc = hml_crc32_ieee(dg + 28, (size_t)(n - 4 - 28));
  uint32_t wire = (uint32_t)dg[n - 4] | ((uint32_t)dg[n - 3] << 8) | ((uint32_t)dg[n - 2] << 16) |
                  ((uint32_t)dg[n - 1] << 24);
  T_CHECK(wire == icrc, "ICRC wire value 0x%08X != hml_crc32_ieee(BTH..payload) 0x%08X", wire, icrc);
  T_CHECK(dg[n - 4] == (uint8_t)icrc, "ICRC byte 0 = 0x%02X, want 0x%02X (little-endian)",
          dg[n - 4], (unsigned)(icrc & 0xFFu));

  /* ICRC coverage must EXCLUDE the IP and UDP headers. */
  T_CHECK(hml_crc32_ieee(dg, n - 4) != icrc, "ICRC accidentally covers the IP/UDP headers");

  /* ---- bounds ---- */
  T_CHECK(hml_roce_build_datagram(dg, sizeof(dg), &w, payload, 1440) == 1500,
          "1440-byte payload must produce exactly 1500 bytes (no IP fragmentation)");
  T_CHECK(hml_roce_build_datagram(dg, sizeof(dg), &w, payload, 1441) == 0,
          "1441-byte payload must be rejected (HML_ERR_TOOLARGE at the ops layer)");
  T_CHECK(hml_roce_build_datagram(dg, 10, &w, payload, LEN) == 0, "short cap must be rejected");
}

/* PSN must advance by exactly 1 per posted datagram, on the wire. */
static void test_psn_progression(void) {
  hml_sock_t *rx = NULL;
  if (hml_sock_udp_create(&rx, 0, PORT, false) != HML_OK) {
    T_CHECK(0, "cannot bind udp/%u", PORT);
    return;
  }

  hml_dest_cfg dest;
  memset(&dest, 0, sizeof(dest));
  dest.kind = HML_XPORT_ROCEV2;
  dest.ipv4 = LOOPBACK;
  dest.udp_port = PORT;
  dest.qp = 1;
  dest.psn = 0x00123456u; /* 24-bit start PSN */
  dest.rkey = RKEY;
  dest.remote_addr = WIN_BASE;
  dest.frame_size = FRAME_SIZE;
  dest.window_len = 16;
  dest.hop_limit = 64;

  const hml_xport_ops *ops = hml_rocev2_ops();
  T_CHECK(ops != NULL, "hml_rocev2_ops() returned NULL");
  if (!ops) {
    hml_sock_close(rx);
    return;
  }
  T_CHECK(strcmp(ops->name, "rocev2") == 0, "ops name = '%s', want 'rocev2'", ops->name);

  hml_xport_t x;
  x.ops = ops;
  x.impl = NULL;
  T_CHECK(ops->open(&x, &dest, 7) == HML_OK, "open failed");
  T_CHECK(x.impl != NULL, "open did not allocate impl");

  static const uint32_t PSN0 = 0x00123456u;
  const size_t LEN = 64;
  uint8_t payload[64];
  memset(payload, 0x5A, sizeof(payload));

  for (int i = 0; i < 4; i++) {
    hml_frame_hdr h;
    memset(&h, 0, sizeof(h));
    h.stream_id = 7;
    h.seq = (uint32_t)i;
    h.frag_idx = (uint16_t)i;
    h.frag_cnt = 4;
    h.flags = (uint8_t)((i == 0 ? HML_FLAG_START : 0) | (i == 3 ? HML_FLAG_END : 0));
    h.payload_len = (uint16_t)LEN;
    T_CHECK(ops->send_frag(&x, &h, payload, LEN) == HML_OK, "send_frag %d failed", i);

    uint8_t got[2048];
    size_t gn = 0;
    hml_status st = hml_sock_recv(rx, got, sizeof(got), &gn, 2000);
    T_CHECK(st == HML_OK, "recv %d failed (%d)", i, (int)st);
    if (st != HML_OK) break;
    /* The kernel rewrites IP+UDP, so the BTH starts at the standard 20+8. */
    T_CHECK(gn >= 28 + 12 + 16 + LEN + 4, "short datagram %zu", gn);
    if (gn < 28 + 12 + 16 + LEN + 4) break;
    uint32_t psn = ((uint32_t)got[28 + 3] << 16) | ((uint32_t)got[28 + 4] << 8) | got[28 + 5];
    uint32_t want = (PSN0 + (uint32_t)i) & 0xFFFFFFu;
    T_CHECK(psn == want, "packet %d PSN = 0x%06X, want 0x%06X", i, psn, want);
    T_CHECK(got[28] == 0x2A, "packet %d opcode = 0x%02X, want 0x2A", i, got[28]);
    T_CHECK(hml_get_be64(got + 40) == WIN_BASE + (uint64_t)i * LEN,
            "packet %d VA = 0x%llX, want 0x%llX", i, (unsigned long long)hml_get_be64(got + 40),
            (unsigned long long)(WIN_BASE + (uint64_t)i * LEN));
    T_CHECK(hml_get_be32(got + 52) == (uint32_t)LEN, "packet %d RETH len = %u", i,
            hml_get_be32(got + 52));
  }

  /* The 4th datagram is the EOF metadata write at remote_addr + frame_size. */
  {
    uint8_t got[2048];
    size_t gn = 0;
    T_CHECK(hml_sock_recv(rx, got, sizeof(got), &gn, 2000) == HML_OK, "meta recv failed");
    if (gn >= 28 + 12 + 16 + HML_ROCE_META_LEN + 4) {
      T_CHECK(hml_get_be64(got + 40) == WIN_BASE + FRAME_SIZE,
              "meta VA = 0x%llX, want 0x%llX", (unsigned long long)hml_get_be64(got + 40),
              (unsigned long long)(WIN_BASE + FRAME_SIZE));
      uint32_t psn = ((uint32_t)got[31] << 16) | ((uint32_t)got[32] << 8) | got[33];
      T_CHECK(psn == ((PSN0 + 4u) & 0xFFFFFFu), "meta PSN = 0x%06X, want 0x%06X", psn,
              (PSN0 + 4u) & 0xFFFFFFu);
      const uint8_t *m = got + 56;
      uint32_t magic = (uint32_t)m[0] | ((uint32_t)m[1] << 8) | ((uint32_t)m[2] << 16) |
                       ((uint32_t)m[3] << 24);
      T_CHECK(magic == HML_MAGIC_FRAME, "meta magic 0x%08X", magic);
      uint32_t seq = (uint32_t)m[8] | ((uint32_t)m[9] << 8) | ((uint32_t)m[10] << 16) |
                     ((uint32_t)m[11] << 24);
      T_CHECK(seq == 3, "meta frame_seq = %u, want 3", seq);
      uint32_t fb = (uint32_t)m[12] | ((uint32_t)m[13] << 8) | ((uint32_t)m[14] << 16) |
                    ((uint32_t)m[15] << 24);
      T_CHECK(fb == 4u * LEN, "meta frame_bytes = %u, want %zu", fb, 4u * LEN);
      uint32_t mcrc = (uint32_t)m[16] | ((uint32_t)m[17] << 8) | ((uint32_t)m[18] << 16) |
                      ((uint32_t)m[19] << 24);
      uint8_t whole[4 * LEN];
      memset(whole, 0x5A, sizeof(whole));
      T_CHECK(mcrc == hml_crc32_ieee(whole, sizeof(whole)),
              "meta crc = 0x%08X, want CRC over the whole 4-fragment frame 0x%08X", mcrc,
              hml_crc32_ieee(whole, sizeof(whole)));
    }
  }

  /* flush() after the metadata already went out must be a no-op. */
  T_CHECK(ops->flush(&x) == HML_OK, "flush failed");
  {
    uint8_t got[64];
    size_t gn = 0;
    T_CHECK(hml_sock_recv(rx, got, sizeof(got), &gn, 200) == HML_ERR_TIMEOUT,
            "flush emitted a duplicate metadata packet");
  }

  /* Oversized fragment must be rejected before any socket traffic. */
  {
    hml_frame_hdr h;
    memset(&h, 0, sizeof(h));
    h.flags = HML_FLAG_START;
    uint8_t big[HML_ROCE_MAX_PAYLOAD + 1];
    memset(big, 0, sizeof(big));
    T_CHECK(ops->send_frag(&x, &h, big, sizeof(big)) == HML_ERR_TOOLARGE,
            "1441-byte fragment must return HML_ERR_TOOLARGE");
  }

  ops->close(&x);
  T_CHECK(x.impl == NULL, "close did not clear impl");
  hml_sock_close(rx);
}

static void test_register(void) {
  /* Requires the core transport registry (hml_core); skip cleanly if absent. */
  hml_status st = hml_rocev2_register();
  T_CHECK(st == HML_OK, "hml_rocev2_register returned %d", (int)st);
  const hml_xport_ops *ops = hml_transport_lookup("rocev2");
  T_CHECK(ops == hml_rocev2_ops(), "lookup('rocev2') did not return the registered ops");
}

static void test_rc_and_imm_datagrams(void) {
  static const uint64_t VA = 0x0000000000200000ull;
  const size_t LEN = 16;
  uint8_t payload[16];
  for (size_t i = 0; i < LEN; i++) payload[i] = (uint8_t)(0x80u + i);

  /* 1. RC RDMA Write (opcode 0x0A, 60 bytes overhead) */
  {
    hml_roce_wire_cfg w;
    memset(&w, 0, sizeof(w));
    w.src_ip = LOOPBACK;
    w.dst_ip = LOOPBACK;
    w.udp_port = PORT;
    w.ttl = 64;
    w.ip_id = 1;
    w.psn = 0x1000;
    w.va = VA;
    w.rkey = RKEY;
    w.qp_type = 1; /* RC */

    uint8_t dg[HML_ROCE_DG_MAX];
    size_t n = hml_roce_build_datagram(dg, sizeof(dg), &w, payload, LEN);
    T_CHECK(n == HML_ROCE_WIRE_OVERHEAD + LEN, "RC datagram len = %zu, want %u", n,
            (unsigned)(HML_ROCE_WIRE_OVERHEAD + LEN));
    T_CHECK(dg[28] == HML_ROCE_OPCODE_RC_RDMA_WRITE, "BTH opcode = 0x%02X, want 0x0A (RC RDMA WRITE)",
            dg[28]);
    uint32_t icrc = hml_crc32_ieee(dg + 28, (size_t)(n - 4 - 28));
    uint32_t wire = (uint32_t)dg[n - 4] | ((uint32_t)dg[n - 3] << 8) | ((uint32_t)dg[n - 2] << 16) |
                    ((uint32_t)dg[n - 1] << 24);
    T_CHECK(wire == icrc, "RC ICRC wire = 0x%08X, want 0x%08X", wire, icrc);
  }

  /* 2. UC RDMA Write with Immediate (opcode 0x2B, 64 bytes overhead) */
  {
    hml_roce_wire_cfg w;
    memset(&w, 0, sizeof(w));
    w.src_ip = LOOPBACK;
    w.dst_ip = LOOPBACK;
    w.udp_port = PORT;
    w.ttl = 64;
    w.ip_id = 2;
    w.psn = 0x1001;
    w.va = VA;
    w.rkey = RKEY;
    w.qp_type = 0; /* UC */
    w.has_imm = true;
    w.imm_data = 0x12345678u;

    uint8_t dg[HML_ROCE_DG_MAX];
    size_t n = hml_roce_build_datagram(dg, sizeof(dg), &w, payload, LEN);
    T_CHECK(n == HML_ROCE_WIRE_OVERHEAD_IMM + LEN, "UC Imm datagram len = %zu, want %u", n,
            (unsigned)(HML_ROCE_WIRE_OVERHEAD_IMM + LEN));
    T_CHECK(dg[28] == HML_ROCE_OPCODE_UC_RDMA_WRITE_WITH_IMM,
            "BTH opcode = 0x%02X, want 0x2B (UC WRITE WITH IMM)", dg[28]);
    T_CHECK(hml_get_be32(dg + HML_ROCE_OFF_IMM) == 0x12345678u,
            "Immediate data = 0x%08X, want 0x12345678", hml_get_be32(dg + HML_ROCE_OFF_IMM));
    T_CHECK(memcmp(dg + HML_ROCE_OFF_IMM_PAYLOAD, payload, LEN) == 0,
            "payload not placed at offset %u", (unsigned)HML_ROCE_OFF_IMM_PAYLOAD);
    uint32_t icrc = hml_crc32_ieee(dg + 28, (size_t)(n - 4 - 28));
    uint32_t wire = (uint32_t)dg[n - 4] | ((uint32_t)dg[n - 3] << 8) | ((uint32_t)dg[n - 2] << 16) |
                    ((uint32_t)dg[n - 1] << 24);
    T_CHECK(wire == icrc, "UC Imm ICRC wire = 0x%08X, want 0x%08X", wire, icrc);
  }

  /* 3. RC RDMA Write with Immediate (opcode 0x0B, 64 bytes overhead) */
  {
    hml_roce_wire_cfg w;
    memset(&w, 0, sizeof(w));
    w.src_ip = LOOPBACK;
    w.dst_ip = LOOPBACK;
    w.udp_port = PORT;
    w.ttl = 64;
    w.ip_id = 3;
    w.psn = 0x1002;
    w.va = VA;
    w.rkey = RKEY;
    w.qp_type = 1; /* RC */
    w.has_imm = true;
    w.imm_data = 0xAABBCCDDu;

    uint8_t dg[HML_ROCE_DG_MAX];
    size_t n = hml_roce_build_datagram(dg, sizeof(dg), &w, payload, LEN);
    T_CHECK(n == HML_ROCE_WIRE_OVERHEAD_IMM + LEN, "RC Imm datagram len = %zu, want %u", n,
            (unsigned)(HML_ROCE_WIRE_OVERHEAD_IMM + LEN));
    T_CHECK(dg[28] == HML_ROCE_OPCODE_RC_RDMA_WRITE_WITH_IMM,
            "BTH opcode = 0x%02X, want 0x0B (RC WRITE WITH IMM)", dg[28]);
    T_CHECK(hml_get_be32(dg + HML_ROCE_OFF_IMM) == 0xAABBCCDDu,
            "Immediate data = 0x%08X, want 0xAABBCCDD", hml_get_be32(dg + HML_ROCE_OFF_IMM));
    uint32_t icrc = hml_crc32_ieee(dg + 28, (size_t)(n - 4 - 28));
    uint32_t wire = (uint32_t)dg[n - 4] | ((uint32_t)dg[n - 3] << 8) | ((uint32_t)dg[n - 2] << 16) |
                    ((uint32_t)dg[n - 1] << 24);
    T_CHECK(wire == icrc, "RC Imm ICRC wire = 0x%08X, want 0x%08X", wire, icrc);
  }
}

int main(void) {
  test_crc_anchor();
  test_golden_datagram();
  test_psn_progression();
  test_rc_and_imm_datagrams();
  test_register();
  T_DONE("test_roce_hdr");
}
