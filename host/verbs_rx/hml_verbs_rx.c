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
 *   - Clean compile-time fallback / stubs with software emulation mode
 */

#include "hml_verbs_rx.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HML_HAVE_IBVERBS) && HML_HAVE_IBVERBS
#include <infiniband/verbs.h>
#define VERBS_NATIVE 1
#else
#define VERBS_NATIVE 0
#endif

#define HML_VERBS_MAX_RX_WR 256
#define HML_VERBS_MAX_CQE   256

struct hml_verbs_rx {
  hml_verbs_cfg       cfg;
  hml_verbs_qp_type_t qp_type;
  bool                is_emulated;
  int                 qp_state;

  uint8_t      *buffer;
  size_t        buffer_size;
  bool          buffer_allocated;

  uint32_t      qp_num;
  uint32_t      rkey;
  uint32_t      lkey;
  uint64_t      remote_addr;

  uint8_t       local_gid[16];
  uint32_t      local_psn;

  uint32_t      remote_qp;
  uint32_t      remote_psn;
  uint8_t       remote_gid[16];

  char          dev_name[64];

#if VERBS_NATIVE
  struct ibv_context *ctx;
  struct ibv_pd      *pd;
  struct ibv_mr      *mr;
  struct ibv_cq      *cq;
  struct ibv_qp      *qp;
  struct ibv_mr      *dummy_mr;
  uint8_t             dummy_buf[64];
#endif

  /* Emulated WR pool & Completion Queue */
  uint64_t      rx_wr_ids[HML_VERBS_MAX_RX_WR];
  uint32_t      rx_wr_head;
  uint32_t      rx_wr_tail;
  uint32_t      rx_wr_count;

  hml_verbs_wc  cqe_queue[HML_VERBS_MAX_CQE];
  uint32_t      cqe_head;
  uint32_t      cqe_tail;
  uint32_t      cqe_count;
};

void hml_verbs_cfg_default(hml_verbs_cfg *cfg) {
  if (!cfg) return;
  memset(cfg, 0, sizeof(*cfg));
  cfg->ib_port          = 1;
  cfg->gid_idx          = 0;
  cfg->hop_limit        = 64;
  cfg->sl               = 0;
  cfg->buffer_size      = 65536; /* 64 KiB default */
  cfg->user_buffer      = NULL;
  cfg->max_rx_wr        = 128;
  cfg->cq_depth         = 128;
  cfg->qp_type          = HML_VERBS_QP_RC; /* default: RC */
  cfg->local_psn        = 0x1000;
  cfg->remote_qp        = 0;
  cfg->remote_psn       = 0x1000;
  cfg->auto_repost_recv = true;
  cfg->allow_emulation  = true;  /* Graceful fallback */
  cfg->force_emulation  = false;
}

#if VERBS_NATIVE
static hml_status modify_qp_to_rtr(struct ibv_qp *qp, hml_verbs_qp_type_t qp_type,
                                   uint8_t port, uint8_t gid_idx, uint8_t hop_limit,
                                   uint8_t sl, uint32_t remote_qp, uint32_t remote_psn,
                                   const uint8_t *remote_gid) {
  struct ibv_qp_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state           = IBV_QPS_RTR;
  attr.path_mtu           = IBV_MTU_1024;
  attr.dest_qp_num        = remote_qp;
  attr.rq_psn             = remote_psn;
  attr.ah_attr.is_global  = 1;
  attr.ah_attr.port_num   = port;
  attr.ah_attr.sl         = sl;
  attr.ah_attr.grh.sgid_index = gid_idx;
  attr.ah_attr.grh.hop_limit  = hop_limit ? hop_limit : 64;
  if (remote_gid) {
    memcpy(&attr.ah_attr.grh.dgid, remote_gid, 16);
  }

  int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN;
  if (qp_type == HML_VERBS_QP_RC) {
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer      = 12;
    flags |= IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
  }

  if (ibv_modify_qp(qp, &attr, flags) != 0) {
    return HML_ERR_STATE;
  }
  return HML_OK;
}

static hml_status modify_qp_to_rts(struct ibv_qp *qp, hml_verbs_qp_type_t qp_type,
                                   uint32_t local_psn) {
  struct ibv_qp_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTS;
  attr.sq_psn   = local_psn;

  int flags = IBV_QP_STATE | IBV_QP_SQ_PSN;
  if (qp_type == HML_VERBS_QP_RC) {
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.max_rd_atomic = 1;
    flags |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC;
  }

  if (ibv_modify_qp(qp, &attr, flags) != 0) {
    return HML_ERR_STATE;
  }
  return HML_OK;
}
#endif

hml_status hml_verbs_rx_open(hml_verbs_rx_t **out, const hml_verbs_cfg *cfg) {
  if (!out) return HML_ERR_ARG;
  *out = NULL;

  hml_verbs_rx_t *rx = (hml_verbs_rx_t *)calloc(1, sizeof(*rx));
  if (!rx) return HML_ERR_NOMEM;

  if (cfg) {
    rx->cfg = *cfg;
  } else {
    hml_verbs_cfg_default(&rx->cfg);
  }

  rx->buffer_size = rx->cfg.buffer_size ? rx->cfg.buffer_size : 65536;
  if (rx->cfg.user_buffer) {
    rx->buffer = (uint8_t *)rx->cfg.user_buffer;
    rx->buffer_allocated = false;
  } else {
    void *ptr = NULL;
    if (posix_memalign(&ptr, 4096, rx->buffer_size) != 0 || !ptr) {
      ptr = malloc(rx->buffer_size);
    }
    if (!ptr) {
      free(rx);
      return HML_ERR_NOMEM;
    }
    memset(ptr, 0, rx->buffer_size);
    rx->buffer = (uint8_t *)ptr;
    rx->buffer_allocated = true;
  }

  rx->remote_addr = (uint64_t)(uintptr_t)rx->buffer;
  rx->local_psn   = rx->cfg.local_psn ? rx->cfg.local_psn : 0x1000;
  rx->remote_psn  = rx->cfg.remote_psn ? rx->cfg.remote_psn : 0x1000;
  rx->remote_qp   = rx->cfg.remote_qp;
  rx->qp_type     = rx->cfg.qp_type;
  memcpy(rx->remote_gid, rx->cfg.remote_gid, 16);

#if VERBS_NATIVE
  bool try_native = !rx->cfg.force_emulation;
  if (try_native) {
    int num_devs = 0;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devs);
    if (!dev_list || num_devs == 0) {
      if (dev_list) ibv_free_device_list(dev_list);
      try_native = false;
    } else {
      struct ibv_device *chosen = NULL;
      if (rx->cfg.dev_name && rx->cfg.dev_name[0] != '\0') {
        for (int i = 0; i < num_devs; i++) {
          if (strcmp(ibv_get_device_name(dev_list[i]), rx->cfg.dev_name) == 0) {
            chosen = dev_list[i];
            break;
          }
        }
      } else {
        chosen = dev_list[0];
      }

      if (!chosen) {
        ibv_free_device_list(dev_list);
        try_native = false;
      } else {
        snprintf(rx->dev_name, sizeof(rx->dev_name), "%s", ibv_get_device_name(chosen));
        rx->ctx = ibv_open_device(chosen);
        ibv_free_device_list(dev_list);

        if (!rx->ctx) {
          try_native = false;
        } else {
          rx->pd = ibv_alloc_pd(rx->ctx);
          if (!rx->pd) {
            try_native = false;
          } else {
            int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
            if (rx->cfg.qp_type == HML_VERBS_QP_RC) {
              access |= IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
            }
            rx->mr = ibv_reg_mr(rx->pd, rx->buffer, rx->buffer_size, access);
            rx->dummy_mr = ibv_reg_mr(rx->pd, rx->dummy_buf, sizeof(rx->dummy_buf),
                                      IBV_ACCESS_LOCAL_WRITE);
            rx->cq = ibv_create_cq(rx->ctx, (int)rx->cfg.cq_depth, NULL, NULL, 0);

            if (!rx->mr || !rx->dummy_mr || !rx->cq) {
              try_native = false;
            } else {
              struct ibv_qp_init_attr qp_init;
              memset(&qp_init, 0, sizeof(qp_init));
              qp_init.send_cq = rx->cq;
              qp_init.recv_cq = rx->cq;
              qp_init.qp_type = (rx->cfg.qp_type == HML_VERBS_QP_RC) ? IBV_QPT_RC : IBV_QPT_UC;
              qp_init.cap.max_recv_wr  = rx->cfg.max_rx_wr ? rx->cfg.max_rx_wr : 128;
              qp_init.cap.max_send_wr  = 16;
              qp_init.cap.max_recv_sge = 1;
              qp_init.cap.max_send_sge = 1;

              rx->qp = ibv_create_qp(rx->pd, &qp_init);
              if (!rx->qp) {
                try_native = false;
              } else {
                /* RESET -> INIT */
                struct ibv_qp_attr attr;
                memset(&attr, 0, sizeof(attr));
                attr.qp_state        = IBV_QPS_INIT;
                attr.port_num        = rx->cfg.ib_port ? rx->cfg.ib_port : 1;
                attr.pkey_index      = 0;
                attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
                if (rx->cfg.qp_type == HML_VERBS_QP_RC) {
                  attr.qp_access_flags |= IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC;
                }

                int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
                if (ibv_modify_qp(rx->qp, &attr, flags) != 0) {
                  try_native = false;
                } else {
                  rx->qp_state = HML_VERBS_QPS_INIT;
                  rx->qp_num   = rx->qp->qp_num;
                  rx->rkey     = rx->mr->rkey;
                  rx->lkey     = rx->mr->lkey;

                  (void)ibv_query_gid(rx->ctx, attr.port_num, rx->cfg.gid_idx,
                                      (union ibv_gid *)rx->local_gid);

                  /* Pre-post receive WRs to catch incoming Immediate Data */
                  uint32_t to_post = rx->cfg.max_rx_wr ? rx->cfg.max_rx_wr : 64;
                  if (to_post > 64) to_post = 64;
                  for (uint32_t i = 0; i < to_post; i++) {
                    (void)hml_verbs_post_recv(rx, (uint64_t)(i + 1));
                  }

                  /* If remote QP was specified, complete transition to RTS */
                  if (rx->remote_qp != 0) {
                    (void)hml_verbs_rx_connect(rx, rx->remote_qp, rx->remote_gid, rx->remote_psn);
                  }

                  rx->is_emulated = false;
                  *out = rx;
                  return HML_OK;
                }
              }
            }
          }
        }
      }
    }
  }
#endif

  /* Clean up partial native allocations if native failed */
#if VERBS_NATIVE
  if (rx->qp)       { ibv_destroy_qp(rx->qp); rx->qp = NULL; }
  if (rx->cq)       { ibv_destroy_cq(rx->cq); rx->cq = NULL; }
  if (rx->dummy_mr) { ibv_dereg_mr(rx->dummy_mr); rx->dummy_mr = NULL; }
  if (rx->mr)       { ibv_dereg_mr(rx->mr); rx->mr = NULL; }
  if (rx->pd)       { ibv_dealloc_pd(rx->pd); rx->pd = NULL; }
  if (rx->ctx)      { ibv_close_device(rx->ctx); rx->ctx = NULL; }
#endif

  if (!rx->cfg.allow_emulation && !rx->cfg.force_emulation) {
    if (rx->buffer_allocated && rx->buffer) free(rx->buffer);
    free(rx);
    return HML_ERR_NOTSUP;
  }

  /* ---- Software Emulation Mode ---- */
  rx->is_emulated = true;
  snprintf(rx->dev_name, sizeof(rx->dev_name), "%s",
           (rx->cfg.dev_name && rx->cfg.dev_name[0]) ? rx->cfg.dev_name : "soft_rxe_sim");

  rx->qp_num      = 0x10001;
  rx->rkey        = 0x20261013;
  rx->lkey        = 0x20261013;
  rx->qp_state    = HML_VERBS_QPS_INIT;

  /* Generate synthetic local GID: fe80::2026:1013 */
  rx->local_gid[0]  = 0xfe;
  rx->local_gid[1]  = 0x80;
  rx->local_gid[12] = 0x20;
  rx->local_gid[13] = 0x26;
  rx->local_gid[14] = 0x10;
  rx->local_gid[15] = 0x13;

  /* Pre-post initial receive WR slots */
  uint32_t to_post = rx->cfg.max_rx_wr ? rx->cfg.max_rx_wr : 64;
  if (to_post > 64) to_post = 64;
  for (uint32_t i = 0; i < to_post; i++) {
    (void)hml_verbs_post_recv(rx, (uint64_t)(i + 1));
  }

  if (rx->remote_qp != 0) {
    (void)hml_verbs_rx_connect(rx, rx->remote_qp, rx->remote_gid, rx->remote_psn);
  }

  *out = rx;
  return HML_OK;
}

hml_status hml_verbs_rx_connect(hml_verbs_rx_t *rx, uint32_t remote_qp,
                                const uint8_t *remote_gid, uint32_t remote_psn) {
  if (!rx) return HML_ERR_ARG;

  rx->remote_qp  = remote_qp;
  rx->remote_psn = remote_psn;
  if (remote_gid && remote_gid != rx->remote_gid) {
    memcpy(rx->remote_gid, remote_gid, 16);
  }

#if VERBS_NATIVE
  if (!rx->is_emulated && rx->qp) {
    hml_status st = modify_qp_to_rtr(rx->qp, rx->qp_type, rx->cfg.ib_port ? rx->cfg.ib_port : 1,
                                     rx->cfg.gid_idx, rx->cfg.hop_limit, rx->cfg.sl,
                                     remote_qp, remote_psn, rx->remote_gid);
    if (st != HML_OK) return st;

    st = modify_qp_to_rts(rx->qp, rx->qp_type, rx->local_psn);
    if (st != HML_OK) return st;

    rx->qp_state = HML_VERBS_QPS_RTS;
    return HML_OK;
  }
#endif

  rx->qp_state = HML_VERBS_QPS_RTS;
  return HML_OK;
}

hml_status hml_verbs_post_recv(hml_verbs_rx_t *rx, uint64_t wr_id) {
  if (!rx) return HML_ERR_ARG;

#if VERBS_NATIVE
  if (!rx->is_emulated && rx->qp) {
    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr   = (uintptr_t)rx->dummy_buf;
    sge.length = sizeof(rx->dummy_buf);
    sge.lkey   = rx->dummy_mr ? rx->dummy_mr->lkey : 0;

    struct ibv_recv_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id   = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.next    = NULL;

    struct ibv_recv_wr *bad_wr = NULL;
    if (ibv_post_recv(rx->qp, &wr, &bad_wr) != 0) {
      return HML_ERR_IO;
    }
    return HML_OK;
  }
#endif

  /* Emulation queue: store pending receive WR */
  if (rx->rx_wr_count >= HML_VERBS_MAX_RX_WR) {
    return HML_ERR_BUSY;
  }
  rx->rx_wr_ids[rx->rx_wr_tail] = wr_id;
  rx->rx_wr_tail = (rx->rx_wr_tail + 1) % HML_VERBS_MAX_RX_WR;
  rx->rx_wr_count++;
  return HML_OK;
}

int hml_verbs_poll_cq(hml_verbs_rx_t *rx, int max_entries, hml_verbs_wc *wc_list) {
  if (!rx || max_entries <= 0 || !wc_list) return (int)HML_ERR_ARG;

  /* Check simulated CQ queue first (e.g. injected frames during self-test) */
  if (rx->cqe_count > 0) {
    int reaped = 0;
    while (reaped < max_entries && rx->cqe_count > 0) {
      wc_list[reaped] = rx->cqe_queue[rx->cqe_head];
      rx->cqe_head = (rx->cqe_head + 1) % HML_VERBS_MAX_CQE;
      rx->cqe_count--;

      if (rx->cfg.auto_repost_recv && wc_list[reaped].status == HML_VERBS_WC_SUCCESS) {
        (void)hml_verbs_post_recv(rx, wc_list[reaped].wr_id);
      }
      reaped++;
    }
    return reaped;
  }

#if VERBS_NATIVE
  if (!rx->is_emulated && rx->cq) {
    struct ibv_wc ibv_wcs[32];
    int to_poll = max_entries < 32 ? max_entries : 32;
    int n = ibv_poll_cq(rx->cq, to_poll, ibv_wcs);
    if (n < 0) return (int)HML_ERR_IO;

    for (int i = 0; i < n; i++) {
      wc_list[i].wr_id    = ibv_wcs[i].wr_id;
      wc_list[i].byte_len = ibv_wcs[i].byte_len;
      wc_list[i].qp_num   = ibv_wcs[i].qp_num;
      wc_list[i].imm_data = ntohl(ibv_wcs[i].imm_data);
      wc_list[i].status   = (ibv_wcs[i].status == IBV_WC_SUCCESS) ?
                            HML_VERBS_WC_SUCCESS : HML_VERBS_WC_GENERAL_ERR;
      wc_list[i].opcode   = (ibv_wcs[i].opcode == IBV_WC_RECV_RDMA_WITH_IMM) ?
                            HML_VERBS_WC_RECV_RDMA_WITH_IMM : HML_VERBS_WC_RECV;

      if (rx->cfg.auto_repost_recv && ibv_wcs[i].status == IBV_WC_SUCCESS) {
        (void)hml_verbs_post_recv(rx, ibv_wcs[i].wr_id);
      }
    }
    return n;
  }
#endif

  return 0;
}

hml_status hml_verbs_poll_one(hml_verbs_rx_t *rx, hml_verbs_wc *wc_out) {
  if (!rx || !wc_out) return HML_ERR_ARG;
  int n = hml_verbs_poll_cq(rx, 1, wc_out);
  if (n < 0) return (hml_status)n;
  if (n == 0) return HML_ERR_AGAIN;
  return HML_OK;
}

hml_status hml_verbs_rx_get_info(const hml_verbs_rx_t *rx, hml_verbs_info *info) {
  if (!rx || !info) return HML_ERR_ARG;
  memset(info, 0, sizeof(*info));
  snprintf(info->dev_name, sizeof(info->dev_name), "%s", rx->dev_name);
  info->qp_num      = rx->qp_num;
  info->qp_type     = rx->qp_type;
  info->rkey        = rx->rkey;
  info->lkey        = rx->lkey;
  info->remote_addr = rx->remote_addr;
  info->buffer_size = rx->buffer_size;
  info->local_psn   = rx->local_psn;
  info->is_emulated = rx->is_emulated;
  info->qp_state    = rx->qp_state;
  memcpy(info->local_gid, rx->local_gid, 16);
  return HML_OK;
}

hml_status hml_verbs_rx_export_dest(const hml_verbs_rx_t *rx, uint32_t ipv4,
                                    uint32_t frame_size, hml_dest_cfg *out_dest) {
  if (!rx || !out_dest) return HML_ERR_ARG;
  memset(out_dest, 0, sizeof(*out_dest));
  out_dest->kind        = HML_XPORT_ROCEV2;
  out_dest->ipv4        = ipv4;
  out_dest->udp_port    = (uint16_t)HML_ROCE_PORT; /* 4791 */
  out_dest->qp          = rx->qp_num;
  out_dest->rkey        = rx->rkey;
  out_dest->remote_addr = rx->remote_addr;
  out_dest->frame_size  = frame_size;
  out_dest->window_len  = (uint16_t)(rx->buffer_size / 4096u);
  out_dest->psn         = rx->local_psn;
  out_dest->hop_limit   = rx->cfg.hop_limit ? rx->cfg.hop_limit : 64;
  return HML_OK;
}

uint8_t *hml_verbs_rx_get_buffer(hml_verbs_rx_t *rx) {
  return rx ? rx->buffer : NULL;
}

size_t hml_verbs_rx_get_buffer_size(const hml_verbs_rx_t *rx) {
  return rx ? rx->buffer_size : 0;
}

hml_status hml_verbs_simulate_write_imm(hml_verbs_rx_t *rx, uint64_t offset,
                                        const void *src, size_t len,
                                        uint32_t imm_data) {
  if (!rx) return HML_ERR_ARG;
  if (offset + len > rx->buffer_size) return HML_ERR_TOOLARGE;

  if (src && len > 0) {
    memcpy(rx->buffer + offset, src, len);
  }

  uint64_t wr_id = 0;
  if (rx->rx_wr_count > 0) {
    wr_id = rx->rx_wr_ids[rx->rx_wr_head];
    rx->rx_wr_head = (rx->rx_wr_head + 1) % HML_VERBS_MAX_RX_WR;
    rx->rx_wr_count--;
  }

  if (rx->cqe_count >= HML_VERBS_MAX_CQE) {
    return HML_ERR_BUSY;
  }

  hml_verbs_wc *wc = &rx->cqe_queue[rx->cqe_tail];
  wc->wr_id    = wr_id;
  wc->imm_data = imm_data;
  wc->byte_len = (uint32_t)len;
  wc->qp_num   = rx->qp_num;
  wc->status   = HML_VERBS_WC_SUCCESS;
  wc->opcode   = HML_VERBS_WC_RECV_RDMA_WITH_IMM;

  rx->cqe_tail = (rx->cqe_tail + 1) % HML_VERBS_MAX_CQE;
  rx->cqe_count++;
  return HML_OK;
}

void hml_verbs_rx_close(hml_verbs_rx_t *rx) {
  if (!rx) return;

#if VERBS_NATIVE
  if (rx->qp)       { ibv_destroy_qp(rx->qp); rx->qp = NULL; }
  if (rx->cq)       { ibv_destroy_cq(rx->cq); rx->cq = NULL; }
  if (rx->dummy_mr) { ibv_dereg_mr(rx->dummy_mr); rx->dummy_mr = NULL; }
  if (rx->mr)       { ibv_dereg_mr(rx->mr); rx->mr = NULL; }
  if (rx->pd)       { ibv_dealloc_pd(rx->pd); rx->pd = NULL; }
  if (rx->ctx)      { ibv_close_device(rx->ctx); rx->ctx = NULL; }
#endif

  if (rx->buffer_allocated && rx->buffer) {
    free(rx->buffer);
    rx->buffer = NULL;
  }
  free(rx);
}
