/*
 * Track B: Server RDMA Verbs Receiver & Socket Control Client Demo / Test.
 *
 * Exercises:
 *   - Verbs receiver lifecycle: config, open, QP state machine, MR registration,
 *     pre-posted receive buffers, RDMA Write with Immediate data delivery,
 *     CQ polling, buffer verification, and teardown.
 *   - Control plane integration: configuring RoCEv2 verbs destination parameters
 *     (kind=HML_XPORT_ROCEV2, qp, rkey, remote_addr, frame_size, udp_port=4791) via
 *     hml_client_set_dest / hml_client_set_dest_roce, verifying ACK, and issuing
 *     hml_client_start and hml_client_stop.
 */

#include "hml_verbs_rx.h"
#include "src/control/hml_client.h"
#include "src/control/hml_control.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void test_sleep_ms(unsigned ms) {
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000u);
  ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
  (void)nanosleep(&ts, NULL);
}

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                  \
  do {                                                                    \
    g_checks++;                                                           \
    if (!(cond)) {                                                        \
      g_failures++;                                                       \
      fprintf(stderr, "FAIL [%s:%d]: ", __FILE__, __LINE__);               \
      fprintf(stderr, __VA_ARGS__);                                       \
      fprintf(stderr, "\n");                                              \
    }                                                                     \
  } while (0)

#define CHECK_OK(expr) CHECK((expr) == HML_OK, "%s -> %d", #expr, (int)(expr))

/* ---- Phase 1: Verbs Receiver Lifecycle & Emulation ---- */
static void test_verbs_rx_lifecycle(void) {
  printf("--- [Phase 1] Verbs Receiver Lifecycle & Immediate Data ---\n");

  hml_verbs_cfg cfg;
  hml_verbs_cfg_default(&cfg);
  CHECK(cfg.ib_port == 1, "default ib_port is 1");
  CHECK(cfg.buffer_size == 65536, "default buffer_size is 64KB");
  CHECK(cfg.auto_repost_recv == true, "default auto_repost_recv is true");
  CHECK(cfg.qp_type == HML_VERBS_QP_RC, "default qp_type is RC");

  cfg.force_emulation = true;
  cfg.buffer_size = 131072; /* 128 KiB */

  hml_verbs_rx_t *rx = NULL;
  CHECK_OK(hml_verbs_rx_open(&rx, &cfg));
  CHECK(rx != NULL, "rx instance created");

  hml_verbs_info info;
  CHECK_OK(hml_verbs_rx_get_info(rx, &info));
  CHECK(info.is_emulated == true, "info reports emulation mode");
  CHECK(info.qp_type == HML_VERBS_QP_RC, "info reports RC QP type");
  CHECK(info.qp_num != 0, "qp_num assigned (0x%08x)", info.qp_num);
  CHECK(info.rkey != 0, "rkey assigned (0x%08x)", info.rkey);
  CHECK(info.remote_addr != 0, "remote_addr assigned (0x%llx)", (unsigned long long)info.remote_addr);
  CHECK(info.buffer_size == 131072, "buffer size matches");

  /* Connect QP to remote peer */
  uint8_t rem_gid[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4};
  CHECK_OK(hml_verbs_rx_connect(rx, 0x20002, rem_gid, 0x1000));
  CHECK_OK(hml_verbs_rx_get_info(rx, &info));
  CHECK(info.qp_state == HML_VERBS_QPS_RTS, "QP transitioned to RTS");

  /* Export destination configuration */
  hml_dest_cfg dest;
  CHECK_OK(hml_verbs_rx_export_dest(rx, 0x7F000001, 9000, &dest));
  CHECK(dest.kind == HML_XPORT_ROCEV2, "kind is HML_XPORT_ROCEV2");
  CHECK(dest.udp_port == 4791, "udp_port is 4791 (RoCEv2)");
  CHECK(dest.qp == info.qp_num, "dest.qp matches local QP");
  CHECK(dest.rkey == info.rkey, "dest.rkey matches MR rkey");
  CHECK(dest.remote_addr == info.remote_addr, "dest.remote_addr matches VA");
  CHECK(dest.frame_size == 9000, "dest.frame_size is 9000");

  /* Buffer access */
  uint8_t *buf = hml_verbs_rx_get_buffer(rx);
  CHECK(buf != NULL, "buffer pointer valid");
  CHECK(hml_verbs_rx_get_buffer_size(rx) == 131072, "buffer size accessor valid");

  /* Poll empty CQ */
  hml_verbs_wc wc;
  hml_status st = hml_verbs_poll_one(rx, &wc);
  CHECK(st == HML_ERR_AGAIN, "polling empty CQ returns HML_ERR_AGAIN");

  /* Simulate incoming RDMA WRITE with Immediate Data */
  const char test_payload[] = "Holoscan_RoCEv2_Verbs_Frame_001";
  size_t payload_len = strlen(test_payload);
  uint32_t imm_val = 0x55AA1234;
  CHECK_OK(hml_verbs_simulate_write_imm(rx, 0x100, test_payload, payload_len, imm_val));

  /* Poll CQ for completion */
  int n = hml_verbs_poll_cq(rx, 1, &wc);
  CHECK(n == 1, "poll_cq returned 1 completion");
  CHECK(wc.imm_data == imm_val, "immediate data matches 0x%08x (got 0x%08x)", imm_val, wc.imm_data);
  CHECK(wc.byte_len == (uint32_t)payload_len, "byte_len matches %u", (unsigned)payload_len);
  CHECK(wc.status == HML_VERBS_WC_SUCCESS, "wc.status is SUCCESS");
  CHECK(wc.opcode == HML_VERBS_WC_RECV_RDMA_WITH_IMM, "wc.opcode is RECV_RDMA_WITH_IMM");

  /* Verify payload was written into registered host buffer */
  CHECK(memcmp(buf + 0x100, test_payload, payload_len) == 0,
        "buffer contains correct written payload");

  /* Batch write and poll */
  for (int i = 0; i < 5; i++) {
    uint32_t imm = 0x1000 + (uint32_t)i;
    CHECK_OK(hml_verbs_simulate_write_imm(rx, (uint64_t)(0x1000 + i * 64), "BATCH", 5, imm));
  }
  hml_verbs_wc batch_wcs[8];
  n = hml_verbs_poll_cq(rx, 8, batch_wcs);
  CHECK(n == 5, "batch poll returned 5 completions");
  for (int i = 0; i < n; i++) {
    CHECK(batch_wcs[i].imm_data == (0x1000u + (uint32_t)i), "batch imm_data #%d matches", i);
  }

  hml_verbs_rx_close(rx);
  printf("  [PASS] Verbs receiver lifecycle complete.\n");
}

/* ---- Phase 2: Reliable Connected (RC) and Unreliable Connected (UC) Modes ---- */
static void test_verbs_rx_qp_types(void) {
  printf("--- [Phase 2] RC and UC Queue Pair Profiles ---\n");

  /* Test 1: Explicit UC QP */
  {
    hml_verbs_cfg cfg;
    hml_verbs_cfg_default(&cfg);
    cfg.qp_type = HML_VERBS_QP_UC;
    cfg.force_emulation = true;

    hml_verbs_rx_t *rx = NULL;
    CHECK_OK(hml_verbs_rx_open(&rx, &cfg));
    CHECK(rx != NULL, "UC rx instance created");

    hml_verbs_info info;
    CHECK_OK(hml_verbs_rx_get_info(rx, &info));
    CHECK(info.qp_type == HML_VERBS_QP_UC, "info.qp_type is UC");

    uint8_t rem_gid[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4};
    CHECK_OK(hml_verbs_rx_connect(rx, 0x10001, rem_gid, 0x2000));
    CHECK_OK(hml_verbs_rx_get_info(rx, &info));
    CHECK(info.qp_state == HML_VERBS_QPS_RTS, "UC QP in RTS state");

    CHECK_OK(hml_verbs_simulate_write_imm(rx, 0, "UC_DATA", 7, 0x1111));
    hml_verbs_wc wc;
    int n = hml_verbs_poll_cq(rx, 1, &wc);
    CHECK(n == 1, "UC CQ poll returned 1 completion");
    CHECK(wc.imm_data == 0x1111, "UC immediate data matches");
    CHECK(memcmp(hml_verbs_rx_get_buffer(rx), "UC_DATA", 7) == 0, "UC buffer matches");

    hml_verbs_rx_close(rx);
  }

  /* Test 2: Explicit RC QP */
  {
    hml_verbs_cfg cfg;
    hml_verbs_cfg_default(&cfg);
    cfg.qp_type = HML_VERBS_QP_RC;
    cfg.force_emulation = true;

    hml_verbs_rx_t *rx = NULL;
    CHECK_OK(hml_verbs_rx_open(&rx, &cfg));
    CHECK(rx != NULL, "RC rx instance created");

    hml_verbs_info info;
    CHECK_OK(hml_verbs_rx_get_info(rx, &info));
    CHECK(info.qp_type == HML_VERBS_QP_RC, "info.qp_type is RC");

    uint8_t rem_gid[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 6, 7, 8};
    CHECK_OK(hml_verbs_rx_connect(rx, 0x10002, rem_gid, 0x3000));
    CHECK_OK(hml_verbs_rx_get_info(rx, &info));
    CHECK(info.qp_state == HML_VERBS_QPS_RTS, "RC QP in RTS state");

    CHECK_OK(hml_verbs_simulate_write_imm(rx, 64, "RC_DATA", 7, 0x2222));
    hml_verbs_wc wc;
    int n = hml_verbs_poll_cq(rx, 1, &wc);
    CHECK(n == 1, "RC CQ poll returned 1 completion");
    CHECK(wc.imm_data == 0x2222, "RC immediate data matches");
    CHECK(memcmp(hml_verbs_rx_get_buffer(rx) + 64, "RC_DATA", 7) == 0, "RC buffer matches");

    hml_verbs_rx_close(rx);
  }

  printf("  [PASS] UC and RC Queue Pair profiles verified.\n");
}

/* ---- Phase 3: Probe / Fallback Mode ---- */
static void test_verbs_rx_probe(void) {
  printf("--- [Phase 3] Verbs Receiver Probe / Auto-Fallback ---\n");

  hml_verbs_cfg cfg;
  hml_verbs_cfg_default(&cfg);
  cfg.force_emulation = false;
  cfg.allow_emulation = true; /* auto-fallback if no RDMA hardware */

  hml_verbs_rx_t *rx = NULL;
  hml_status st = hml_verbs_rx_open(&rx, &cfg);
  CHECK_OK(st);
  if (rx) {
    hml_verbs_info info;
    CHECK_OK(hml_verbs_rx_get_info(rx, &info));
    printf("  Receiver opened device '%s' (mode: %s, QP: 0x%08x, rkey: 0x%08x)\n",
           info.dev_name, info.is_emulated ? "emulated" : "native-ibverbs",
           info.qp_num, info.rkey);
    hml_verbs_rx_close(rx);
  }
  printf("  [PASS] Probe / auto-fallback verified.\n");
}

/* ---- Phase 4: Control Client Negotiation (Task B.3) ---- */
typedef struct {
  hml_control_t *ctl;
  uint16_t port;
  volatile int run;
} server_thread_ctx_t;

static void *server_thread_fn(void *arg) {
  server_thread_ctx_t *s = (server_thread_ctx_t *)arg;
  while (s->run) {
    hml_control_poll(s->ctl, 10);
  }
  return NULL;
}

static void test_control_client_verbs_integration(void) {
  printf("--- [Phase 4] Control Client Verbs Destination & ACK Integration ---\n");

  uint16_t ctrl_port = 55192;
  uint16_t client_port = 45592;

  /* Initialize control server */
  hml_control_cfg srv_cfg;
  memset(&srv_cfg, 0, sizeof(srv_cfg));
  srv_cfg.ctrl_port = ctrl_port;
  srv_cfg.disc_port = 0; /* no beacon needed for direct test */
  srv_cfg.reply_ip  = HML_IP_LOOPBACK;
  srv_cfg.reply_port = client_port;
  srv_cfg.uuid      = 0x42;
  srv_cfg.caps      = 0x21;
  srv_cfg.fw_ver    = 0x010000;

  hml_control_t *ctl = NULL;
  CHECK_OK(hml_control_open(&ctl, &srv_cfg));

  server_thread_ctx_t sctx;
  sctx.ctl = ctl;
  sctx.port = ctrl_port;
  sctx.run = 1;

  pthread_t tid;
  pthread_create(&tid, NULL, server_thread_fn, &sctx);
  test_sleep_ms(20); /* 20 ms */

  /* Open host client */
  hml_client_t *cli = NULL;
  CHECK_OK(hml_client_open(&cli, client_port));
  CHECK_OK(hml_client_set_peer(cli, HML_IP_LOOPBACK, ctrl_port, 0));
  CHECK_OK(hml_client_set_timeout(cli, 1000));

  /* Open verbs receiver to get destination parameters */
  hml_verbs_cfg vcfg;
  hml_verbs_cfg_default(&vcfg);
  vcfg.force_emulation = true;
  hml_verbs_rx_t *rx = NULL;
  CHECK_OK(hml_verbs_rx_open(&rx, &vcfg));

  hml_dest_cfg vdest;
  CHECK_OK(hml_verbs_rx_export_dest(rx, HML_IP_LOOPBACK, 8192, &vdest));
  CHECK(vdest.kind == HML_XPORT_ROCEV2, "kind is ROCEV2");
  CHECK(vdest.udp_port == 4791, "udp_port is 4791");

  /* 1. Send HML_OP_SET_DEST with verbs parameters and verify server ACK */
  CHECK_OK(hml_client_set_dest(cli, &vdest));

  /* 2. Read back destination from device and verify all fields */
  hml_dest_cfg readback;
  CHECK_OK(hml_client_get_dest(cli, &readback));
  CHECK(readback.kind == HML_XPORT_ROCEV2, "readback kind is ROCEV2");
  CHECK(readback.qp == vdest.qp, "readback QP matches");
  CHECK(readback.rkey == vdest.rkey, "readback rkey matches");
  CHECK(readback.remote_addr == vdest.remote_addr, "readback remote_addr matches");
  CHECK(readback.frame_size == 8192, "readback frame_size matches");
  CHECK(readback.udp_port == 4791, "readback udp_port matches");

  /* 3. Issue START, verify ACK, check streaming status */
  CHECK_OK(hml_client_start(cli));
  bool streaming = false;
  CHECK_OK(hml_client_status(cli, &streaming));
  CHECK(streaming == true, "status reports streaming after START");

  /* 4. Issue STOP, verify ACK, check streaming status */
  CHECK_OK(hml_client_stop(cli));
  CHECK_OK(hml_client_status(cli, &streaming));
  CHECK(streaming == false, "status reports stopped after STOP");

  /* 5. Test convenience helper hml_client_set_dest_roce */
  CHECK_OK(hml_client_set_dest_roce(cli, HML_IP_LOOPBACK, 4791, 0x3344, 0xFEEDFACE,
                                    0x10002000ull, 16384, 8, 0x4000));
  CHECK_OK(hml_client_get_dest(cli, &readback));
  CHECK(readback.kind == HML_XPORT_ROCEV2, "readback kind is ROCEV2");
  CHECK(readback.qp == 0x3344, "readback qp is 0x3344");
  CHECK(readback.rkey == 0xFEEDFACE, "readback rkey is 0xFEEDFACE");
  CHECK(readback.remote_addr == 0x10002000ull, "readback remote_addr is 0x10002000");
  CHECK(readback.frame_size == 16384, "readback frame_size is 16384");

  /* Teardown */
  hml_verbs_rx_close(rx);
  hml_client_close(cli);

  sctx.run = 0;
  pthread_join(tid, NULL);
  hml_control_close(ctl);

  printf("  [PASS] Control client verbs negotiation verified.\n");
}

int main(void) {
  printf("====================================================\n");
  printf("  TRACK B: Holoscan Server Verbs Receiver Engine\n");
  printf("====================================================\n");

  test_verbs_rx_lifecycle();
  test_verbs_rx_qp_types();
  test_verbs_rx_probe();
  test_control_client_verbs_integration();

  printf("----------------------------------------------------\n");
  if (g_failures == 0) {
    printf("ALL CHECKS PASSED: %d checks\n", g_checks);
    return 0;
  } else {
    printf("TEST FAILED: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
  }
}
