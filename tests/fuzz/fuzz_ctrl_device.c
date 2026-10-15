/*
 * fuzz_ctrl_device -- the control-plane device (src/control/hml_control.c)
 * driven over a real loopback socket.
 *
 * fuzz_ctrl covers the codec; this covers what the device *does* with a decoded
 * request, which no pure parser harness can reach because the dispatcher is
 * file-static. Each input is a control datagram, so this also drives the
 * SET_DEST -> hml_proto_cfg_parse, POKE and READBACK paths with attacker-chosen
 * payloads, and the reply path a host client depends on.
 *
 * The invariant worth having is a liveness one. The device is documented to
 * answer HML_OP_ERROR for an unknown opcode precisely so that a client never
 * hangs, and to drop an undecodable datagram silently. Both halves are asserted
 * against a real socket:
 *
 *   received and decoded -> exactly one well-formed response datagram;
 *   received and dropped -> no response at all (fail closed, no amplification);
 *   response             -> must itself parse through hml_ctrl_parse.
 *
 * Ports: a free UDP port is picked at startup from a private range and a small
 * window of them is rotated per input, so this can run beside the rest of the
 * suite. A port that turns out to be busy, or a datagram the kernel never
 * delivered, is counted as skipped and reported in the summary -- a resource
 * condition, not a defect.
 *
 * Timings: on loopback the request is already queued when hml_control_poll is
 * called and the reply is already queued when it returns, so both budgets are
 * single-digit milliseconds. They only matter when the kernel dropped something.
 */

#include "hml_fuzz.h"

#include "hml/hml_ctrl.h"
#include "hml_control.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PORT_LO 47000u
#define PORT_HI 47999u
#define PORT_SPAN 16u

#define DEV_POLL_MS 5
#define CLI_RCV_US 20000

static uint16_t g_port0;
static unsigned long g_sent, g_dropped, g_replied, g_lost, g_skipped;

static void on_set_dest(void *ctx, const hml_dest_cfg *cfg) {
  (void)ctx;
  (void)cfg;
}
static void on_start(void *ctx) { (void)ctx; }
static void on_stop(void *ctx) { (void)ctx; }
static uint32_t on_poke(void *ctx, uint32_t addr, uint32_t val, bool *ok) {
  (void)ctx;
  *ok = true;
  return addr ^ val;
}
static uint32_t on_read(void *ctx, uint32_t addr, bool *ok) {
  (void)ctx;
  *ok = true;
  return addr + 1u;
}

/* An ephemeral client socket plus the port the kernel gave it. */
static int client_open(uint16_t *port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(HML_IP_LOOPBACK);
  sa.sin_port = 0;
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
    (void)close(fd);
    return -1;
  }
  socklen_t sl = sizeof(sa);
  if (getsockname(fd, (struct sockaddr *)&sa, &sl) != 0) {
    (void)close(fd);
    return -1;
  }
  *port = ntohs(sa.sin_port);
  return fd;
}

static void run(const uint8_t *data, size_t n) {
  uint16_t dev_port =
      (uint16_t)(PORT_LO + (((uint32_t)(g_port0 - PORT_LO) + (g_sent % PORT_SPAN)) % PORT_SPAN));
  if (!hml_fuzz_port_free(dev_port)) {
    g_skipped++;
    return;
  }
  uint16_t cli_port = 0u;
  int cli = client_open(&cli_port);
  if (cli < 0) {
    g_skipped++;
    return;
  }

  hml_control_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.ctrl_port = dev_port;
  cfg.disc_port = 0; /* no beacon: this harness is about the request path */
  cfg.beacon_enable = false;
  cfg.uuid = 0x5eedu;
  cfg.caps = HML_CAP_UDP;
  cfg.fw_ver = 0x00010000u;
  cfg.reply_ip = HML_IP_LOOPBACK;
  cfg.reply_port = cli_port;

  hml_control_t *dev = NULL;
  hml_status st = hml_control_open(&dev, &cfg);
  if (st != HML_OK || dev == NULL) {
    (void)close(cli);
    g_skipped++;
    return;
  }
  hml_control_set_cbs(dev, on_set_dest, on_start, on_stop, on_poke, NULL);
  hml_control_set_read_cb(dev, on_read, NULL);

  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(HML_IP_LOOPBACK);
  to.sin_port = htons(dev_port);
  if (sendto(cli, data, n, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
    hml_control_close(dev);
    (void)close(cli);
    g_skipped++;
    return;
  }
  g_sent++;

  (void)hml_control_poll(dev, DEV_POLL_MS);

  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = CLI_RCV_US;
  (void)setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  uint8_t reply[HML_FUZZ_CTRL_MAX_MSG + 64u];
  ssize_t rn = recv(cli, reply, sizeof(reply), 0);

  uint32_t rx = hml_control_rx_count(dev);
  uint32_t bad = hml_control_bad_count(dev);
  if (rx == 0u) {
    g_lost++; /* the kernel never delivered it; nothing to assert */
  } else if (bad != 0u) {
    g_dropped++;
    if (rn >= 0) {
      hml_fuzz_finding("a datagram the device rejected still produced a %zd byte response",
                       rn);
    }
  } else {
    g_replied++;
    if (rn < 0) {
      hml_fuzz_finding("the device decoded a request but sent no response: a client "
                       "would block until its timeout");
    }
    hml_ctrl_msg rm;
    const uint8_t *rp = NULL;
    if (hml_ctrl_parse(reply, (size_t)rn, &rm, &rp) != HML_OK) {
      hml_fuzz_finding("the device's %zd byte response does not parse as a control record",
                       rn);
    }
    if ((rm.flags & (uint16_t)HML_CTRL_FLAG_RESP) == 0u) {
      hml_fuzz_finding("response flags 0x%04x do not carry HML_CTRL_FLAG_RESP",
                       (unsigned)rm.flags);
    }
    if (rm.payload_len > (uint16_t)HML_CTRL_MAX_PAYLOAD) {
      hml_fuzz_finding("response payload_len %u above the maximum", (unsigned)rm.payload_len);
    }
  }

  hml_control_close(dev);
  (void)close(cli);
}

static void setup(void) {
  uint16_t seed = (uint16_t)(20000u + ((unsigned)getpid() % 20000u));
  g_port0 = hml_fuzz_port_pick(seed, PORT_LO, PORT_HI, PORT_SPAN);
  if (g_port0 == 0u) {
    fprintf(stderr, "fuzz_ctrl_device: no free UDP port in %u..%u\n", PORT_LO, PORT_HI);
    exit(HML_FUZZ_EXIT_ERROR);
  }
}

static void summary(void) {
  printf("fuzz_ctrl_device: sent=%lu dropped=%lu replied=%lu undelivered=%lu skipped=%lu\n",
         g_sent, g_dropped, g_replied, g_lost, g_skipped);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  run(data, size);
  return 0;
}

#ifndef HML_FUZZ_LIBFUZZER
int main(int argc, char **argv) {
  setup();
  int rc = hml_fuzz_main(argc, argv, "fuzz_ctrl_device", run, NULL);
  summary();
  return rc;
}
#endif
