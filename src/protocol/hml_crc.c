#include "hml/hml_crc.h"

/* CCITT-16, poly 0x1021, init 0xFFFF, no final xor. */
uint16_t hml_crc16(const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  uint16_t crc = 0xFFFFu;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)((uint16_t)p[i] << 8);
    for (int b = 0; b < 8; b++) {
      if (crc & 0x8000u) crc = (uint16_t)((crc << 1) ^ 0x1021u);
      else crc = (uint16_t)(crc << 1);
    }
  }
  return crc;
}

/* IEEE 802.3 CRC-32 (reflected, poly 0xEDB88320), used for RoCEv2 ICRC payload. */
uint32_t hml_crc32_ieee(const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) {
      if (crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
      else crc >>= 1;
    }
  }
  return ~crc;
}
