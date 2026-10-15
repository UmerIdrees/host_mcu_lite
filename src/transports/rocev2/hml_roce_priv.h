#ifndef HML_ROCE_PRIV_H
#define HML_ROCE_PRIV_H

/*
 * M3 / T1a: socket-crafted RoCEv2 (UC RDMA WRITE) transmitter -- private interface.
 *
 * WIRE LAYOUT OF ONE EMITTED DATAGRAM (all offsets from the first IP byte):
 *
 *   off  size  field
 *   ---  ----  ---------------------------------------------------------------
 *    0    20   IPv4 header          ver/IHL=0x45, DSCP 0, tot_len, id, DF,
 *                                   TTL = dest.hop_limit, proto = 17, csum,
 *                                   src, dst
 *   20     8   UDP header           sport = dport, dport = dest.udp_port
 *                                   (HML_ROCE_PORT 4791 in production),
 *                                   len, checksum = 0
 *   28    12   BTH                  [0]=0x2A, [1]=0x00, [2]=0x00,
 *                                   [3..5]=PSN24 (BE), [6..7]=AETH=0x0000,
 *                                   [8..11]=0
 *   40    16   RETH                 [0..7]=VA (BE u64), [8..11]=rkey (BE u32),
 *                                   [12..15]=length (BE u32, D bit = 0)
 *   56    len  RDMA write payload
 *   56+len 4   ICRC                 hml_crc32_ieee(BTH..end-of-payload),
 *                                   transmitted LITTLE-ENDIAN
 *
 *   total = 60 + len, max len = 1440 (1500 MTU - 60), so nothing fragments.
 *
 * BTH opcode byte: 0x2A = transport-service UC (0b001 << 5) | RDMA (0x0A).
 * See the extended block comment at the top of hml_rocev2.c.
 */

#include "hml/hml_crc.h"
#include "hml/hml_port.h"
#include "hml/hml_protocol.h"
#include "hml/hml_sock.h"
#include "hml/hml_transport.h"

/* ---- sizes ---- */
#define HML_ROCE_IP_HDR_LEN 20u
#define HML_ROCE_UDP_HDR_LEN 8u
#define HML_ROCE_BTH_LEN 12u
#define HML_ROCE_RETH_LEN 16u
#define HML_ROCE_ICRC_LEN 4u
#define HML_ROCE_IC_HDR_LEN (HML_ROCE_IP_HDR_LEN + HML_ROCE_UDP_HDR_LEN) /* 28 */
#define HML_ROCE_BTH_RETH_LEN (HML_ROCE_BTH_LEN + HML_ROCE_RETH_LEN)     /* 28 */
#define HML_ROCE_WIRE_OVERHEAD \
  (HML_ROCE_IC_HDR_LEN + HML_ROCE_BTH_RETH_LEN + HML_ROCE_ICRC_LEN)      /* 60 */
#define HML_ROCE_MAX_PAYLOAD 1440u /* 1500 MTU - 60 bytes of headers */
#define HML_ROCE_DG_MAX (HML_ROCE_WIRE_OVERHEAD + HML_ROCE_MAX_PAYLOAD)   /* 1500 */

/* ---- byte offsets (absolute, and relative to the BTH) ---- */
#define HML_ROCE_OFF_IP 0u
#define HML_ROCE_OFF_UDP 20u
#define HML_ROCE_OFF_BTH 28u
#define HML_ROCE_OFF_RETH 40u
#define HML_ROCE_OFF_PAYLOAD 56u

#define HML_ROCE_BTH_OPCODE 0u
#define HML_ROCE_BTH_FLAGS 1u
#define HML_ROCE_BTH_RSVD 2u
#define HML_ROCE_BTH_PSN 3u  /* 3 bytes, big-endian */
#define HML_ROCE_BTH_AETH 6u /* 2 bytes */

#define HML_ROCE_OPCODE_UC_RDMA_WRITE 0x2Au
#define HML_ROCE_OPCODE_UC_RDMA_WRITE_WITH_IMM 0x2Bu
#define HML_ROCE_OPCODE_RC_RDMA_WRITE 0x0Au
#define HML_ROCE_OPCODE_RC_RDMA_WRITE_WITH_IMM 0x0Bu
#define HML_ROCE_IMM_LEN 4u
#define HML_ROCE_OFF_IMM 56u
#define HML_ROCE_OFF_IMM_PAYLOAD 60u
#define HML_ROCE_WIRE_OVERHEAD_IMM (HML_ROCE_WIRE_OVERHEAD + HML_ROCE_IMM_LEN) /* 64 */

/* Unprivileged mirror of HML_ROCE_PORT (4791 is < 1024 => root / CAP_NET_BIND_SERVICE). */
#define HML_ROCE_TEST_PORT 14791u

#define HML_ROCE_DEFAULT_TTL 64u
#define HML_ROCE_LOOPBACK_IP 0x7F000001u

/*
 * EOF metadata structure, written to the dedicated meta slot that sits
 * immediately past the frame data window:
 *
 *   VA = dest.remote_addr + dest.frame_size
 *
 * Serialized LITTLE-endian (it is host-local RDMA memory, not a wire struct).
 * Field list is 4+2+2+4+4+4 = 20 bytes; the "16-byte" figure in the M3 notes was
 * a miscount, the explicit field list is authoritative.
 */
#define HML_ROCE_META_LEN 20u
#define HML_ROCE_OFF_META_MAGIC 0u
#define HML_ROCE_OFF_META_STREAM 4u
#define HML_ROCE_OFF_META_FLAGS 6u
#define HML_ROCE_OFF_META_SEQ 8u
#define HML_ROCE_OFF_META_BYTES 12u
#define HML_ROCE_OFF_META_CRC 16u

typedef struct hml_roce_ctx {
  hml_sock_t *sock;
  hml_dest_cfg dest;
  uint16_t port;         /* resolved UDP dest port */
  uint32_t psn;          /* 24-bit, current packet sequence number */
  uint16_t ip_id;        /* IPv4 id field, increments per emitted datagram */
  uint32_t seq_in_frame; /* fragment index within the current frame (diagnostic) */
  size_t frame_bytes;    /* payload bytes of the frame currently being streamed */
  uint64_t remote_cursor;/* offset inside the current frame window */
  uint32_t crc_state;    /* running ICRC-style CRC of the current frame payload */
  uint32_t frame_seq;    /* seq of the frame currently being streamed */
  uint16_t stream_id;
  uint8_t frame_flags;   /* flags of the last fragment of the current frame */
  unsigned dg_index;     /* datagrams emitted, for the fault hook */
  bool end_seen;         /* HML_FLAG_END observed on a data fragment */
  bool meta_sent;        /* EOF metadata already emitted for this frame */
  uint8_t qp_type;       /* 0 = UC (default), 1 = RC */
  bool use_imm;          /* true = emit RDMA Write with Immediate on EOF fragment */
  uint32_t imm_val;      /* immediate value override (0 = use seq/magic) */
  uint8_t buf[HML_MAX_FRAG_SIZE + 64];
} hml_roce_ctx;

/* Everything the datagram builder needs; keeps the golden-vector tests honest. */
typedef struct hml_roce_wire_cfg {
  uint32_t src_ip;  /* network-byte-order IPv4 (same convention as hml_dest_cfg) */
  uint32_t dst_ip;
  uint16_t udp_port;
  uint8_t ttl;
  uint16_t ip_id;
  uint32_t psn;     /* 24-bit, low 24 bits are used */
  uint64_t va;      /* RETH virtual address */
  uint32_t rkey;    /* RETH remote key */
  uint32_t imm_data;/* Immediate data (host byte order, transmitted BE) */
  bool     has_imm; /* true when emitting RDMA_WRITE_WITH_IMM (opcode 0x2B or 0x0B) */
  uint8_t  qp_type; /* 0 = UC, 1 = RC */
} hml_roce_wire_cfg;

/*
 * Serialize one RoCEv2 UC RDMA WRITE datagram (IP|UDP|BTH|RETH|payload|ICRC).
 * Returns the number of bytes written, or 0 if len > 1440 / cap too small.
 */
size_t hml_roce_build_datagram(uint8_t *out, size_t cap, const hml_roce_wire_cfg *w,
                               const uint8_t *payload, size_t len);

/*
 * Streaming twin of hml_crc32_ieee(): identical algorithm (reflected 0xEDB88320,
 * init/final inversion) so that chunked updates equal the one-shot result.
 * state starts at HML_ROCE_CRC32_INIT and is finalized with hml_roce_crc32_done.
 */
#define HML_ROCE_CRC32_INIT 0xFFFFFFFFu
uint32_t hml_roce_crc32_update(uint32_t state, const void *data, size_t n);
static inline uint32_t hml_roce_crc32_done(uint32_t state) { return ~state; }

/*
 * Fault-injection hook (M3 fault matrix: loss / corrupt / re-address).
 * Invoked after the datagram is built and before it is handed to the socket, so
 * the hook may drop it (return true) or rewrite dg/len in place. Process-global
 * and not per-transport; install it around a test run only.
 */
typedef bool (*hml_roce_fault_fn)(void *ctx, unsigned dg_index, uint8_t *dg, size_t *len);
void hml_roce_set_fault_hook(hml_roce_fault_fn fn, void *ctx);

/* Transport entry points. */
const hml_xport_ops *hml_rocev2_ops(void);
hml_status hml_rocev2_register(void); /* hml_transport_register(hml_rocev2_ops()) */

/*
 * ops->open with an explicit UDP source port (0 = ephemeral, which is the only
 * thing ops->open ever does). The frozen hml_dest_cfg has no field for it, so a
 * sender that needs a pinned source port -- or a caller that wants to reproduce
 * a source-bind failure (busy or privileged port) without touching the config --
 * uses this. Every other behaviour is byte-identical to ops->open.
 */
hml_status hml_roce_open_ex(hml_xport_t *x, const hml_dest_cfg *dest, uint16_t stream_id,
                            uint16_t src_port);

/* Configuration hooks for Queue Pair transport service (UC vs RC) and Immediate Signaling */
hml_status hml_roce_set_qp_type(hml_xport_t *x, uint8_t qp_type);
hml_status hml_roce_set_imm_mode(hml_xport_t *x, bool enable, uint32_t imm_val);

#endif /* HML_ROCE_PRIV_H */
