#ifndef HML_DISCOVERY_H
#define HML_DISCOVERY_H

#include "hml_types.h"

/*
 * Discovery beacon (own, Path A). UDP broadcast/multicast-friendly.
 * [magic u32 BE][version u8][flags u8][ttl u8][rsvd u8][uuid u32 BE][caps u32 BE][fw_ver u32 BE][ts_us u64 BE][crc16 BE]
 */

#define HML_MAGIC_BEACON 0x484d4c42u /* 'HMLB' */
#define HML_DISC_VERSION 1u
#define HML_DISC_SIZE 28u

#define HML_CAP_UDP    (1u<<0)
#define HML_CAP_ROCEV2 (1u<<1)
#define HML_CAP_BRIDGE (1u<<2)
#define HML_CAP_GPU    (1u<<3)
#define HML_CAP_MCU    (1u<<4)

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif /* HML_DISCOVERY_H */