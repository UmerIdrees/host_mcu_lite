/*
 * M3 / T1a -- host-side RoCEv2 receiver / demux ("software NIC").
 *
 * Parsed datagram layout (offsets from the first byte received):
 *
 *    0   ..  IHL*4   IPv4 header; IHL is read from the low nibble of byte 0 and
 *                     options are skipped, so IHL > 5 is handled. Requires
 *                     version 4, IHL >= 5 and protocol 17.
 *  IHL   + 0  2      UDP source port
 *  IHL   + 2  2      UDP destination port (must equal cfg->udp_port)
 *  IHL   + 4  2      UDP length (clamped to what actually arrived)
 *  IHL   + 6  2      UDP checksum (accepted, not verified -- the ICRC is the
 *                     integrity mechanism on a link-layer CRC'd fabric)
 *  IHL+8  + 0  1      BTH OpCode: 0x2A UC RDMA Write (also accepts 0x0A RC)
 *             + 1  1   BTH flags
 *             + 2  1   BTH reserved
 *             + 3  3   PSN, 24-bit big-endian
 *             + 6  2   AETH
 *             + 8  4   BTH reserved
 *  IHL+8  +12  8      RETH virtual address, big-endian
 *             +20  4   RETH rkey, big-endian
 *             +24  4   RETH length, big-endian (low 24 bits; bit 31 is "D")
 *  IHL+8  +28  n      write payload
 *  IHL+8  +28+n 4     ICRC, LITTLE-endian = CRC-32/IEEE over [BTH .. payload),
 *                             i.e. excluding the IP/UDP headers and the ICRC
 *
 * Frame completion: one write whose VA is exactly cfg->window_base +
 * cfg->frame_size is the meta slot (it may sit just past window_len_bytes, so
 * it is recognised before the window bounds are applied). Its payload is the
 * 20-byte little-endian record {u32 magic, u16 stream_id, u16 flags,
 * u32 frame_seq, u32 frame_bytes, u32 frame_crc32}. On magic + CRC match the
 * assembled window is delivered once to the callback and the window is zeroed for
 * the next frame. A meta whose frame_seq repeats the last delivered frame is
 * ignored, so a frame is never delivered twice, and a meta arriving with a
 * stale (backwards) PSN is ignored without touching any counter.
 */

#include "hml_roce_demux.h"

#include "hml/hml_crc.h"
#include "hml/hml_port.h"
#include "hml/hml_protocol.h"
#include "hml/hml_sock.h"

#include <stdlib.h>
#include <string.h>

/* ---- receive-side wire constants (mirror of src/transports/rocev2) -------- */
#define HML_RX_IP_MIN 20u
#define HML_RX_IP_MAX 60u
#define HML_RX_UDP_LEN 8u
#define HML_RX_PROTO_UDP 17u
#define HML_RX_BTH_LEN 12u
#define HML_RX_RETH_LEN 16u
#define HML_RX_BTH_OFF_OPCODE 0u
#define HML_RX_BTH_OFF_PSN 3u /* 24-bit, big-endian */
#define HML_RX_RETH_OFF_VA 0u
#define HML_RX_RETH_OFF_RKEY 8u
#define HML_RX_RETH_OFF_LEN 12u
#define HML_RX_ICRC_LEN 4u
#define HML_RX_PSN_MASK 0x00FFFFFFu
#define HML_RX_OPCODE_UC_RDMA_WRITE 0x2Au
#define HML_RX_OPCODE_RC_RDMA_WRITE 0x0Au
#define HML_RX_META_LEN 20u
#define HML_RX_MAX_DG 2048u
#define HML_RX_MAX_WINDOW (16u * 1024u * 1024u)

struct hml_roce_rx {
  hml_sock_t *sock;
  hml_roce_cfg cfg;
  hml_roce_eof_cb cb;
  void *cb_ctx;
  uint8_t *win;
  uint8_t *dg;
  hml_roce_stats st;
  bool have_psn;
  uint32_t last_psn;
  bool have_last_frame;
  uint32_t last_frame_seq;
};

/*
 * CRC-32/IEEE table (reflected, poly 0xEDB88320), generated as
 *   tab[i] = the byte-wise reflected CRC-32 of the value i
 * It is the standard zlib-style table; hml_crc32_ieee() walks the same algorithm
 * one bit at a time and costs ~100 us per KiB on the reference host, which is
 * ~100x too slow for a 1.5 KiB-per-packet data path. Every use of this table is
 * pinned against hml_crc32_ieee() by tests/m3/test_roce_hdr.c.
 */
static const uint32_t hml_rx_crc32_tab[256] = {
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
/* Table-driven twin of hml_crc32_ieee(); pinned against it by tests/m3. */
static uint32_t rx_crc32(uint32_t state, const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t crc = state;
  for (size_t i = 0; i < n; i++)
    crc = (crc >> 8) ^ hml_rx_crc32_tab[(crc ^ p[i]) & 0xFFu];
  return crc;
}

static uint32_t rx_crc32_all(const void *data, size_t n) {
  return ~rx_crc32(0xFFFFFFFFu, data, n);
}

/* ---- little-endian record access ---------------------------------------- */

static uint16_t rx_le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rx_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* ---- per-datagram receive path ------------------------------------------ */

static void rx_meta(hml_roce_rx_t *r, const uint8_t *m, uint32_t len) {
  if (len != HML_RX_META_LEN) {
    r->st.meta_drops++;
    return;
  }
  uint32_t magic = rx_le32(m + 0);
  const uint16_t stream_id = rx_le16(m + 4); /* informational: one window == one stream */
  const uint16_t mflags = rx_le16(m + 6);    /* HML_FLAG_* of the last fragment */
  uint32_t frame_seq = rx_le32(m + 8);
  uint32_t frame_bytes = rx_le32(m + 12);
  uint32_t frame_crc = rx_le32(m + 16);
  (void)stream_id;
  (void)mflags;

  if (magic != HML_MAGIC_FRAME) {
    r->st.meta_drops++;
    return;
  }
  /* A frame is never delivered twice. */
  if (r->have_last_frame && frame_seq == r->last_frame_seq) return;
  if (frame_bytes > r->cfg.frame_size || frame_bytes > r->cfg.window_len_bytes) {
    r->st.meta_drops++;
    memset(r->win, 0, r->cfg.window_len_bytes);
    return;
  }

  uint32_t crc = rx_crc32_all(r->win, frame_bytes);
  if (crc != frame_crc) {
    r->st.meta_drops++;
    memset(r->win, 0, r->cfg.window_len_bytes);
    return;
  }

  r->have_last_frame = true;
  r->last_frame_seq = frame_seq;
  r->st.eof_frames++;
  if (r->cb) r->cb(r->cb_ctx, frame_seq, r->win, frame_bytes, crc);
  memset(r->win, 0, r->cfg.window_len_bytes);
}

static void rx_datagram(hml_roce_rx_t *r, const uint8_t *d, size_t n) {
  r->st.pkts++;

  if (n < HML_RX_IP_MIN) {
    r->st.proto_err++;
    return;
  }
  size_t ihl = (size_t)(d[0] & 0x0Fu) * 4u; /* IP options are skipped here */
  if ((uint8_t)(d[0] >> 4) != 4u || ihl < HML_RX_IP_MIN || ihl > HML_RX_IP_MAX ||
      n < ihl + HML_RX_UDP_LEN) {
    r->st.proto_err++;
    return;
  }
  if (d[9] != HML_RX_PROTO_UDP) {
    r->st.proto_err++;
    return;
  }

  const uint8_t *udp = d + ihl;
  if (hml_get_be16(udp + 2) != r->cfg.udp_port) {
    r->st.proto_err++;
    return;
  }
  size_t ulen = hml_get_be16(udp + 4);
  if (ulen < HML_RX_UDP_LEN) {
    r->st.proto_err++;
    return;
  }
  size_t paylen = ulen - HML_RX_UDP_LEN;
  if (ihl + ulen > n) paylen = n - ihl - HML_RX_UDP_LEN; /* truncated on the wire */

  const size_t fixed = HML_RX_BTH_LEN + HML_RX_RETH_LEN;
  if (paylen < fixed + HML_RX_ICRC_LEN) {
    r->st.proto_err++;
    return;
  }

  const uint8_t *bth = udp + HML_RX_UDP_LEN;
  const size_t cov = paylen - HML_RX_ICRC_LEN; /* ICRC coverage = BTH..payload */
  const uint8_t *icrc = bth + cov;

  /* ICRC first: an integrity failure invalidates everything behind it. */
  if (r->cfg.verify_icrc) {
    uint32_t got = rx_le32(icrc);
    if (got != rx_crc32_all(bth, cov)) {
      r->st.icrc_err++;
      return;
    }
  }

  uint8_t op = bth[HML_RX_BTH_OFF_OPCODE];
  if (op != HML_RX_OPCODE_UC_RDMA_WRITE && op != HML_RX_OPCODE_RC_RDMA_WRITE) {
    r->st.proto_err++;
    return;
  }

  const uint8_t *reth = bth + HML_RX_BTH_LEN;
  uint64_t va = hml_get_be64(reth + HML_RX_RETH_OFF_VA);
  uint32_t rkey = hml_get_be32(reth + HML_RX_RETH_OFF_RKEY);
  uint32_t wlen = hml_get_be32(reth + HML_RX_RETH_OFF_LEN) & HML_RX_PSN_MASK;
  const uint8_t *pay = reth + HML_RX_RETH_LEN;
  if (wlen != cov - fixed) { /* no immediate, no padding inside the datagram */
    r->st.proto_err++;
    return;
  }
  if (r->cfg.rkey != 0 && rkey != r->cfg.rkey) {
    r->st.proto_err++;
    return;
  }

  const bool is_meta = (va == r->cfg.window_base + (uint64_t)r->cfg.frame_size);
  if (!is_meta) {
    uint64_t off = va - r->cfg.window_base;
    if (va < r->cfg.window_base || off > r->cfg.window_len_bytes ||
        wlen > r->cfg.window_len_bytes - off) {
      r->st.proto_err++;
      return;
    }
  }

  /* PSN continuity. Packets dropped above (bad ICRC / bad rkey / bad VA) never
   * advance the receiver PSN, so they show up as a gap on the next datagram. */
  uint32_t psn = ((uint32_t)bth[HML_RX_BTH_OFF_PSN] << 16) |
                 ((uint32_t)bth[HML_RX_BTH_OFF_PSN + 1] << 8) |
                 (uint32_t)bth[HML_RX_BTH_OFF_PSN + 2];
  if (r->have_psn) {
    uint32_t fwd = (psn - r->last_psn) & HML_RX_PSN_MASK;
    if (fwd != 1u) {
      if (fwd >= 0x800000u) return; /* stale / duplicated PSN: ignore it */
      r->st.psn_gaps++;
    }
  }
  r->have_psn = true;
  r->last_psn = psn;

  if (is_meta) {
    rx_meta(r, pay, wlen);
    return;
  }
  memcpy(r->win + (size_t)(va - r->cfg.window_base), pay, wlen);
  r->st.writes++;
  r->st.bytes += wlen;
}

/* ---- public API --------------------------------------------------------- */

hml_status hml_roce_rx_open(hml_roce_rx_t **out, const hml_roce_cfg *cfg, hml_roce_eof_cb cb,
                            void *ctx) {
  if (!out || !cfg) return HML_ERR_ARG;
  *out = NULL;
  if (cfg->window_len_bytes == 0 || cfg->window_len_bytes > HML_RX_MAX_WINDOW) return HML_ERR_ARG;
  if (cfg->udp_port == 0) return HML_ERR_ARG;
  if (cfg->frame_size == 0) return HML_ERR_ARG; /* meta slot would alias the data window */

  hml_roce_rx_t *r = (hml_roce_rx_t *)calloc(1, sizeof(*r));
  if (!r) return HML_ERR_NOMEM;
  r->win = (uint8_t *)calloc(1, cfg->window_len_bytes);
  r->dg = (uint8_t *)calloc(1, HML_RX_MAX_DG);
  if (!r->win || !r->dg) {
    free(r->win);
    free(r->dg);
    free(r);
    return HML_ERR_NOMEM;
  }
  r->cfg = *cfg;
  r->cb = cb;
  r->cb_ctx = ctx;

  hml_status st = hml_sock_udp_create(&r->sock, 0, cfg->udp_port, false);
  if (st != HML_OK) {
    free(r->win);
    free(r->dg);
    free(r);
    return st;
  }

  *out = r;
  return HML_OK;
}

hml_status hml_roce_rx_poll(hml_roce_rx_t *r, int timeout_ms) {
  if (!r) return HML_ERR_ARG;

  bool got = false;
  for (;;) {
    size_t n = 0;
    hml_status st = hml_sock_recv(r->sock, r->dg, HML_RX_MAX_DG, &n, got ? 0 : timeout_ms);
    if (st == HML_OK) {
      got = true;
      rx_datagram(r, r->dg, n);
      continue;
    }
    if (st == HML_ERR_AGAIN) break; /* nothing queued (or interrupted) */
    if (st == HML_ERR_TIMEOUT) return got ? HML_OK : HML_ERR_TIMEOUT;
    return got ? HML_OK : st;
  }
  if (got) return HML_OK;
  if (timeout_ms < 0) return HML_ERR_AGAIN;
  return timeout_ms == 0 ? HML_ERR_AGAIN : HML_ERR_TIMEOUT;
}

void hml_roce_rx_stats(const hml_roce_rx_t *r, hml_roce_stats *st) {
  if (!st) return;
  if (!r) {
    memset(st, 0, sizeof(*st));
    return;
  }
  *st = r->st;
}

void hml_roce_rx_close(hml_roce_rx_t *r) {
  if (!r) return;
  hml_sock_close(r->sock);
  r->sock = NULL;
  free(r->win);
  free(r->dg);
  free(r);
}
