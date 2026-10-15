/*
 * Discovery beacon codec (own control-plane discovery, Path A).
 *
 * Wire layout, all multi-byte fields big-endian, total HML_DISC_SIZE == 28:
 *
 *   off  size  field
 *     0    4   magic    u32  = HML_MAGIC_BEACON (0x484d4c42 'HMLB')
 *     4    1   version  u8   = HML_DISC_VERSION
 *     5    1   flags    u8   (reserved, 0)
 *     6    1   ttl      u8   (hop budget, always 1 for a direct LAN peer)
 *     7    1   rsvd     u8   (reserved, 0)
 *     8    4   uuid     u32  device-unique id
 *    12    4   caps     u32  HML_CAP_* bitmask
 *    16    4   fw_ver   u32  firmware version (major<<16 | minor<<8 | patch)
 *    20    6   ts_us    48-bit BE  low 48 bits of the u64 monotonic clock
 *    26    2   crc16    u16  = hml_crc16 over bytes [0, 26)
 *
 * NOTE on ts_us width: the frozen header comment lists a full u64 ts_us, which
 * would make the record 30 bytes, but HML_DISC_SIZE is fixed at 28 and the CRC
 * must cover exactly the first 26 bytes. Field order/offsets are kept exactly as
 * documented and ts_us is encoded as 48-bit BE instead: 2^48 microseconds is
 * ~8.9 years of monotonic uptime, far past any beacon use, and the record stays
 * exactly HML_DISC_SIZE bytes. hml_beacon_parse only accepts HML_DISC_SIZE.
 */

#include "hml/hml_discovery.h"
#include "hml/hml_crc.h"
#include "hml/hml_port.h"

#include <string.h>

#define HML_DISC_OFF_TS   20
#define HML_DISC_TS_BYTES 6
#define HML_DISC_CRC_OFF  26
#define HML_DISC_TS_MASK  0x0000ffffffffffffull

static void put_ts48(uint8_t *p, uint64_t ts_us) {
  for (int i = 0; i < HML_DISC_TS_BYTES; i++)
    p[i] = (uint8_t)(ts_us >> (8 * (HML_DISC_TS_BYTES - 1 - i)));
}

static uint64_t get_ts48(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < HML_DISC_TS_BYTES; i++) v = (v << 8) | p[i];
  return v;
}

void hml_beacon_fill(const hml_beacon *b, uint8_t *dst) {
  if (!b || !dst) return;
  hml_put_be32(dst, HML_MAGIC_BEACON);
  dst[4] = b->version;
  dst[5] = b->flags;
  dst[6] = b->ttl;
  dst[7] = 0;
  hml_put_be32(dst + 8, b->uuid);
  hml_put_be32(dst + 12, b->caps);
  hml_put_be32(dst + 16, b->fw_ver);
  put_ts48(dst + HML_DISC_OFF_TS, b->ts_us);
  hml_put_be16(dst + HML_DISC_CRC_OFF, hml_crc16(dst, HML_DISC_CRC_OFF));
}

hml_status hml_beacon_parse(const uint8_t *buf, size_t len, hml_beacon *out) {
  if (!buf || !out) return HML_ERR_ARG;
  if (len != HML_DISC_SIZE) return HML_ERR_PROTO;
  if (hml_get_be32(buf) != HML_MAGIC_BEACON) return HML_ERR_PROTO;
  if (hml_crc16(buf, HML_DISC_CRC_OFF) != hml_get_be16(buf + HML_DISC_CRC_OFF)) return HML_ERR_CRC;

  memset(out, 0, sizeof(*out));
  out->version = buf[4];
  out->flags = buf[5];
  out->ttl = buf[6];
  out->uuid = hml_get_be32(buf + 8);
  out->caps = hml_get_be32(buf + 12);
  out->fw_ver = hml_get_be32(buf + 16);
  out->ts_us = get_ts48(buf + HML_DISC_OFF_TS) & HML_DISC_TS_MASK;
  return HML_OK;
}
