#include "hml/hml_protocol.h"

#include "hml/hml_crc.h"

#include <string.h>

/*
 * Frame datagram (T0 UDP; also the inner block of T1a):
 *
 *   off  size  field
 *     0     4  magic          u32 BE  HML_MAGIC_FRAME
 *     4     1  version        u8      HML_PROTO_VERSION
 *     5     1  flags          u8      HML_FLAG_START | HML_FLAG_END
 *     6     2  stream_id      u16 BE
 *     8     4  seq            u32 BE
 *    12     2  frag_idx       u16 BE
 *    14     2  frag_cnt       u16 BE
 *    16     8  ts_us          u64 BE
 *    24     2  payload_len    u16 BE  <= HML_MAX_FRAG_PAYLOAD
 *    26     2  hdr_crc16      u16 BE  CCITT over bytes [0,26)
 *    28   len  payload
 *   28+len  2  payload_crc16  u16 BE  CCITT over the payload bytes only
 *
 * Total datagram size == 28 + payload_len + 2.
 *
 * hml_dest_cfg wire form (HML_CFG_SIZE == 40, all multi-byte fields BE):
 *
 *   off  size  field
 *     0     1  kind           u8      hml_xport_kind
 *     1     1  pad0           u8      MUST be 0 on the wire
 *     2     2  udp_port       u16 BE
 *     4     4  ipv4           u32 BE  network byte order
 *     8     4  qp             u32 BE
 *    12     4  psn            u32 BE
 *    16     4  rkey           u32 BE
 *    20     8  remote_addr    u64 BE
 *    28     4  frame_size     u32 BE
 *    32     2  window_len     u16 BE
 *    34     1  hop_limit      u8
 *    35     1  pad1           u8      MUST be 0 on the wire (rsvd[0])
 *    36     4  rsvd[1..4]     u8[4]   MUST be 0 on the wire
 */

/* Offsets of the serialized hml_dest_cfg (private to this file). */
enum {
  CFG_OFF_KIND = 0, CFG_OFF_PAD0 = 1, CFG_OFF_UDP_PORT = 2, CFG_OFF_IPV4 = 4,
  CFG_OFF_QP = 8, CFG_OFF_PSN = 12, CFG_OFF_RKEY = 16, CFG_OFF_REMOTE_ADDR = 20,
  CFG_OFF_FRAME_SIZE = 28, CFG_OFF_WINDOW_LEN = 32, CFG_OFF_HOP_LIMIT = 34,
  CFG_OFF_PAD1 = 35, CFG_OFF_RSVD = 36
};

void hml_proto_frame_fill(const hml_frame_hdr *h, uint8_t *dst) {
  if (!h || !dst) return;

  uint16_t plen = h->payload_len;
  if (plen > (uint16_t)HML_MAX_FRAG_PAYLOAD) plen = (uint16_t)HML_MAX_FRAG_PAYLOAD;

  memset(dst, 0, (size_t)HML_FRAME_HDR_SIZE);
  hml_put_be32(dst + HML_OFF_MAGIC, HML_MAGIC_FRAME);
  dst[HML_OFF_VERSION] = (uint8_t)HML_PROTO_VERSION;
  dst[HML_OFF_FLAGS] = h->flags;
  hml_put_be16(dst + HML_OFF_STREAM, h->stream_id);
  hml_put_be32(dst + HML_OFF_SEQ, h->seq);
  hml_put_be16(dst + HML_OFF_FRAG_IDX, h->frag_idx);
  hml_put_be16(dst + HML_OFF_FRAG_CNT, h->frag_cnt);
  hml_put_be64(dst + HML_OFF_TS, h->ts_us);
  hml_put_be16(dst + HML_OFF_PAYLOAD_LEN, plen);
  hml_put_be16(dst + HML_OFF_HDR_CRC, hml_crc16(dst, (size_t)HML_OFF_HDR_CRC));
}

hml_status hml_proto_frame_parse(const uint8_t *datagram, size_t len,
                                 hml_frame_hdr *out, const uint8_t **payload) {
  if (!datagram || !out) return HML_ERR_ARG;
  if (len < (size_t)HML_FRAME_HDR_SIZE) return HML_ERR_PROTO;

  if (hml_get_be32(datagram + HML_OFF_MAGIC) != HML_MAGIC_FRAME) return HML_ERR_PROTO;
  if (datagram[HML_OFF_VERSION] != (uint8_t)HML_PROTO_VERSION) return HML_ERR_PROTO;

  uint16_t plen = hml_get_be16(datagram + HML_OFF_PAYLOAD_LEN);
  if (plen > (uint16_t)HML_MAX_FRAG_PAYLOAD) return HML_ERR_PROTO;

  if (hml_get_be16(datagram + HML_OFF_HDR_CRC) != hml_crc16(datagram, (size_t)HML_OFF_HDR_CRC))
    return HML_ERR_CRC;

  if (payload) {
    /* payload != NULL: the caller wants the payload, so the whole datagram must be there. */
    if (len < (size_t)HML_FRAME_HDR_SIZE + (size_t)plen + (size_t)HML_TRAIL_CRC_SIZE)
      return HML_ERR_PROTO;
    const uint8_t *p = datagram + HML_OFF_PAYLOAD;
    if (hml_get_be16(p + plen) != hml_crc16(p, (size_t)plen)) return HML_ERR_CRC;
    *payload = p;
  }

  out->flags = datagram[HML_OFF_FLAGS];
  out->stream_id = hml_get_be16(datagram + HML_OFF_STREAM);
  out->seq = hml_get_be32(datagram + HML_OFF_SEQ);
  out->frag_idx = hml_get_be16(datagram + HML_OFF_FRAG_IDX);
  out->frag_cnt = hml_get_be16(datagram + HML_OFF_FRAG_CNT);
  out->ts_us = hml_get_be64(datagram + HML_OFF_TS);
  out->payload_len = plen;
  return HML_OK;
}

void hml_proto_cfg_fill(const hml_dest_cfg *cfg, uint8_t *dst) {
  if (!cfg || !dst) return;

  memset(dst, 0, HML_CFG_SIZE);
  dst[CFG_OFF_KIND] = (uint8_t)cfg->kind;
  dst[CFG_OFF_PAD0] = 0;
  hml_put_be16(dst + CFG_OFF_UDP_PORT, cfg->udp_port);
  hml_put_be32(dst + CFG_OFF_IPV4, cfg->ipv4);
  hml_put_be32(dst + CFG_OFF_QP, cfg->qp);
  hml_put_be32(dst + CFG_OFF_PSN, cfg->psn);
  hml_put_be32(dst + CFG_OFF_RKEY, cfg->rkey);
  hml_put_be64(dst + CFG_OFF_REMOTE_ADDR, cfg->remote_addr);
  hml_put_be32(dst + CFG_OFF_FRAME_SIZE, cfg->frame_size);
  hml_put_be16(dst + CFG_OFF_WINDOW_LEN, cfg->window_len);
  dst[CFG_OFF_HOP_LIMIT] = cfg->hop_limit;
  dst[CFG_OFF_PAD1] = 0;
  memcpy(dst + CFG_OFF_RSVD, cfg->rsvd + 1, 4);
}

hml_status hml_proto_cfg_parse(const uint8_t *src, size_t len, hml_dest_cfg *out) {
  if (!src || !out) return HML_ERR_ARG;
  if (len < (size_t)HML_CFG_SIZE) return HML_ERR_PROTO;

  uint8_t kind = src[CFG_OFF_KIND];
  if (kind != (uint8_t)HML_XPORT_UDP && kind != (uint8_t)HML_XPORT_ROCEV2)
    return HML_ERR_PROTO;
  if (src[CFG_OFF_PAD0] != 0u) return HML_ERR_PROTO;
  if (src[CFG_OFF_PAD1] != 0u) return HML_ERR_PROTO;
  for (unsigned i = 0; i < 4; i++)
    if (src[CFG_OFF_RSVD + i] != 0u) return HML_ERR_PROTO;

  memset(out, 0, sizeof(*out));
  out->kind = (hml_xport_kind)kind;
  out->udp_port = hml_get_be16(src + CFG_OFF_UDP_PORT);
  out->ipv4 = hml_get_be32(src + CFG_OFF_IPV4);
  out->qp = hml_get_be32(src + CFG_OFF_QP);
  out->psn = hml_get_be32(src + CFG_OFF_PSN);
  out->rkey = hml_get_be32(src + CFG_OFF_RKEY);
  out->remote_addr = hml_get_be64(src + CFG_OFF_REMOTE_ADDR);
  out->frame_size = hml_get_be32(src + CFG_OFF_FRAME_SIZE);
  out->window_len = hml_get_be16(src + CFG_OFF_WINDOW_LEN);
  out->hop_limit = src[CFG_OFF_HOP_LIMIT];
  out->pad = 0;
  out->rsvd[0] = 0;
  for (unsigned i = 1; i < 5; i++) out->rsvd[i] = src[CFG_OFF_RSVD + (i - 1)];
  return HML_OK;
}
