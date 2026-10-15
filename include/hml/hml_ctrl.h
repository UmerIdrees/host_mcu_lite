#ifndef HML_CTRL_H
#define HML_CTRL_H

#include "hml_protocol.h"

/*
 * Control plane: own req/resp UDP (Path A). Opcode-based messages; serialized as
 * [magic u32 BE][version u8][op u8][flags u16 BE][len u16 BE][payload ...][crc16 BE]
 * Payload includes hml_dest_cfg when applicable.
 */

#define HML_MAGIC_CTRL 0x484d4c43u /* 'HMLC' */
#define HML_CTRL_VERSION 1u
#define HML_CTRL_MAX_PAYLOAD 512u

#define HML_OP_NOP        0x00
#define HML_OP_PING       0x01
#define HML_OP_SET_DEST   0x02
#define HML_OP_GET_DEST   0x03
#define HML_OP_START      0x04
#define HML_OP_STOP       0x05
#define HML_OP_RESET      0x06
#define HML_OP_GET_CAPS   0x07
#define HML_OP_STATUS     0x08
#define HML_OP_POKE       0x09  /* payload: addr u32 BE, val u32 BE */
#define HML_OP_READBACK   0x0A  /* payload: addr u32 BE; resp payload: addr u32 BE, val u32 BE */
#define HML_OP_ERROR      0xFF

#define HML_CTRL_FLAG_REQ 0x0001
#define HML_CTRL_FLAG_RESP 0x0002
#define HML_CTRL_FLAG_ACK  0x0004
#define HML_CTRL_FLAG_ERR  0x0008

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hml_ctrl_msg {
  uint8_t  op;
  uint16_t flags;
  const uint8_t *payload; /* points into a backing buffer if parsed */
  uint16_t payload_len;
} hml_ctrl_msg;

HML_API void hml_ctrl_fill(const hml_ctrl_msg *m, uint8_t *dst, uint16_t max_dst, uint16_t *out_n);
HML_API hml_status hml_ctrl_parse(const uint8_t *buf, size_t len, hml_ctrl_msg *out,
                                  const uint8_t **payload);

#ifdef __cplusplus
}
#endif

#endif /* HML_CTRL_H */