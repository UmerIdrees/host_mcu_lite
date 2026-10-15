#ifndef HML_PORT_H
#define HML_PORT_H

#include "hml_types.h"

/*
 * Platform detection. Ports may predefine their own tag (e.g. -DHML_PORT_FREERTOS)
 * or rely on compiler macros below. There must be NO #ifdef inside src/core;
 * portability lives entirely in ports/.
 *
 * HML_NO_POSIX is the bare-metal / RTOS opt-out: define it and this header will
 * never tag the build as HML_PORT_POSIX, so a cross-compiled firmware cannot
 * accidentally be built against the POSIX port. A port that really is POSIX may
 * still predefine HML_PORT_POSIX explicitly, but combining the two is a
 * configuration error and is rejected below. See docs/final_partitioning.md.
 */

#if defined(HML_NO_POSIX) && defined(HML_PORT_POSIX)
#  error "HML_NO_POSIX and HML_PORT_POSIX are mutually exclusive: an embedded build must not pull in the POSIX port"
#endif

#if defined(HML_PORT_POSIX)
#elif defined(HML_NO_POSIX)
/* embedded/RTOS: no automatic POSIX tag, no automatic FreeRTOS tag either */
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
#define HML_PORT_POSIX
#elif defined(HML_PORT_FREERTOS)
#elif defined(FREERTOS) || defined(ESP32) || defined(ESP_PLATFORM)
#define HML_PORT_FREERTOS
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic clock in microseconds (per-device, no wall sync required). */
HML_API uint64_t hml_clock_us(void);

/* Big-endian host<->wire helpers (wire format is big-endian per protocol). */
HML_INLINE uint16_t hml_bswap16(uint16_t v) {
  return (uint16_t)((v >> 8) | (v << 8));
}
HML_INLINE uint32_t hml_bswap32(uint32_t v) {
  return ((v >> 24) & 0xffu) | ((v >> 8) & 0xff00u) |
         ((v << 8) & 0xff0000u) | ((v << 24) & 0xff000000u);
}
HML_INLINE uint64_t hml_bswap64(uint64_t v) {
  return (((uint64_t)hml_bswap32((uint32_t)(v & 0xffffffffu))) << 32) |
         (uint64_t)hml_bswap32((uint32_t)(v >> 32));
}

/* Store/load big-endian from an unaligned byte pointer. */
HML_INLINE void hml_put_be16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
HML_INLINE uint16_t hml_get_be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
HML_INLINE void hml_put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
HML_INLINE uint32_t hml_get_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
HML_INLINE void hml_put_be64(uint8_t *p, uint64_t v) {
  hml_put_be32(p, (uint32_t)(v >> 32)); hml_put_be32(p + 4, (uint32_t)v);
}
HML_INLINE uint64_t hml_get_be64(const uint8_t *p) {
  return ((uint64_t)hml_get_be32(p) << 32) | hml_get_be32(p + 4);
}

#ifdef __cplusplus
}
#endif

#endif /* HML_PORT_H */