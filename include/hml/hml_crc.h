#ifndef HML_CRC_H
#define HML_CRC_H

#include "hml_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * hml_crc16  : CCITT-16, poly 0x1021, init 0xFFFF, no final xor.
 *              Used over frame datagram payload (and control message).
 *              Check vector: hml_crc16("123456789") == 0x29B1.
 * hml_crc32  : IEEE 802.3 CRC-32 (RoCEv2 ICRC uses this polynomial; coverage
 *              is per RoCEv2 spec = everything except IP/UDP headers and the
 *              ICRC field itself). Check vector: hml_crc32("123456789") == 0xCBF43926.
 */
HML_API uint16_t hml_crc16(const void *data, size_t n);
HML_API uint32_t hml_crc32_ieee(const void *data, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* HML_CRC_H */