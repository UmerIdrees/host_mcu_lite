#ifndef HML_PROTOCOL_H
#define HML_PROTOCOL_H

#include "hml_port.h"

/*
 * Wire protocol, v1. All multi-byte fields are big-endian.
 *
 * Data datagram (T0 UDP and, with different outer header, the basis of T1a RoCEv2):
 *   [HML_FRAME_HDR_SIZE bytes header][payload HML_MAX_FRAG_PAYLOAD max][uint16 be crc over payload]
 * Header:
 *   0  u32 magic HML_MAGIC_FRAME
 *   4  u8  version HML_PROTO_VERSION
 *   5  u8  flags (HML_FLAG_START | HML_FLAG_END)
 *   6  u16 stream_id
 *   8  u32 seq
 *   12 u16 frag_idx (0-based)
 *   14 u16 frag_cnt
 *   16 u64 ts_us (source monotonic/us since boot)
 *   24 u16 payload_len
 *   26 u16 hdr_crc16 (CRC-CCITT over bytes [0,26), i.e. up to and excluding the crc field)
 *   28 payload[...]
 *   end u16 payload_crc16 (CRC-CCITT over payload bytes only)
 *
 * End-of-frame: the final fragment of a frame carries HML_FLAG_END and the full
 * frame assembles as frag_idx == 0..frag_cnt-1. An "EOF-only flush" is a fragment
 * with payload_len == 0, HML_FLAG_END set; it has no fragment contributions.
 */

#define HML_PROTO_VERSION 1u

#define HML_MAGIC_FRAME  0x484d4c31u /* "HML1" */
#define HML_MAGIC_CTRL   0x484d4c43u /* "HMLC" */
#define HML_MAGIC_BEACON 0x484d4c42u /* "HMLB" */

#define HML_FLAG_START 0x01u
#define HML_FLAG_END   0x02u

#define HML_FRAME_HDR_SIZE      28
#define HML_TRAIL_CRC_SIZE      2
#define HML_MAX_FRAG_PAYLOAD    1428u
#define HML_MAX_FRAG_SIZE       (HML_FRAME_HDR_SIZE + HML_MAX_FRAG_PAYLOAD + HML_TRAIL_CRC_SIZE) /* 1458 */
#define HML_MAX_FRAME_SIZE      (1u << 20)          /* 1 MiB */
#define HML_MAX_FRAGS           (HML_MAX_FRAME_SIZE / HML_MAX_FRAG_PAYLOAD + 1)

/* Default UDP ports (data / control / discovery beacon). */
#define HML_DATA_PORT 55101u
#define HML_CTRL_PORT 55102u
#define HML_DISC_PORT 55100u
#define HML_ROCE_PORT 4791u  /* IANA RoCEv2 UDP port */

/* Header field offsets. */
enum {
  HML_OFF_MAGIC = 0, HML_OFF_VERSION = 4, HML_OFF_FLAGS = 5, HML_OFF_STREAM = 6,
  HML_OFF_SEQ = 8, HML_OFF_FRAG_IDX = 12, HML_OFF_FRAG_CNT = 14, HML_OFF_TS = 16,
  HML_OFF_PAYLOAD_LEN = 24, HML_OFF_HDR_CRC = 26, HML_OFF_PAYLOAD = 28
};

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hml_frame_hdr {
  uint16_t stream_id;
  uint32_t seq;
  uint16_t frag_idx;
  uint16_t frag_cnt;
  uint64_t ts_us;
  uint8_t  flags;        /* HML_FLAG_* */
  uint16_t payload_len;
} hml_frame_hdr;

/* Serialize a logical frame header into dst (must be >= HML_FRAME_HDR_SIZE). */
HML_API void hml_proto_frame_fill(const hml_frame_hdr *h, uint8_t *dst);

/* Parse+validate a full datagram. Returns hml_frame_hdr plus payload pointer/length.
 * Validates magic, version, header sizes and header crc. payload may be NULL when
 * checking structure only. */
HML_API hml_status hml_proto_frame_parse(const uint8_t *datagram, size_t len,
                                         hml_frame_hdr *out, const uint8_t **payload);

/* Transport kinds (data plane). */
typedef enum hml_xport_kind {
  HML_XPORT_UDP = 0,
  HML_XPORT_ROCEV2 = 1
} hml_xport_kind;

/*
 * Destination / RDMA-window configuration, shared by control plane (on the wire,
 * see hml_proto_cfg_fill/parse), the transports, and the host receiver.
 */
typedef struct hml_dest_cfg {
  hml_xport_kind kind;
  uint32_t ipv4;          /* network-byte-order dest address */
  uint16_t udp_port;      /* data port (UDP T0) or 4791 for RoCEv2 */
  uint16_t pad;
  uint32_t qp;            /* RoCEv2 Queue Pair number */
  uint32_t psn;           /* RoCEv2 initial PSN */
  uint32_t rkey;          /* RoCEv2 remote rkey */
  uint64_t remote_addr;   /* RoCEv2 remote memory window base VA */
  uint32_t frame_size;    /* bytes per EOF-delimited frame (implicit EOF for T1a) */
  uint16_t window_len;    /* remote window length in units of 4KiB */
  uint8_t  hop_limit;
  uint8_t  rsvd[5];
} hml_dest_cfg;

#define HML_CFG_SIZE 40u /* serialized size of hml_dest_cfg (all fields BE) */

HML_API void hml_proto_cfg_fill(const hml_dest_cfg *cfg, uint8_t *dst);   /* dst >= HML_CFG_SIZE */
HML_API hml_status hml_proto_cfg_parse(const uint8_t *src, size_t len, hml_dest_cfg *out);

#ifdef __cplusplus
}
#endif

#endif /* HML_PROTOCOL_H */