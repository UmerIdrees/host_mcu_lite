#ifndef HML_VERBS_RX_H
#define HML_VERBS_RX_H

/*
 * Track B: Server RDMA & Soft-RoCE Reference Engine (Verbs Receiver).
 *
 * Implements the verbs receiver lifecycle:
 *   - Device discovery and opening (ibv_open_device for named device or first available)
 *   - Protection Domain allocation (ibv_alloc_pd)
 *   - Host memory buffer allocation and registration (ibv_reg_mr)
 *     with IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
 *   - Completion Queue creation (ibv_create_cq)
 *   - Unreliable Connected (UC) Queue Pair creation (ibv_create_qp)
 *     and state transition RESET -> INIT -> RTR -> RTS (ibv_modify_qp)
 *   - Pre-post receive buffers (ibv_post_recv) for Immediate Data completions
 *   - Non-blocking work completion detection on incoming Immediate data (hml_verbs_poll_cq)
 *   - Direct export of verbs parameters into hml_dest_cfg for control plane negotiation
 *   - Full emulation fallback when libibverbs / RDMA hardware is unavailable
 */

#include "hml/hml_protocol.h"
#include "hml/hml_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* QP State codes mirroring InfiniBand spec */
enum {
  HML_VERBS_QPS_RESET = 0,
  HML_VERBS_QPS_INIT  = 1,
  HML_VERBS_QPS_RTR   = 2,
  HML_VERBS_QPS_RTS   = 3,
  HML_VERBS_QPS_ERR   = 4
};

/* Queue Pair transport service types */
typedef enum hml_verbs_qp_type {
  HML_VERBS_QP_UC = 0, /* Unreliable Connected: unidirectional sensor stream */
  HML_VERBS_QP_RC = 1  /* Reliable Connected: ACK/NACK, retransmissions, universal standard */
} hml_verbs_qp_type_t;

/* Opcode identifiers in work completion */
enum {
  HML_VERBS_WC_RECV                = 0,
  HML_VERBS_WC_RECV_RDMA_WITH_IMM  = 1
};

/* Work completion status */
enum {
  HML_VERBS_WC_SUCCESS             = 0,
  HML_VERBS_WC_LOC_LEN_ERR         = 1,
  HML_VERBS_WC_REM_ACC_ERR         = 2,
  HML_VERBS_WC_WR_FLUSH_ERR        = 3,
  HML_VERBS_WC_GENERAL_ERR         = 4
};

/*
 * Opaque handle to verbs receiver instance.
 */
typedef struct hml_verbs_rx hml_verbs_rx_t;

/*
 * Receiver configuration parameters.
 */
typedef struct hml_verbs_cfg {
  const char *dev_name;         /* IB device name (e.g. "rxe0"), NULL = first available */
  uint8_t     ib_port;          /* 1-based port index (default: 1) */
  uint8_t     gid_idx;          /* GID table index for RoCEv2 (default: 0) */
  uint8_t     hop_limit;        /* IP TTL / hop limit (default: 64) */
  uint8_t     sl;               /* Service Level (default: 0) */

  size_t      buffer_size;      /* Registered buffer size in bytes (default: 65536) */
  void       *user_buffer;      /* Pre-allocated memory; NULL = hml_verbs_rx allocates */

  uint32_t    max_rx_wr;        /* Max outstanding Receive WRs (default: 128) */
  uint32_t    cq_depth;         /* Completion Queue entries (default: 128) */

  /* Queue Pair & connection parameters */
  hml_verbs_qp_type_t qp_type;  /* HML_VERBS_QP_RC (default) or HML_VERBS_QP_UC */
  uint32_t    local_psn;        /* Local initial PSN (default: 0x1000) */
  uint32_t    remote_qp;        /* Remote QP number (0 if connection delayed) */
  uint32_t    remote_psn;       /* Remote initial PSN (default: 0x1000) */
  uint8_t     remote_gid[16];   /* Remote GID (for RoCEv2 address vector) */

  bool        auto_repost_recv; /* Automatically re-post recv WR when reaped in poll_cq */
  bool        allow_emulation;  /* Fall back to software emulation if verbs/device unavailable */
  bool        force_emulation;  /* Force emulation mode even if libibverbs is available */
} hml_verbs_cfg;

/*
 * Work completion report for polled events.
 */
typedef struct hml_verbs_wc {
  uint64_t wr_id;               /* Work request ID */
  uint32_t imm_data;            /* Immediate data from IBV_WR_RDMA_WRITE_WITH_IMM */
  uint32_t byte_len;            /* Bytes transferred */
  uint32_t qp_num;              /* Queue Pair number */
  int      status;              /* HML_VERBS_WC_SUCCESS (0) on success */
  uint32_t opcode;              /* HML_VERBS_WC_RECV_RDMA_WITH_IMM */
} hml_verbs_wc;

/*
 * Runtime information and exported memory registration parameters.
 */
typedef struct hml_verbs_info {
  char     dev_name[64];        /* Active device name */
  uint32_t qp_num;              /* Local Queue Pair number */
  hml_verbs_qp_type_t qp_type;  /* Active QP type (UC or RC) */
  uint32_t rkey;                /* Memory Region remote key (rkey) */
  uint32_t lkey;                /* Memory Region local key (lkey) */
  uint64_t remote_addr;         /* Base virtual address of registered memory */
  size_t   buffer_size;         /* Size of registered memory window */
  uint8_t  local_gid[16];       /* Local GID */
  uint32_t local_psn;           /* Local PSN */
  bool     is_emulated;         /* True if running in software emulation mode */
  int      qp_state;            /* HML_VERBS_QPS_* */
} hml_verbs_info;

/*
 * Initialize default configuration settings.
 */
HML_API void hml_verbs_cfg_default(hml_verbs_cfg *cfg);

/*
 * Open and initialize the verbs receiver:
 * 1. Discover and open device (ibv_open_device)
 * 2. Allocate Protection Domain (ibv_alloc_pd)
 * 3. Allocate/register memory buffer (ibv_reg_mr) with LOCAL_WRITE | REMOTE_WRITE
 * 4. Create Completion Queue (ibv_create_cq)
 * 5. Create Unreliable Connected (UC) Queue Pair (ibv_create_qp)
 * 6. Transition QP RESET -> INIT (ibv_modify_qp)
 * 7. If remote_qp is set: transition INIT -> RTR -> RTS
 * 8. Pre-post receive WRs (ibv_post_recv) for Immediate Data completions
 */
HML_API hml_status hml_verbs_rx_open(hml_verbs_rx_t **out, const hml_verbs_cfg *cfg);

/*
 * Transition QP from INIT to RTR and RTS with given remote parameters.
 * (Used when remote QP/GID are exchanged dynamically via control protocol).
 */
HML_API hml_status hml_verbs_rx_connect(hml_verbs_rx_t *rx, uint32_t remote_qp,
                                        const uint8_t *remote_gid, uint32_t remote_psn);

/*
 * Non-blocking poll on Completion Queue for incoming Immediate data completions.
 * Returns number of completions retrieved (>= 0), or negative hml_status on error.
 */
HML_API int hml_verbs_poll_cq(hml_verbs_rx_t *rx, int max_entries, hml_verbs_wc *wc_list);

/*
 * Convenience function: poll for a single completion.
 * Returns HML_OK if a completion was reaped, HML_ERR_AGAIN if CQ is empty, or error.
 */
HML_API hml_status hml_verbs_poll_one(hml_verbs_rx_t *rx, hml_verbs_wc *wc_out);

/*
 * Manually post a receive buffer (used if auto_repost_recv is disabled).
 */
HML_API hml_status hml_verbs_post_recv(hml_verbs_rx_t *rx, uint64_t wr_id);

/*
 * Query runtime info, keys, and base address.
 */
HML_API hml_status hml_verbs_rx_get_info(const hml_verbs_rx_t *rx, hml_verbs_info *info);

/*
 * Populate an hml_dest_cfg struct with this receiver's verbs destination parameters
 * (HML_XPORT_ROCEV2, qp, rkey, remote_addr, window_len, udp_port=4791).
 */
HML_API hml_status hml_verbs_rx_export_dest(const hml_verbs_rx_t *rx, uint32_t ipv4,
                                            uint32_t frame_size, hml_dest_cfg *out_dest);

/*
 * Direct access to the memory window buffer.
 */
HML_API uint8_t *hml_verbs_rx_get_buffer(hml_verbs_rx_t *rx);
HML_API size_t   hml_verbs_rx_get_buffer_size(const hml_verbs_rx_t *rx);

/*
 * Simulation helper: inject a simulated RDMA WRITE with Immediate Data into
 * the registered buffer and CQ (works in emulation mode or loopback testing).
 */
HML_API hml_status hml_verbs_simulate_write_imm(hml_verbs_rx_t *rx, uint64_t offset,
                                                const void *src, size_t len,
                                                uint32_t imm_data);

/*
 * Tear down receiver and release all verbs/allocated resources.
 */
HML_API void hml_verbs_rx_close(hml_verbs_rx_t *rx);

#ifdef __cplusplus
}
#endif

#endif /* HML_VERBS_RX_H */
