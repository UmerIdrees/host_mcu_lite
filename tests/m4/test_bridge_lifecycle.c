/*
 * M4 - test_bridge_lifecycle: the full bridge mode lifecycle over the real
 * UDP control plane.
 *
 * The device is a stub bridge behind a real hml_control server (its own
 * thread); the host side is the real hml_client. Nothing is stubbed between
 * the two: every step below is a datagram on the loopback control port.
 *
 *   discovery beacon, GET_CAPS, PING
 *   GPIO reset poke + I2C register writes over HML_OP_POKE
 *   STATUS not streaming -> SET_DEST (bridge goes to CONFIG, packetizer words
 *   programmed) -> START (STATUS streaming, bridge in STREAM)
 *   20 poke / readback round trips while streaming
 *   STOP (STATUS not streaming, bridge back to CONFIG, pokes survived)
 *   register dump verification of everything the host wrote
 *   injected I2C NAK: the third poke fails, reported as HML_ERR_NOTSUP with
 *   hml_client_last_device_status() == HML_ERR_STATE, and a PING still works
 *
 * Ports default to 45600 + {0 discovery, 1 data, 2 control} and 45699 for the
 * client; "--base N" shifts all of them so the test can run beside another one.
 */

#include "hml_bridge.h"
#include "hml_bridge_control.h"
#include "hml_bridge_stub.h"

#include "hml_client.h"
#include "hml_control.h"

#include "hml_t_m4.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_UUID 0x00000043u
#define TEST_CAPS (HML_CAP_UDP | HML_CAP_BRIDGE | HML_CAP_MCU)
#define TEST_FW 0x00010400u
#define TEST_STREAM_POKES 20u

#define TEST_REG_BASE 0x20u /* first of the 20 poked i2c registers */
#define TEST_REG_ENABLE 0x10u
#define TEST_REG_FRAME 0x11u
#define TEST_SENSOR_ENABLE 0x80000001u
#define TEST_FRAME_SIZE 4096u

/* ---- device: control server on its own thread ----------------------------- */

typedef struct {
  hml_control_t *ctl;
  hml_bridge_t *bridge;
  volatile int run;
  pthread_t th;
} dev_t;

static void *dev_thread(void *arg) {
  dev_t *d = (dev_t *)arg;
  while (d->run) {
    hml_status st = hml_control_poll(d->ctl, 20);
    if (st == HML_ERR_IO) {
      d->run = 0;
      break;
    }
  }
  return NULL;
}

static hml_status dev_up(dev_t *d, uint16_t ctrl_port, uint16_t disc_port, uint16_t client_port) {
  memset(d, 0, sizeof(*d));

  hml_status st = hml_bridge_stub_hml_device(&d->bridge);
  if (st != HML_OK) return st;

  hml_control_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.ctrl_port = ctrl_port;
  cfg.disc_port = disc_port;
  cfg.beacon_enable = true;
  cfg.beacon_period_ms = 50u;
  cfg.uuid = TEST_UUID;
  cfg.caps = TEST_CAPS;
  cfg.fw_ver = TEST_FW;
  cfg.reply_ip = HML_IP_LOOPBACK;
  cfg.reply_port = client_port;

  st = hml_control_open(&d->ctl, &cfg);
  if (st != HML_OK) {
    hml_bridge_close(d->bridge);
    d->bridge = NULL;
    return st;
  }
  /* The whole glue is one call: poke, readback, set_dest, start, stop. */
  st = hml_bridge_control_wire(d->ctl, d->bridge);
  if (st != HML_OK) {
    hml_control_close(d->ctl);
    hml_bridge_close(d->bridge);
    d->ctl = NULL;
    d->bridge = NULL;
    return st;
  }

  d->run = 1;
  if (pthread_create(&d->th, NULL, dev_thread, d) != 0) {
    hml_control_close(d->ctl);
    hml_bridge_close(d->bridge);
    d->ctl = NULL;
    d->bridge = NULL;
    return HML_ERR_UNKNOWN;
  }
  return HML_OK;
}

static void dev_down(dev_t *d) {
  if (d->ctl) {
    d->run = 0;
    (void)pthread_join(d->th, NULL);
    hml_control_close(d->ctl);
  }
  if (d->bridge) hml_bridge_close(d->bridge);
  d->ctl = NULL;
  d->bridge = NULL;
}

/* ---- host side ------------------------------------------------------------ */

static void build_dest(hml_dest_cfg *cfg, uint16_t data_port) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->kind = HML_XPORT_UDP;
  cfg->ipv4 = HML_IP_LOOPBACK;
  cfg->udp_port = data_port;
  cfg->frame_size = TEST_FRAME_SIZE;
  cfg->hop_limit = 64u;
}

/* ---- steps ---------------------------------------------------------------- */

static void step_discovery(hml_client_t *cl, hml_bridge_t *b) {
  uint32_t uuid = 0, caps = 0, fw = 0;
  uint64_t ts = 0;
  hml_status st = hml_client_discover(cl, 1500, &uuid, &caps, &fw, &ts);
  T_ST(st, HML_OK, "discovery beacon");
  T_EQ_U(uuid, TEST_UUID, "beacon uuid");
  T_EQ_U(caps, TEST_CAPS, "beacon caps");
  T_EQ_U(fw, TEST_FW, "beacon fw");
  T_CHECK(ts > 0ull, "beacon carries a timestamp");
  T_CHECK((caps & HML_CAP_BRIDGE) != 0u, "caps advertise HML_CAP_BRIDGE");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_INIT, "still in INIT");
}

static void step_caps(hml_client_t *cl) {
  uint32_t uuid = 0, caps = 0, fw = 0;
  T_ST(hml_client_get_caps(cl, &uuid, &caps, &fw), HML_OK, "GET_CAPS");
  T_EQ_U(uuid, TEST_UUID, "caps uuid");
  T_EQ_U(caps, TEST_CAPS, "caps mask");
  T_EQ_U(fw, TEST_FW, "caps fw");

  uint32_t echo = 0;
  T_ST(hml_client_ping(cl, 0xfeedf00du, &echo), HML_OK, "PING");
  T_EQ_U(echo, 0xfeedf00du, "PING echoes the token");
}

static void step_config_pokes(hml_client_t *cl, hml_bridge_t *b) {
  bool streaming = true;
  T_ST(hml_client_status(cl, &streaming), HML_OK, "STATUS before START");
  T_CHECK(!streaming, "STATUS says stopped");
  T_CHECK(!hml_bridge_is_streaming(b), "bridge is not streaming");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_INIT, "bridge still in INIT");

  uint32_t rb = 0;
  T_ST(hml_client_poke(cl, hml_bridge_stub_gpio_addr(HML_STUB_GPIO_REG_RESET), 1u, &rb), HML_OK,
       "POKE gpio reset");
  T_EQ_U(rb, 1u, "gpio reset readback");
  T_CHECK(hml_bridge_stub_gpio_reset_asserted(b), "device asserted the reset");

  uint32_t flag = 0;
  T_ST(hml_bridge_read32(b, hml_bridge_stub_gpio_addr(HML_STUB_GPIO_REG_RESET_FLAG), &flag), HML_OK,
       "read the reset flag");
  T_EQ_U(flag, 1u, "reset flag is 1");

  rb = 0;
  T_ST(hml_client_poke(cl, hml_bridge_stub_i2c_addr(TEST_REG_ENABLE), TEST_SENSOR_ENABLE, &rb), HML_OK,
       "POKE sensor enable");
  T_EQ_U(rb, TEST_SENSOR_ENABLE, "sensor enable readback");
  rb = 0;
  T_ST(hml_client_poke(cl, hml_bridge_stub_i2c_addr(TEST_REG_FRAME), TEST_FRAME_SIZE, &rb), HML_OK,
       "POKE frame size");
  T_EQ_U(rb, TEST_FRAME_SIZE, "frame size readback");

  /* The packetizer is write protected from INIT, over the wire as well. */
  rb = 0xdeadbeefu;
  hml_status st = hml_client_poke(cl, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_IP), 0x7f000001u,
                                  &rb);
  T_ST(st, HML_ERR_NOTSUP, "POKE the packetizer from INIT fails");
  T_EQ_U(hml_client_last_device_status(cl), (unsigned)HML_ERR_STATE, "device says HML_ERR_STATE");
  T_EQ_U(rb, 0xdeadbeefu, "the failed poke left the host readback alone");
}

static void step_set_dest(hml_client_t *cl, hml_bridge_t *b, uint16_t data_port) {
  hml_dest_cfg want, got, rb;
  build_dest(&want, data_port);
  memset(&got, 0, sizeof(got));
  memset(&rb, 0, sizeof(rb));

  T_ST(hml_client_set_dest(cl, &want), HML_OK, "SET_DEST");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_CONFIG, "SET_DEST moved the bridge to CONFIG");

  T_ST(hml_client_get_dest(cl, &got), HML_OK, "GET_DEST over the wire");
  T_CHECK(memcmp(&want, &got, sizeof(want)) == 0, "GET_DEST round trips the whole hml_dest_cfg");
  T_ST(hml_bridge_stub_get_dest(b, &rb), HML_OK, "the packetizer words hold the dest");
  T_CHECK(memcmp(&want, &rb, sizeof(want)) == 0, "packetizer round trip is exact");

  uint32_t ip = 0, port = 0;
  T_ST(hml_bridge_read32(b, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_IP), &ip), HML_OK, "read ip");
  T_ST(hml_bridge_read32(b, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_PORT), &port), HML_OK, "read port");
  T_EQ_U(ip, HML_IP_LOOPBACK, "pktz reg 0 is the dest ipv4");
  T_EQ_U(port, (unsigned)data_port, "pktz reg 1 is the dest udp port");
}

static void step_start(hml_client_t *cl, hml_bridge_t *b) {
  T_ST(hml_client_start(cl), HML_OK, "START");
  bool streaming = false;
  T_ST(hml_client_status(cl, &streaming), HML_OK, "STATUS after START");
  T_CHECK(streaming, "STATUS says streaming");
  T_CHECK(hml_bridge_is_streaming(b), "bridge is streaming");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_STREAM, "bridge is in STREAM");
}

static void step_stream_pokes(hml_client_t *cl, hml_bridge_t *b) {
  /* 20 poke / readback round trips while the data plane would be running. */
  uint32_t bad = 0;
  for (uint32_t i = 0; i < TEST_STREAM_POKES; i++) {
    uint32_t addr = hml_bridge_stub_i2c_addr(TEST_REG_BASE + i);
    uint32_t want = 0x5a5a0000u | i;
    uint32_t rb = 0;
    if (hml_client_poke(cl, addr, want, &rb) != HML_OK || rb != want) bad++;
    uint32_t back = 0;
    if (hml_client_readback(cl, addr, &back) != HML_OK || back != want) bad++;
    /* The device side sees the same value, straight from the register file. */
    uint32_t dev = 0;
    if (hml_bridge_stub_i2c_get(b, TEST_REG_BASE + i, &dev) != HML_OK || dev != want) bad++;
    T_EQ_U(bad, 0u, "round trip %u", (unsigned)i);
  }
  T_EQ_U(bad, 0u, "all %u mid-stream round trips clean", (unsigned)TEST_STREAM_POKES);
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_STREAM, "still STREAM after the pokes");

  /* The uart window is byte granular but reachable with a 4-byte poke: the
   * write pushes four bytes and the readback pops the same four. */
  uint32_t rb = 0;
  T_ST(hml_client_poke(cl, HML_STUB_UART_BASE, 0x44434241u, &rb), HML_OK, "POKE the uart fifo");
  T_EQ_U(rb, 0x44434241u, "uart round trip through the fifo");
  T_EQ_U(hml_bridge_stub_uart_count(b), 0u, "the poke left the fifo empty");

  /* Reading an empty fifo must fail rather than invent bytes. The frozen
   * control server reports any failed readback as HML_ERR_STATE on the wire
   * (the specific status, here HML_ERR_AGAIN, only reaches the device log), so
   * the host sees HML_ERR_NOTSUP. */
  T_ST(hml_client_readback(cl, HML_STUB_UART_BASE, &rb), HML_ERR_NOTSUP, "READBACK an empty uart fails");
  T_EQ_U(hml_client_last_device_status(cl), (unsigned)HML_ERR_STATE, "device says HML_ERR_STATE");
}

static void step_stop(hml_client_t *cl, hml_bridge_t *b) {
  T_ST(hml_client_stop(cl), HML_OK, "STOP");
  bool streaming = true;
  T_ST(hml_client_status(cl, &streaming), HML_OK, "STATUS after STOP");
  T_CHECK(!streaming, "STATUS says stopped");
  T_CHECK(!hml_bridge_is_streaming(b), "bridge stopped");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_CONFIG, "bridge back in CONFIG");

  /* Every mid-stream poke survived the stop. */
  uint32_t rb = 0, dev = 0;
  T_ST(hml_client_readback(cl, hml_bridge_stub_i2c_addr(TEST_REG_BASE), &rb), HML_OK, "READBACK after STOP");
  T_EQ_U(rb, 0x5a5a0000u, "the first mid-stream poke survived");
  T_ST(hml_bridge_stub_i2c_get(b, TEST_REG_BASE + TEST_STREAM_POKES - 1u, &dev), HML_OK, "last register");
  T_EQ_U(dev, 0x5a5a0000u | (TEST_STREAM_POKES - 1u), "the last mid-stream poke survived");
}

static void step_verify_dump(hml_client_t *cl, hml_bridge_t *b, uint16_t data_port) {
  char dump[192];
  T_ST(hml_bridge_stub_dump(b, dump, sizeof(dump)), HML_OK, "register dump");

  uint32_t v = 0;
  T_ST(hml_bridge_stub_i2c_get(b, TEST_REG_ENABLE, &v), HML_OK, "get enable");
  T_EQ_U(v, TEST_SENSOR_ENABLE, "enable register as the host wrote it");
  T_ST(hml_bridge_stub_i2c_get(b, TEST_REG_FRAME, &v), HML_OK, "get frame size");
  T_EQ_U(v, TEST_FRAME_SIZE, "frame size register as the host wrote it");
  for (uint32_t i = 0; i < TEST_STREAM_POKES; i++) {
    T_ST(hml_bridge_stub_i2c_get(b, TEST_REG_BASE + i, &v), HML_OK, "get %u", (unsigned)i);
    T_EQ_U(v, 0x5a5a0000u | i, "register %u as the host wrote it", (unsigned)i);
  }
  T_ST(hml_bridge_stub_gpio_get(b, HML_STUB_GPIO_REG_RESET, &v), HML_OK, "get gpio reset");
  T_EQ_U(v, 1u, "gpio reset register as the host wrote it");
  T_CHECK(hml_bridge_stub_gpio_reset_asserted(b), "reset still asserted");
  T_EQ_U(hml_bridge_stub_uart_count(b), 0u, "the uart fifo is empty");

  /* The host and the device agree on the destination one more time. */
  hml_dest_cfg want, got;
  build_dest(&want, data_port);
  memset(&got, 0, sizeof(got));
  T_ST(hml_client_get_dest(cl, &got), HML_OK, "GET_DEST after the stream");
  T_CHECK(memcmp(&want, &got, sizeof(want)) == 0, "the destination survived the whole lifecycle");
  (void)cl;
}

static void step_nak_fault(hml_client_t *cl, hml_bridge_t *b) {
  /* Two i2c pokes are allowed through, every later one NAKs. */
  T_ST(hml_bridge_stub_set_nak_after(b, 2), HML_OK, "arm the NAK after 2 writes");

  uint32_t rb = 0;
  T_ST(hml_client_poke(cl, hml_bridge_stub_i2c_addr(0x60u), 0x60000001u, &rb), HML_OK, "NAK poke 1");
  T_EQ_U(rb, 0x60000001u, "poke 1 landed");
  T_ST(hml_client_poke(cl, hml_bridge_stub_i2c_addr(0x61u), 0x60000002u, &rb), HML_OK, "NAK poke 2");
  T_EQ_U(rb, 0x60000002u, "poke 2 landed");

  rb = 0xfeedfaceu;
  hml_status st = hml_client_poke(cl, hml_bridge_stub_i2c_addr(0x62u), 0x60000003u, &rb);
  T_ST(st, HML_ERR_NOTSUP, "poke 3 is NAKed over the wire");
  T_EQ_U(hml_client_last_device_status(cl), (unsigned)HML_ERR_STATE, "device status is HML_ERR_STATE");
  T_EQ_U(rb, 0xfeedfaceu, "the NAKed poke left the host readback alone");
  uint32_t v = 0;
  T_ST(hml_bridge_stub_i2c_get(b, 0x62u, &v), HML_OK, "read reg 0x62");
  T_EQ_U(v, 0u, "the NAKed poke did not change the register");

  /* Every later i2c poke keeps failing, and the readbacks of good registers
   * keep working: the fault is a peripheral fault, not a dead link. */
  T_ST(hml_client_poke(cl, hml_bridge_stub_i2c_addr(0x63u), 1u, &rb), HML_ERR_NOTSUP, "poke 4 is NAKed");
  T_ST(hml_client_readback(cl, hml_bridge_stub_i2c_addr(0x60u), &v), HML_OK, "READBACK of a good register");
  T_EQ_U(v, 0x60000001u, "the good register still reads back");

  /* PING keeps working while the i2c window is faulting. */
  uint32_t echo = 0;
  T_ST(hml_client_ping(cl, 0x0badc0deu, &echo), HML_OK, "PING while the i2c window NAKs");
  T_EQ_U(echo, 0x0badc0deu, "PING token echoed while faulting");
  bool streaming = true;
  T_ST(hml_client_status(cl, &streaming), HML_OK, "STATUS while faulting");
  T_CHECK(!streaming, "still stopped");

  /* GPIO is a different peripheral: unaffected by the i2c NAK. */
  rb = 0;
  T_ST(hml_client_poke(cl, hml_bridge_stub_gpio_addr(HML_STUB_GPIO_REG_RESET), 0u, &rb), HML_OK,
       "POKE gpio while the i2c window NAKs");
  T_EQ_U(rb, 0u, "gpio poke landed");

  /* Disarm: the window works again. */
  T_ST(hml_bridge_stub_set_nak_after(b, -1), HML_OK, "disarm the NAK");
  T_ST(hml_client_poke(cl, hml_bridge_stub_i2c_addr(0x63u), 0x63000001u, &rb), HML_OK, "poke after disarm");
  T_EQ_U(rb, 0x63000001u, "the window works again");
  T_CHECK(hml_client_last_device_status(cl) == HML_OK, "last device status is OK again");
}

static void step_restart(hml_client_t *cl, hml_bridge_t *b) {
  /* A restart after a stop needs no new SET_DEST: the config is kept. */
  T_ST(hml_client_start(cl), HML_OK, "START again");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_STREAM, "streaming again");
  uint32_t v = 0;
  T_ST(hml_client_readback(cl, hml_bridge_stub_i2c_addr(TEST_REG_BASE), &v), HML_OK, "READBACK while streaming");
  T_EQ_U(v, 0x5a5a0000u, "the register survived the restart");
  T_ST(hml_client_stop(cl), HML_OK, "STOP again");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_CONFIG, "back in CONFIG");

  /* RESET takes the device back to INIT, packetizer protected again. */
  T_ST(hml_client_get_dest(cl, NULL), HML_ERR_ARG, "GET_DEST into NULL is refused");

  /* RESET stops the data plane; the bridge keeps its configuration (a device
   * reset, i.e. back to INIT, is a local decision the host cannot force here). */
  T_ST(hml_client_reset(cl), HML_OK, "RESET");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_CONFIG,
         "RESET stops the data plane and keeps the configuration");
  hml_dest_cfg got;
  memset(&got, 0, sizeof(got));
  T_ST(hml_client_get_dest(cl, &got), HML_ERR_NOTSUP, "GET_DEST after RESET fails");
  T_EQ_U(hml_client_last_device_status(cl), (unsigned)HML_ERR_STATE, "device says HML_ERR_STATE");
  T_ST(hml_bridge_stub_get_dest(b, &got), HML_OK, "the packetizer words still hold the dest");
}

int main(int argc, char **argv) {
  uint32_t base = 45600u;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--base") == 0 && (i + 1) < argc) {
      base = (uint32_t)strtoul(argv[++i], NULL, 0);
    } else {
      fprintf(stderr, "usage: %s [--base N]\n", argv[0]);
      return 2;
    }
  }
  const uint16_t disc_port = (uint16_t)base;
  const uint16_t data_port = (uint16_t)(base + 1u);
  const uint16_t ctrl_port = (uint16_t)(base + 2u);
  const uint16_t client_port = (uint16_t)(base + 99u);

  printf("test_bridge_lifecycle: control=%u data=%u beacon=%u client=%u\n", (unsigned)ctrl_port,
         (unsigned)data_port, (unsigned)disc_port, (unsigned)client_port);
  fflush(stdout);

  dev_t d;
  if (dev_up(&d, ctrl_port, disc_port, client_port) != HML_OK) {
    fprintf(stderr, "FAIL: the device did not come up (port %u busy?)\n", (unsigned)ctrl_port);
    return 1;
  }

  hml_client_t *cl = NULL;
  hml_status st = hml_client_open(&cl, client_port);
  if (st == HML_OK) st = hml_client_set_peer(cl, HML_IP_LOOPBACK, ctrl_port, disc_port);
  if (st == HML_OK) st = hml_client_set_timeout(cl, 2000);
  if (st != HML_OK) {
    fprintf(stderr, "FAIL: control client setup failed (%d)\n", (int)st);
    if (cl) hml_client_close(cl);
    dev_down(&d);
    return 1;
  }

  step_discovery(cl, d.bridge);
  step_caps(cl);
  step_config_pokes(cl, d.bridge);
  step_set_dest(cl, d.bridge, data_port);
  step_start(cl, d.bridge);
  step_stream_pokes(cl, d.bridge);
  step_stop(cl, d.bridge);
  step_verify_dump(cl, d.bridge, data_port);
  step_nak_fault(cl, d.bridge);
  step_restart(cl, d.bridge);

  /* The control plane served every request and rejected nothing malformed. */
  T_EQ_U(hml_control_bad_count(d.ctl), 0u, "no bad control datagrams");
  T_CHECK(hml_control_rx_count(d.ctl) > 0u, "the control plane served requests");
  T_CHECK(!hml_control_streaming(d.ctl), "the data plane is stopped at the end");

  hml_client_close(cl);
  dev_down(&d);
  T_DONE("test_bridge_lifecycle");
}
