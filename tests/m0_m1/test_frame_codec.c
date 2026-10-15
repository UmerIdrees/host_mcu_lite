/* M0: frame/config codec - golden vectors, round-trip, fail-closed parsing. */
#include "hml/hml_crc.h"
#include "hml/hml_protocol.h"
#include "hml_test_util.h"

#define HDR_TOTAL(len) ((size_t)HML_FRAME_HDR_SIZE + (size_t)(len) + (size_t)HML_TRAIL_CRC_SIZE)

/* Golden 28-byte header for the header described below (hdr_crc16 == 0xD373). */
static const uint8_t k_golden_hdr[HML_FRAME_HDR_SIZE] = {
    0x48, 0x4D, 0x4C, 0x31, 0x01, 0x03, 0x12, 0x34, 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x03, 0x00, 0x07,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x00, 0x00, 0xD3, 0x73};

/* Golden 40-byte hml_dest_cfg. */
static const uint8_t k_golden_cfg[HML_CFG_SIZE] = {
    0x00, 0x00, 0xD7, 0x3D, 0x7F, 0x00, 0x00, 0x01, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
    0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x00, 0x00, 0x23, 0x28, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00};

static void build(uint8_t *dg, const hml_frame_hdr *h, const uint8_t *payload, size_t len) {
  hml_proto_frame_fill(h, dg);
  if (len) memcpy(dg + HML_OFF_PAYLOAD, payload, len);
  hml_put_be16(dg + HML_OFF_PAYLOAD + len, hml_crc16(payload, len));
}

int main(void) {
  /* ---- golden header vector, field by field ---- */
  hml_frame_hdr h, o;
  h.stream_id = 0x1234u;
  h.seq = 0xDEADBEEFu;
  h.frag_idx = 3u;
  h.frag_cnt = 7u;
  h.ts_us = 0x0102030405060708ull;
  h.flags = (uint8_t)(HML_FLAG_START | HML_FLAG_END);
  h.payload_len = 0u;

  uint8_t dg[HML_MAX_FRAG_SIZE];
  hml_proto_frame_fill(&h, dg);
  CHECK_EQ_SIZE(HDR_TOTAL(0), 30u);
  CHECK(memcmp(dg, k_golden_hdr, HML_FRAME_HDR_SIZE) == 0);
  /* every field at its documented offset */
  CHECK_EQ_U32(hml_get_be32(dg + 0), HML_MAGIC_FRAME);
  CHECK_EQ_U32(dg[4], HML_PROTO_VERSION);
  CHECK_EQ_U32(dg[5], HML_FLAG_START | HML_FLAG_END);
  CHECK_EQ_U32(hml_get_be16(dg + 6), 0x1234u);
  CHECK_EQ_U32(hml_get_be32(dg + 8), 0xDEADBEEFu);
  CHECK_EQ_U32(hml_get_be16(dg + 12), 3u);
  CHECK_EQ_U32(hml_get_be16(dg + 14), 7u);
  CHECK_EQ_U32(hml_get_be64(dg + 16), 0x0102030405060708ull);
  CHECK_EQ_U32(hml_get_be16(dg + 24), 0u);
  CHECK_EQ_U32(hml_get_be16(dg + 26), 0xD373u);
  CHECK_EQ_U32(hml_crc16(dg, 26), 0xD373u);

  /* ---- full datagram round-trip (max fragment payload) ---- */
  uint8_t payload[HML_MAX_FRAG_PAYLOAD];
  for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 7u + 3u);
  h.stream_id = 0xBEEFu;
  h.seq = 42u;
  h.frag_idx = 0u;
  h.frag_cnt = 0u;
  h.ts_us = 123456789ull;
  h.flags = (uint8_t)HML_FLAG_START;
  h.payload_len = (uint16_t)sizeof(payload);
  build(dg, &h, payload, sizeof(payload));

  const uint8_t *pay = NULL;
  memset(&o, 0, sizeof(o));
  CHECK_STATUS(hml_proto_frame_parse(dg, HDR_TOTAL(sizeof(payload)), &o, &pay), HML_OK);
  CHECK_EQ_U32(o.stream_id, h.stream_id);
  CHECK_EQ_U32(o.seq, h.seq);
  CHECK_EQ_U32(o.frag_idx, h.frag_idx);
  CHECK_EQ_U32(o.frag_cnt, h.frag_cnt);
  CHECK_EQ_U32(o.ts_us, h.ts_us);
  CHECK_EQ_U32(o.flags, h.flags);
  CHECK_EQ_U32(o.payload_len, sizeof(payload));
  CHECK(pay == dg + HML_OFF_PAYLOAD);
  CHECK(pay && memcmp(pay, payload, sizeof(payload)) == 0);
  CHECK_EQ_SIZE(HML_MAX_FRAG_SIZE, 1458u);

  /* structure-only parse (payload == NULL) still validates the header */
  CHECK_STATUS(hml_proto_frame_parse(dg, HML_FRAME_HDR_SIZE, &o, NULL), HML_OK);
  CHECK_EQ_U32(o.payload_len, sizeof(payload));
  /* asking for the payload of a header-only buffer fails closed */
  const uint8_t *none = dg;
  CHECK_STATUS(hml_proto_frame_parse(dg, HML_FRAME_HDR_SIZE, &o, &none), HML_ERR_PROTO);
  CHECK(none == dg);

  /* ---- fail closed ---- */
  CHECK_STATUS(hml_proto_frame_parse(NULL, 30, &o, &pay), HML_ERR_ARG);
  CHECK_STATUS(hml_proto_frame_parse(dg, 30, NULL, &pay), HML_ERR_ARG);

  /* truncated datagram: 10 and 27 bytes */
  CHECK_STATUS(hml_proto_frame_parse(dg, 10, &o, &pay), HML_ERR_PROTO);
  CHECK_STATUS(hml_proto_frame_parse(dg, 27, &o, &pay), HML_ERR_PROTO);
  CHECK_STATUS(hml_proto_frame_parse(dg, 0, &o, &pay), HML_ERR_PROTO);
  /* header complete but payload/trailing crc missing */
  CHECK_STATUS(hml_proto_frame_parse(dg, HML_FRAME_HDR_SIZE, &o, &pay), HML_ERR_PROTO);
  CHECK_STATUS(hml_proto_frame_parse(dg, HDR_TOTAL(sizeof(payload)) - 1, &o, &pay), HML_ERR_PROTO);

  /* bad magic */
  memcpy(dg, k_golden_hdr, HML_FRAME_HDR_SIZE);
  build(dg, &h, payload, 0);
  dg[HML_OFF_MAGIC + 3] ^= 0xFFu;
  hml_put_be16(dg + HML_OFF_HDR_CRC, hml_crc16(dg, HML_OFF_HDR_CRC));
  CHECK_STATUS(hml_proto_frame_parse(dg, HDR_TOTAL(0), &o, &pay), HML_ERR_PROTO);

  /* unsupported version (re-CRC'd so the version check is what fails) */
  h.flags = (uint8_t)HML_FLAG_START;
  h.frag_idx = 0u;
  h.frag_cnt = 0u;
  h.payload_len = 0u;
  build(dg, &h, NULL, 0);
  dg[HML_OFF_VERSION] = 7u;
  hml_put_be16(dg + HML_OFF_HDR_CRC, hml_crc16(dg, HML_OFF_HDR_CRC));
  CHECK_STATUS(hml_proto_frame_parse(dg, HDR_TOTAL(0), &o, &pay), HML_ERR_PROTO);

  /* payload_len larger than HML_MAX_FRAG_PAYLOAD (re-CRC'd) */
  hml_put_be16(dg + HML_OFF_PAYLOAD_LEN, (uint16_t)(HML_MAX_FRAG_PAYLOAD + 1u));
  hml_put_be16(dg + HML_OFF_HDR_CRC, hml_crc16(dg, HML_OFF_HDR_CRC));
  CHECK_STATUS(hml_proto_frame_parse(dg, HDR_TOTAL(0), &o, &pay), HML_ERR_PROTO);

  /* corrupted header crc -> HML_ERR_CRC (both with and without payload) */
  build(dg, &h, payload, 16);
  dg[HML_OFF_HDR_CRC] ^= 0x01u;
  CHECK_STATUS(hml_proto_frame_parse(dg, HDR_TOTAL(16), &o, &pay), HML_ERR_CRC);
  CHECK_STATUS(hml_proto_frame_parse(dg, HML_FRAME_HDR_SIZE, &o, &pay), HML_ERR_CRC);

  /* corrupted trailing payload crc -> HML_ERR_CRC */
  h.payload_len = 16u;
  build(dg, &h, payload, 16);
  CHECK_EQ_U32(hml_get_be16(dg + HML_OFF_PAYLOAD_LEN), 16u);
  dg[HML_FRAME_HDR_SIZE + 16] ^= 0x80u; /* flip a payload bit: trailing crc now stale */
  CHECK_STATUS(hml_proto_frame_parse(dg, HDR_TOTAL(16), &o, &pay), HML_ERR_CRC);
  /* a payload byte change with a matching trailing crc still parses (hdr crc unaffected) */
  build(dg, &h, payload, 16);
  dg[HML_FRAME_HDR_SIZE + 3] ^= 0x08u;
  hml_put_be16(dg + HML_FRAME_HDR_SIZE + 16, hml_crc16(dg + HML_OFF_PAYLOAD, 16));
  CHECK_STATUS(hml_proto_frame_parse(dg, HDR_TOTAL(16), &o, &pay), HML_OK);
  CHECK(dg[HML_FRAME_HDR_SIZE + 3] == (uint8_t)(payload[3] ^ 0x08u));
  /* a corrupted payload byte with a stale trailing crc is caught */
  build(dg, &h, payload, 16);
  dg[HML_FRAME_HDR_SIZE + 5] ^= 0x40u;
  CHECK_STATUS(hml_proto_frame_parse(dg, HDR_TOTAL(16), &o, &pay), HML_ERR_CRC);

  /* zero-payload fragment with HML_FLAG_END parses fine */
  h.flags = (uint8_t)HML_FLAG_END;
  h.frag_idx = 5u;
  h.frag_cnt = 0u;
  h.payload_len = 0u;
  build(dg, &h, NULL, 0);
  CHECK_STATUS(hml_proto_frame_parse(dg, HDR_TOTAL(0), &o, &pay), HML_OK);
  CHECK_EQ_U32(o.payload_len, 0u);
  CHECK_EQ_U32(o.flags, HML_FLAG_END);
  CHECK_EQ_U32(o.frag_idx, 5u);
  CHECK(pay == dg + HML_OFF_PAYLOAD);
  CHECK_STATUS(hml_proto_frame_parse(dg, HML_FRAME_HDR_SIZE, &o, NULL), HML_OK);

  /* seq wrap round-trips */
  h.seq = 0xFFFFFFFFu;
  build(dg, &h, NULL, 0);
  CHECK_STATUS(hml_proto_frame_parse(dg, HDR_TOTAL(0), &o, &pay), HML_OK);
  CHECK_EQ_U32(o.seq, 0xFFFFFFFFu);
  h.seq = 0u;
  build(dg, &h, NULL, 0);
  CHECK_STATUS(hml_proto_frame_parse(dg, HDR_TOTAL(0), &o, &pay), HML_OK);
  CHECK_EQ_U32(o.seq, 0u);

  /* payload_len above the max is clamped by fill (never written oversized) */
  h.payload_len = 0xFFFFu;
  build(dg, &h, payload, 0);
  CHECK_EQ_U32(hml_get_be16(dg + HML_OFF_PAYLOAD_LEN), HML_MAX_FRAG_PAYLOAD);

  /* ---- dest cfg round-trip ---- */
  CHECK_EQ_SIZE(HML_CFG_SIZE, 40u);
  hml_dest_cfg c, c2;
  memset(&c, 0, sizeof(c));
  c.kind = HML_XPORT_UDP;
  c.ipv4 = 0x7F000001u;
  c.udp_port = 55101u;
  c.qp = 0x11223344u;
  c.psn = 0x55667788u;
  c.rkey = 0x99AABBCCu;
  c.remote_addr = 0x0011223344556677ull;
  c.frame_size = 9000u;
  c.window_len = 64u;
  c.hop_limit = 64u;

  uint8_t cb[HML_CFG_SIZE];
  hml_proto_cfg_fill(&c, cb);
  CHECK(memcmp(cb, k_golden_cfg, HML_CFG_SIZE) == 0);
  memset(&c2, 0xAB, sizeof(c2));
  CHECK_STATUS(hml_proto_cfg_parse(cb, HML_CFG_SIZE, &c2), HML_OK);
  CHECK_EQ_U32(c2.kind, c.kind);
  CHECK_EQ_U32(c2.ipv4, c.ipv4);
  CHECK_EQ_U32(c2.udp_port, c.udp_port);
  CHECK_EQ_U32(c2.qp, c.qp);
  CHECK_EQ_U32(c2.psn, c.psn);
  CHECK_EQ_U32(c2.rkey, c.rkey);
  CHECK_EQ_U32(c2.remote_addr, c.remote_addr);
  CHECK_EQ_U32(c2.frame_size, c.frame_size);
  CHECK_EQ_U32(c2.window_len, c.window_len);
  CHECK_EQ_U32(c2.hop_limit, c.hop_limit);
  CHECK(memcmp(c2.rsvd, "\0\0\0\0\0", 5) == 0);

  /* rocev2 kind survives too */
  c.kind = HML_XPORT_ROCEV2;
  hml_proto_cfg_fill(&c, cb);
  CHECK_EQ_U32(cb[0], (uint8_t)HML_XPORT_ROCEV2);
  CHECK_STATUS(hml_proto_cfg_parse(cb, HML_CFG_SIZE, &c2), HML_OK);
  CHECK_EQ_U32(c2.kind, HML_XPORT_ROCEV2);

  /* cfg parse fails closed */
  CHECK_STATUS(hml_proto_cfg_parse(NULL, HML_CFG_SIZE, &c2), HML_ERR_ARG);
  CHECK_STATUS(hml_proto_cfg_parse(cb, HML_CFG_SIZE, NULL), HML_ERR_ARG);
  CHECK_STATUS(hml_proto_cfg_parse(cb, 0, &c2), HML_ERR_PROTO);
  CHECK_STATUS(hml_proto_cfg_parse(cb, HML_CFG_SIZE - 1, &c2), HML_ERR_PROTO);
  CHECK_STATUS(hml_proto_cfg_parse(cb, 39, &c2), HML_ERR_PROTO);

  hml_proto_cfg_fill(&c, cb);
  cb[0] = 9u; /* bad kind */
  CHECK_STATUS(hml_proto_cfg_parse(cb, HML_CFG_SIZE, &c2), HML_ERR_PROTO);
  hml_proto_cfg_fill(&c, cb);
  cb[1] = 1u; /* pad must be zero */
  CHECK_STATUS(hml_proto_cfg_parse(cb, HML_CFG_SIZE, &c2), HML_ERR_PROTO);
  hml_proto_cfg_fill(&c, cb);
  cb[35] = 1u; /* pad1 */
  CHECK_STATUS(hml_proto_cfg_parse(cb, HML_CFG_SIZE, &c2), HML_ERR_PROTO);
  hml_proto_cfg_fill(&c, cb);
  cb[36] = 0x80u; /* rsvd */
  CHECK_STATUS(hml_proto_cfg_parse(cb, HML_CFG_SIZE, &c2), HML_ERR_PROTO);
  hml_proto_cfg_fill(&c, cb);
  cb[HML_CFG_SIZE - 1] = 0x01u; /* rsvd */
  CHECK_STATUS(hml_proto_cfg_parse(cb, HML_CFG_SIZE, &c2), HML_ERR_PROTO);

  /* trailing garbage beyond HML_CFG_SIZE is ignored, not fatal */
  hml_proto_cfg_fill(&c, cb);
  cb[HML_CFG_SIZE - 1] = 0u;
  uint8_t big[HML_CFG_SIZE + 8];
  memcpy(big, cb, HML_CFG_SIZE);
  memset(big + HML_CFG_SIZE, 0xEE, 8);
  CHECK_STATUS(hml_proto_cfg_parse(big, sizeof(big), &c2), HML_OK);

  TEST_DONE();
}
