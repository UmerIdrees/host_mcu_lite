/*
 * Control message codec (Path A control plane, UDP req/resp).
 *
 * Wire layout, all multi-byte fields big-endian, fixed 12-byte header:
 *
 *   off  size  field
 *     0    4   magic     u32  = HML_MAGIC_CTRL (0x484d4c43 'HMLC')
 *     4    1   version   u8   = HML_CTRL_VERSION
 *     5    1   op        u8   = HML_OP_*
 *     6    2   flags     u16  = HML_CTRL_FLAG_*
 *     8    2   len       u16  = payload length (<= HML_CTRL_MAX_PAYLOAD)
 *    10    2   rsvd      u16  = 0 (pad, keeps the header fixed at 12 bytes)
 *    12  len   payload
 * 12+len   2   crc16     u16  = hml_crc16 over bytes [0, 12+len)
 *
 * Total datagram size = 12 + len + 2. hml_ctrl_parse requires the buffer
 * length to equal that total exactly (fail closed on truncate/trailing junk).
 */

#include "hml/hml_ctrl.h"
#include "hml/hml_crc.h"
#include "hml/hml_port.h"

#include <string.h>

#define HML_CTRL_RSVD_OFF 10u
#define HML_CTRL_CRC_OFF  12u

void hml_ctrl_fill(const hml_ctrl_msg *m, uint8_t *dst, uint16_t max_dst, uint16_t *out_n) {
  if (out_n) *out_n = 0;
  if (!m || !dst || !out_n) return;
  if (m->payload_len > HML_CTRL_MAX_PAYLOAD) return;
  if (m->payload_len > 0 && !m->payload) return;

  uint16_t need = (uint16_t)(HML_CTRL_RSVD_OFF + 2u + m->payload_len + 2u);
  if (max_dst < need) return; /* fail-safe: write nothing at all */

  hml_put_be32(dst, HML_MAGIC_CTRL);
  dst[4] = (uint8_t)HML_CTRL_VERSION;
  dst[5] = m->op;
  hml_put_be16(dst + 6, m->flags);
  hml_put_be16(dst + 8, m->payload_len);
  hml_put_be16(dst + HML_CTRL_RSVD_OFF, 0);
  if (m->payload_len > 0) memcpy(dst + HML_CTRL_CRC_OFF, m->payload, m->payload_len);

  hml_put_be16(dst + HML_CTRL_CRC_OFF + m->payload_len,
               hml_crc16(dst, (size_t)HML_CTRL_CRC_OFF + m->payload_len));
  *out_n = need;
}

hml_status hml_ctrl_parse(const uint8_t *buf, size_t len, hml_ctrl_msg *out,
                          const uint8_t **payload) {
  if (!buf || !out || !payload) return HML_ERR_ARG;

  memset(out, 0, sizeof(*out));
  *payload = NULL;

  if (len < (size_t)HML_CTRL_CRC_OFF + 2u) return HML_ERR_PROTO;
  if (hml_get_be32(buf) != HML_MAGIC_CTRL) return HML_ERR_PROTO;
  if (buf[4] != (uint8_t)HML_CTRL_VERSION) return HML_ERR_PROTO;

  uint16_t plen = hml_get_be16(buf + 8);
  if (plen > HML_CTRL_MAX_PAYLOAD) return HML_ERR_PROTO;
  if (len != (size_t)HML_CTRL_CRC_OFF + plen + 2u) return HML_ERR_PROTO;

  size_t covered = (size_t)HML_CTRL_CRC_OFF + plen;
  if (hml_crc16(buf, covered) != hml_get_be16(buf + covered)) return HML_ERR_CRC;

  out->op = buf[5];
  out->flags = hml_get_be16(buf + 6);
  out->payload_len = plen;
  *payload = buf + HML_CTRL_CRC_OFF; /* points into buf, never copied */
  out->payload = *payload;
  return HML_OK;
}
