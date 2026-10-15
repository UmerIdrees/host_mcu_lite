/*
 * M3 / T1a -- socket-crafted RoCEv2 "UC RDMA WRITE" transmitter, modelled on the
 * NVIDIA HSB Emulator RoCEv2Transmitter: no libibverbs, no ConnectX, just a UDP
 * socket carrying a hand-rolled IP/UDP/BTH/RETH/ICRC datagram.
 *
 * ---------------------------------------------------------------------------
 * EXACT WIRE LAYOUT. "off" is the absolute byte offset from the first byte of
 * the datagram, "sz" the field size in bytes. Total = 60 + payload_len.
 * ---------------------------------------------------------------------------
 *   off sz  field                       encoding
 *   --- --  --------------------------  ---------------------------------------
 *    0   1  ver/IHL                     0x45 = IPv4, IHL 5 (no options)
 *    1   1  DSCP/ECN                    0x00
 *    2   2  IP total length             BE = 60 + payload_len
 *    4   2  identification              BE, +1 per emitted datagram
 *    6   2  flags / fragment offset     0x4000 = "don't fragment"
 *    8   1  time to live                dest->hop_limit (0 => 64)
 *    9   1  protocol                    17 (UDP)
 *   10   2  IP header checksum          ones-complement 16-bit, mandatory
 *   12   4  source IPv4                 BE, see note (1)
 *   16   4  destination IPv4            BE = dest->ipv4
 *   20   2  UDP source port             BE, mirrors the destination port (2)
 *   22   2  UDP destination port        BE, dest->udp_port (4791 in production)
 *   24   2  UDP length                  BE = 40 + payload_len
 *   26   2  UDP checksum                0 -- see note (3)
 *   28  12  BTH                         see below
 *   40  16  RETH                        see below
 *   56   n  RDMA write payload
 * 56+n   4  ICRC                        LITTLE-endian, see note (4)
 *
 * BTH (12 bytes; offsets are relative to off 28):
 *    0   1   OpCode = 0x2A                transport-service UC (0b001) << 5
 *                                        | RDMA opcode 0x0A  -> 0x20 | 0x0A
 *    1   1   Flags = 0x00                 RDMA Write: SE=0, DNC=0, Fenced=0
 *    2   1   Reserved = 0x00
 *    3   3   PSN (24-bit, big-endian)     low 24 bits of psn
 *    6   2   AETH = 0x0000                SYN-ACK, no syndrome
 *    8   4   Reserved = 0
 *
 * RETH (16 bytes; offsets are relative to off 40):
 *    0   8   Virtual Address              BE u64 = dest->remote_addr + frame off
 *    8   4   Remote Key (rkey)            BE u32 = dest->rkey
 *   12   4   Remote Segment Length        BE u32 = payload_len (D bit = 0)
 *
 * notes
 * (1) Source IP: 127.0.0.1 when the destination is loopback, else 0.0.0.0. The
 *     frozen hml_sock interface is an ordinary SOCK_DGRAM socket, so on the wire
 *     the kernel supplies its own outer IPv4/UDP header and our whole buffer
 *     (including the IP/UDP bytes below) rides as the UDP payload; the receiver
 *     therefore sees this exact buffer back from hml_sock_recv() and the demux
 *     strips the 20/8-byte IP/UDP prefix of it. The crafted IP header is
 *     byte-exact and is what the golden-vector tests assert, and it is exactly
 *     what a raw-socket / IP_HDRINCL sender would put on the wire.
 * (2) Real RoCE NICs use 4791 on both source and destination; mirrored here.
 * (3) A zero UDP checksum is legal for IPv4 (RFC 768) and saves a pseudo-header
 *     implementation. IPv4 over Ethernet actually has a usable checksum, but on
 *     a link-layer CRC'd fabric the ICRC below is the real integrity mechanism,
 *     and Linux fills the UDP checksum in for us on send anyway.
 * (4) ICRC = CRC-32/IEEE over the BTH through the end of the payload: coverage
 *     EXCLUDES the IP and UDP headers and the ICRC field itself, which is the
 *     RoCEv2 rule. The 32-bit value is transmitted LITTLE-ENDIAN (InfiniBand
 *     puts the CRC on the wire least-significant byte first) while every other
 *     multi-byte field here is big-endian. The value is bit-identical to
 *     hml_crc32_ieee(); it is produced by the table-driven twin below because
 *     the frozen bitwise loop costs ~100 us per KiB, which is far too slow for a
 *     1.5 KiB-per-packet data path. tests/m3/test_roce_hdr.c pins the two
 *     against each other.
 *
 * ---------------------------------------------------------------------------
 * EOF / frame-completion design
 * ---------------------------------------------------------------------------
 * There is no trailer and no implicit length, so the receiver cannot know that a
 * frame is complete. T1a therefore adds a *meta slot*: one dedicated 20-byte
 * RDMA WRITE to VA = dest->remote_addr + dest->frame_size, i.e. immediately past
 * the data window. The slot address is derived purely from the control-plane
 * hml_dest_cfg, so the host demux recognises it with zero out-of-band state and
 * with no extra control message. Exactly one metadata packet per frame is
 * emitted: on the HML_FLAG_END data fragment, and flush() is only a safety net
 * for a stream that sets END without a trailing data fragment.
 *
 * Meta payload (LITTLE-endian, host-local RDMA memory):
 *   [0]  u32 magic       = HML_MAGIC_FRAME
 *   [4]  u16 stream_id
 *   [6]  u16 flags       = HML_FLAG_* of the last fragment
 *   [8]  u32 frame_seq
 *   [12] u32 frame_bytes
 *   [16] u32 frame_crc32 = CRC-32/IEEE over the assembled frame payload
 * (4+2+2+4+4+4 = 20 bytes; the "16-byte" figure in the M3 notes was a miscount,
 * the explicit field list is authoritative.)
 *
 * PRIVILEGED PORT: production uses UDP 4791 (HML_ROCE_PORT), which is below 1024
 * and therefore needs root or CAP_NET_BIND_SERVICE. dest->udp_port makes it fully
 * configurable; every test in tests/m3 uses 14791.
 */

#include "hml_roce_priv.h"

#include <stdlib.h>
#include <string.h>

/* ---- module state ------------------------------------------------------- */

static hml_roce_fault_fn hml_roce_fault = NULL;
static void *hml_roce_fault_ctx = NULL;

void hml_roce_set_fault_hook(hml_roce_fault_fn fn, void *ctx) {
  hml_roce_fault = fn;
  hml_roce_fault_ctx = ctx;
}

/* ---- helpers ------------------------------------------------------------ */

static uint16_t roce_ip_csum16(const uint8_t *h, size_t n) {
  uint32_t sum = 0;
  for (size_t i = 0; i + 1 < n; i += 2) sum += ((uint32_t)h[i] << 8) | h[i + 1];
  if (n & 1u) sum += (uint32_t)h[n - 1] << 8;
  while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
  return (uint16_t)(~sum);
}

/*
 * CRC-32/IEEE table (reflected, poly 0xEDB88320), generated as
 *   tab[i] = the byte-wise reflected CRC-32 of the value i
 * It is the standard zlib-style table; hml_crc32_ieee() walks the same algorithm
 * one bit at a time and costs ~100 us per KiB on the reference host, which is
 * ~100x too slow for a 1.5 KiB-per-packet data path. Every use of this table is
 * pinned against hml_crc32_ieee() by tests/m3/test_roce_hdr.c.
 */
static const uint32_t hml_roce_crc32_tab[256] = {
    0x00000000U, 0x77073096U, 0xEE0E612CU, 0x990951BAU, 0x076DC419U, 0x706AF48FU,
    0xE963A535U, 0x9E6495A3U, 0x0EDB8832U, 0x79DCB8A4U, 0xE0D5E91EU, 0x97D2D988U,
    0x09B64C2BU, 0x7EB17CBDU, 0xE7B82D07U, 0x90BF1D91U, 0x1DB71064U, 0x6AB020F2U,
    0xF3B97148U, 0x84BE41DEU, 0x1ADAD47DU, 0x6DDDE4EBU, 0xF4D4B551U, 0x83D385C7U,
    0x136C9856U, 0x646BA8C0U, 0xFD62F97AU, 0x8A65C9ECU, 0x14015C4FU, 0x63066CD9U,
    0xFA0F3D63U, 0x8D080DF5U, 0x3B6E20C8U, 0x4C69105EU, 0xD56041E4U, 0xA2677172U,
    0x3C03E4D1U, 0x4B04D447U, 0xD20D85FDU, 0xA50AB56BU, 0x35B5A8FAU, 0x42B2986CU,
    0xDBBBC9D6U, 0xACBCF940U, 0x32D86CE3U, 0x45DF5C75U, 0xDCD60DCFU, 0xABD13D59U,
    0x26D930ACU, 0x51DE003AU, 0xC8D75180U, 0xBFD06116U, 0x21B4F4B5U, 0x56B3C423U,
    0xCFBA9599U, 0xB8BDA50FU, 0x2802B89EU, 0x5F058808U, 0xC60CD9B2U, 0xB10BE924U,
    0x2F6F7C87U, 0x58684C11U, 0xC1611DABU, 0xB6662D3DU, 0x76DC4190U, 0x01DB7106U,
    0x98D220BCU, 0xEFD5102AU, 0x71B18589U, 0x06B6B51FU, 0x9FBFE4A5U, 0xE8B8D433U,
    0x7807C9A2U, 0x0F00F934U, 0x9609A88EU, 0xE10E9818U, 0x7F6A0DBBU, 0x086D3D2DU,
    0x91646C97U, 0xE6635C01U, 0x6B6B51F4U, 0x1C6C6162U, 0x856530D8U, 0xF262004EU,
    0x6C0695EDU, 0x1B01A57BU, 0x8208F4C1U, 0xF50FC457U, 0x65B0D9C6U, 0x12B7E950U,
    0x8BBEB8EAU, 0xFCB9887CU, 0x62DD1DDFU, 0x15DA2D49U, 0x8CD37CF3U, 0xFBD44C65U,
    0x4DB26158U, 0x3AB551CEU, 0xA3BC0074U, 0xD4BB30E2U, 0x4ADFA541U, 0x3DD895D7U,
    0xA4D1C46DU, 0xD3D6F4FBU, 0x4369E96AU, 0x346ED9FCU, 0xAD678846U, 0xDA60B8D0U,
    0x44042D73U, 0x33031DE5U, 0xAA0A4C5FU, 0xDD0D7CC9U, 0x5005713CU, 0x270241AAU,
    0xBE0B1010U, 0xC90C2086U, 0x5768B525U, 0x206F85B3U, 0xB966D409U, 0xCE61E49FU,
    0x5EDEF90EU, 0x29D9C998U, 0xB0D09822U, 0xC7D7A8B4U, 0x59B33D17U, 0x2EB40D81U,
    0xB7BD5C3BU, 0xC0BA6CADU, 0xEDB88320U, 0x9ABFB3B6U, 0x03B6E20CU, 0x74B1D29AU,
    0xEAD54739U, 0x9DD277AFU, 0x04DB2615U, 0x73DC1683U, 0xE3630B12U, 0x94643B84U,
    0x0D6D6A3EU, 0x7A6A5AA8U, 0xE40ECF0BU, 0x9309FF9DU, 0x0A00AE27U, 0x7D079EB1U,
    0xF00F9344U, 0x8708A3D2U, 0x1E01F268U, 0x6906C2FEU, 0xF762575DU, 0x806567CBU,
    0x196C3671U, 0x6E6B06E7U, 0xFED41B76U, 0x89D32BE0U, 0x10DA7A5AU, 0x67DD4ACCU,
    0xF9B9DF6FU, 0x8EBEEFF9U, 0x17B7BE43U, 0x60B08ED5U, 0xD6D6A3E8U, 0xA1D1937EU,
    0x38D8C2C4U, 0x4FDFF252U, 0xD1BB67F1U, 0xA6BC5767U, 0x3FB506DDU, 0x48B2364BU,
    0xD80D2BDAU, 0xAF0A1B4CU, 0x36034AF6U, 0x41047A60U, 0xDF60EFC3U, 0xA867DF55U,
    0x316E8EEFU, 0x4669BE79U, 0xCB61B38CU, 0xBC66831AU, 0x256FD2A0U, 0x5268E236U,
    0xCC0C7795U, 0xBB0B4703U, 0x220216B9U, 0x5505262FU, 0xC5BA3BBEU, 0xB2BD0B28U,
    0x2BB45A92U, 0x5CB36A04U, 0xC2D7FFA7U, 0xB5D0CF31U, 0x2CD99E8BU, 0x5BDEAE1DU,
    0x9B64C2B0U, 0xEC63F226U, 0x756AA39CU, 0x026D930AU, 0x9C0906A9U, 0xEB0E363FU,
    0x72076785U, 0x05005713U, 0x95BF4A82U, 0xE2B87A14U, 0x7BB12BAEU, 0x0CB61B38U,
    0x92D28E9BU, 0xE5D5BE0DU, 0x7CDCEFB7U, 0x0BDBDF21U, 0x86D3D2D4U, 0xF1D4E242U,
    0x68DDB3F8U, 0x1FDA836EU, 0x81BE16CDU, 0xF6B9265BU, 0x6FB077E1U, 0x18B74777U,
    0x88085AE6U, 0xFF0F6A70U, 0x66063BCAU, 0x11010B5CU, 0x8F659EFFU, 0xF862AE69U,
    0x616BFFD3U, 0x166CCF45U, 0xA00AE278U, 0xD70DD2EEU, 0x4E048354U, 0x3903B3C2U,
    0xA7672661U, 0xD06016F7U, 0x4969474DU, 0x3E6E77DBU, 0xAED16A4AU, 0xD9D65ADCU,
    0x40DF0B66U, 0x37D83BF0U, 0xA9BCAE53U, 0xDEBB9EC5U, 0x47B2CF7FU, 0x30B5FFE9U,
    0xBDBDF21CU, 0xCABAC28AU, 0x53B39330U, 0x24B4A3A6U, 0xBAD03605U, 0xCDD70693U,
    0x54DE5729U, 0x23D967BFU, 0xB3667A2EU, 0xC4614AB8U, 0x5D681B02U, 0x2A6F2B94U,
    0xB40BBE37U, 0xC30C8EA1U, 0x5A05DF1BU, 0x2D02EF8DU
};

uint32_t hml_roce_crc32_update(uint32_t state, const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t crc = state;
  for (size_t i = 0; i < n; i++)
    crc = (crc >> 8) ^ hml_roce_crc32_tab[(crc ^ p[i]) & 0xFFu];
  return crc;
}

static void roce_put_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void roce_put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t roce_src_ip(const hml_roce_ctx *c) {
  return (c->dest.ipv4 == HML_ROCE_LOOPBACK_IP) ? HML_ROCE_LOOPBACK_IP : 0u;
}

static uint8_t roce_ttl(const hml_roce_ctx *c) {
  return c->dest.hop_limit ? c->dest.hop_limit : (uint8_t)HML_ROCE_DEFAULT_TTL;
}

size_t hml_roce_build_datagram(uint8_t *out, size_t cap, const hml_roce_wire_cfg *w,
                               const uint8_t *payload, size_t len) {
  if (!out || !w || (!payload && len)) return 0;
  if (len > HML_ROCE_MAX_PAYLOAD) return 0;

  const size_t imm_len = w->has_imm ? HML_ROCE_IMM_LEN : 0u;
  const size_t total = HML_ROCE_WIRE_OVERHEAD + imm_len + len;
  if (cap < total) return 0;

  uint8_t *ip = out + HML_ROCE_OFF_IP;
  uint8_t *udp = out + HML_ROCE_OFF_UDP;
  uint8_t *bth = out + HML_ROCE_OFF_BTH;
  uint8_t *reth = out + HML_ROCE_OFF_RETH;
  uint8_t *pay = out + (w->has_imm ? HML_ROCE_OFF_IMM_PAYLOAD : HML_ROCE_OFF_PAYLOAD);
  uint8_t *icrc = pay + len;

  memset(out, 0, total);

  /* IP: IHL=5 (no options), DSCP 0, DF set, protocol UDP. */
  ip[0] = 0x45;
  ip[1] = 0x00;
  hml_put_be16(ip + 2, (uint16_t)total);
  hml_put_be16(ip + 4, w->ip_id);
  hml_put_be16(ip + 6, 0x4000);
  ip[8] = w->ttl;
  ip[9] = 17;
  hml_put_be32(ip + 12, w->src_ip);
  hml_put_be32(ip + 16, w->dst_ip);
  hml_put_be16(ip + 10, roce_ip_csum16(ip, HML_ROCE_IP_HDR_LEN));

  /* UDP: source port mirrors the destination port; checksum left at 0. */
  hml_put_be16(udp + 0, w->udp_port);
  hml_put_be16(udp + 2, w->udp_port);
  hml_put_be16(udp + 4, (uint16_t)(HML_ROCE_UDP_HDR_LEN + HML_ROCE_BTH_RETH_LEN + imm_len + len +
                                    HML_ROCE_ICRC_LEN));

  /* BTH: Select opcode based on qp_type and has_imm */
  uint8_t opcode;
  if (w->qp_type == 1 /* RC */) {
    opcode = w->has_imm ? HML_ROCE_OPCODE_RC_RDMA_WRITE_WITH_IMM : HML_ROCE_OPCODE_RC_RDMA_WRITE;
  } else {
    opcode = w->has_imm ? HML_ROCE_OPCODE_UC_RDMA_WRITE_WITH_IMM : HML_ROCE_OPCODE_UC_RDMA_WRITE;
  }
  bth[HML_ROCE_BTH_OPCODE] = opcode;
  bth[HML_ROCE_BTH_FLAGS] = 0x00;
  bth[HML_ROCE_BTH_RSVD] = 0x00;
  bth[HML_ROCE_BTH_PSN + 0] = (uint8_t)((w->psn >> 16) & 0xFFu);
  bth[HML_ROCE_BTH_PSN + 1] = (uint8_t)((w->psn >> 8) & 0xFFu);
  bth[HML_ROCE_BTH_PSN + 2] = (uint8_t)(w->psn & 0xFFu);
  hml_put_be16(bth + HML_ROCE_BTH_AETH, 0x0000);

  /* RETH: VA, rkey, segment length, all big-endian. */
  hml_put_be64(reth, w->va);
  hml_put_be32(reth + 8, w->rkey);
  hml_put_be32(reth + 12, (uint32_t)len);

  /* Immediate Data */
  if (w->has_imm) {
    hml_put_be32(out + HML_ROCE_OFF_IMM, w->imm_data);
  }

  if (len) memcpy(pay, payload, len);

  /* ICRC: CRC-32/IEEE over BTH..end-of-payload, stored little-endian. */
  uint32_t ic = hml_roce_crc32_done(hml_roce_crc32_update(HML_ROCE_CRC32_INIT, bth,
                                                          (size_t)(icrc - bth)));
  roce_put_le32(icrc, ic);
  return total;
}

/* ---- emission ----------------------------------------------------------- */

static hml_status roce_emit(hml_roce_ctx *c, uint64_t va, uint32_t rkey, const uint8_t *payload,
                            size_t len, bool has_imm, uint32_t imm_data) {
  hml_roce_wire_cfg w;
  memset(&w, 0, sizeof(w));
  w.src_ip = roce_src_ip(c);
  w.dst_ip = c->dest.ipv4;
  w.udp_port = c->port;
  w.ttl = roce_ttl(c);
  w.ip_id = c->ip_id;
  w.psn = c->psn & 0x00FFFFFFu;
  w.va = va;
  w.rkey = rkey;
  w.qp_type = c->qp_type;
  w.has_imm = has_imm;
  w.imm_data = imm_data;

  size_t n = hml_roce_build_datagram(c->buf, sizeof(c->buf), &w, payload, len);
  if (n == 0) return HML_ERR_TOOLARGE;

  const uint32_t psn = w.psn;
  const uint16_t ip_id = w.ip_id;
  const unsigned idx = c->dg_index++;

  if (hml_roce_fault) {
    size_t hook_len = n;
    if (hml_roce_fault(hml_roce_fault_ctx, idx, c->buf, &hook_len)) {
      /* Dropped in flight: the datagram left the sender, so keep PSN monotonic. */
      c->psn = (psn + 1u) & 0x00FFFFFFu;
      c->ip_id = (uint16_t)(ip_id + 1u);
      return HML_OK;
    }
    if (hook_len != n) return HML_ERR_STATE;
  }

  hml_status st = hml_sock_send(c->sock, c->buf, n);
  if (st != HML_OK) {
    /* Keep psn/ip_id untouched so the caller can retry a byte-identical packet. */
    c->dg_index = idx;
    return st;
  }
  c->psn = (psn + 1u) & 0x00FFFFFFu;
  c->ip_id = (uint16_t)(ip_id + 1u);
  return HML_OK;
}

static hml_status roce_emit_meta(hml_roce_ctx *c) {
  uint8_t meta[HML_ROCE_META_LEN];
  roce_put_le32(meta + HML_ROCE_OFF_META_MAGIC, HML_MAGIC_FRAME);
  roce_put_le16(meta + HML_ROCE_OFF_META_STREAM, c->stream_id);
  roce_put_le16(meta + HML_ROCE_OFF_META_FLAGS, c->frame_flags);
  roce_put_le32(meta + HML_ROCE_OFF_META_SEQ, c->frame_seq);
  roce_put_le32(meta + HML_ROCE_OFF_META_BYTES, (uint32_t)c->frame_bytes);
  roce_put_le32(meta + HML_ROCE_OFF_META_CRC, hml_roce_crc32_done(c->crc_state));

  /* Meta slot sits immediately past the data window -- no out-of-band state. */
  uint64_t meta_va = c->dest.remote_addr + (uint64_t)c->dest.frame_size;
  bool use_imm = c->use_imm;
  uint32_t imm_val = c->imm_val ? c->imm_val : (uint32_t)c->frame_seq;
  hml_status st = roce_emit(c, meta_va, c->dest.rkey, meta, sizeof(meta), use_imm, imm_val);
  if (st == HML_OK) c->meta_sent = true;
  return st;
}

/* ---- hml_xport_ops ------------------------------------------------------ */

static hml_status roce_open_ex(hml_xport_t *x, const hml_dest_cfg *dest, uint16_t stream_id,
                               uint16_t src_port) {
  if (!x || !dest) return HML_ERR_ARG;
  /* frame_size places the EOF meta slot at remote_addr + frame_size; a zero
   * frame_size would make the meta slot collide with the first data write, so
   * reject it here and let the caller fall back to T0. */
  if (dest->frame_size == 0) return HML_ERR_ARG;

  hml_roce_ctx *c = (hml_roce_ctx *)calloc(1, sizeof(*c));
  if (!c) return HML_ERR_NOMEM;

  c->dest = *dest;
  c->stream_id = stream_id;
  c->psn = dest->psn & 0x00FFFFFFu;
  c->ip_id = 0;
  c->crc_state = HML_ROCE_CRC32_INIT;
  c->port = dest->udp_port ? dest->udp_port : (uint16_t)HML_ROCE_PORT;

  hml_status st = hml_sock_udp_create(&c->sock, 0, src_port, false);
  if (st == HML_OK) st = hml_sock_set_dest(c->sock, dest->ipv4, c->port);
  if (st != HML_OK) {
    /* Propagate: the core falls back to T0 (UDP) on any open failure. */
    hml_sock_close(c->sock);
    c->sock = NULL;
    free(c);
    return st;
  }

  x->impl = c;
  return HML_OK;
}

hml_status hml_roce_open_ex(hml_xport_t *x, const hml_dest_cfg *dest, uint16_t stream_id,
                            uint16_t src_port) {
  return roce_open_ex(x, dest, stream_id, src_port);
}

static hml_status roce_open(hml_xport_t *x, const hml_dest_cfg *dest, uint16_t stream_id) {
  return roce_open_ex(x, dest, stream_id, 0);
}

static hml_status roce_send_frag(hml_xport_t *x, const hml_frame_hdr *hdr,
                                 const uint8_t *payload, size_t payload_len) {
  if (!x || !x->impl || !hdr) return HML_ERR_ARG;
  if (payload_len > HML_ROCE_MAX_PAYLOAD) return HML_ERR_TOOLARGE;
  if (payload_len && !payload) return HML_ERR_ARG;

  hml_roce_ctx *c = (hml_roce_ctx *)x->impl;

  if (hdr->flags & HML_FLAG_START) {
    c->remote_cursor = 0;
    c->frame_bytes = 0;
    c->crc_state = HML_ROCE_CRC32_INIT;
    c->meta_sent = false;
    c->end_seen = false;
    c->seq_in_frame = 0;
  }
  c->seq_in_frame++;

  if (payload_len) {
    uint64_t va = c->dest.remote_addr + c->remote_cursor;
    hml_status st = roce_emit(c, va, c->dest.rkey, payload, payload_len, false, 0);
    if (st != HML_OK) return st;
    c->crc_state = hml_roce_crc32_update(c->crc_state, payload, payload_len);
    c->remote_cursor += payload_len;
    c->frame_bytes = c->remote_cursor;
  }

  if (hdr->flags & HML_FLAG_END) {
    c->end_seen = true;
    c->frame_seq = hdr->seq;
    c->frame_flags = hdr->flags;
    return roce_emit_meta(c);
  }
  return HML_OK;
}

static hml_status roce_flush(hml_xport_t *x) {
  if (!x || !x->impl) return HML_ERR_ARG;
  hml_roce_ctx *c = (hml_roce_ctx *)x->impl;
  if (c->end_seen && !c->meta_sent) return roce_emit_meta(c);
  return HML_OK;
}

static void roce_close(hml_xport_t *x) {
  if (!x) return;
  hml_roce_ctx *c = (hml_roce_ctx *)x->impl;
  if (!c) return;
  hml_sock_close(c->sock);
  c->sock = NULL;
  free(c);
  x->impl = NULL;
}

static const hml_xport_ops hml_roce_ops = {
    .name = "rocev2",
    .open = roce_open,
    .send_frag = roce_send_frag,
    .flush = roce_flush,
    .close = roce_close,
};

const hml_xport_ops *hml_rocev2_ops(void) { return &hml_roce_ops; }

hml_status hml_rocev2_register(void) { return hml_transport_register(&hml_roce_ops); }

hml_status hml_roce_set_qp_type(hml_xport_t *x, uint8_t qp_type) {
  if (!x || !x->impl) return HML_ERR_ARG;
  hml_roce_ctx *c = (hml_roce_ctx *)x->impl;
  c->qp_type = qp_type;
  return HML_OK;
}

hml_status hml_roce_set_imm_mode(hml_xport_t *x, bool enable, uint32_t imm_val) {
  if (!x || !x->impl) return HML_ERR_ARG;
  hml_roce_ctx *c = (hml_roce_ctx *)x->impl;
  c->use_imm = enable;
  c->imm_val = imm_val;
  return HML_OK;
}
