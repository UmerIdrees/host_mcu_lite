/*
 * hml_bridge_demo - M4 bridge mode over the real control plane.
 *
 * Two modes, both on loopback high ports (overridable on the command line):
 *
 *   --lifecycle  device control server in a pthread, host client in this
 *                thread: init -> GPIO reset poke -> I2C register writes ->
 *                program the packetizer dest -> START -> stream N frames ->
 *                STOP, every step printed with PASS/FAIL. Exits 0 only if all
 *                steps passed.
 *   --serve      the device side only: control server, beacon, and a data path
 *                that streams frames to the configured destination while
 *                streaming, until SIGINT.
 *
 * The device side is a bridge mode device: hml_bridge_stub_hml_device() gives
 * it gpio / i2c / uart / packetizer windows, and the control callbacks are the
 * bridge glue (hml_bridge_control_poke_cb / read_cb / set_dest / start / stop)
 * composed with the data path, so the control context stays the device state.
 * The data path is the real framer (hml_stream from hml_core) over UDP, with
 * the small hml_xport_ops below so the demo does not depend on the transports
 * track.
 *
 * Defaults: control 45602, data 45601, beacon 45600, client reply 45699.
 */

#include "hml_bridge.h"
#include "hml_bridge_control.h"
#include "hml_bridge_stub.h"

#include "hml/hml_crc.h"
#include "hml/hml_sock.h"
#include "hml/hml_stream.h"
#include "hml/hml_transport.h"

#include "hml_client.h"
#include "hml_control.h"

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_CTRL_PORT 45602u
#define DEMO_DATA_PORT 45601u
#define DEMO_DISC_PORT 45600u
#define DEMO_CLIENT_PORT 45699u
#define DEMO_UUID 0x00000042u
#define DEMO_CAPS (HML_CAP_UDP | HML_CAP_BRIDGE | HML_CAP_MCU)
#define DEMO_FW 0x00010400u
#define DEMO_FRAME_BYTES 256u

/* Sensor register file: reg 0x10 enables the sensor, reg 0x11 frame size. */
#define DEMO_I2C_REG_ENABLE 0x10u
#define DEMO_I2C_REG_FRAME 0x11u
#define DEMO_I2C_REG_EXPOSURE 0x20u
#define DEMO_SENSOR_ENABLE 0x80000001u
#define DEMO_FRAME_SIZE 4096u

/* ---- demo-local UDP transport (HML framed datagrams over UDP) ------------ */

typedef struct {
  hml_sock_t *sock;
  uint8_t buf[HML_MAX_FRAG_SIZE];
} xport_impl_t;

static hml_status xport_open(hml_xport_t *x, const hml_dest_cfg *dest, uint16_t stream_id) {
  (void)stream_id;
  xport_impl_t *im = (xport_impl_t *)calloc(1, sizeof(*im));
  if (!im) return HML_ERR_NOMEM;
  /* hml_sock_set_dest takes the address in the very form hml_dest_cfg.ipv4
   * carries (the value hml_get_be32 yields, the same convention as
   * HML_IP_LOOPBACK), so no byte swap belongs here. */
  if (hml_sock_udp_create(&im->sock, HML_IP_ANY, 0u, false) != HML_OK) {
    free(im);
    return HML_ERR_IO;
  }
  hml_status st = hml_sock_set_dest(im->sock, dest->ipv4, dest->udp_port);
  if (st != HML_OK) {
    hml_sock_close(im->sock);
    free(im);
    return st;
  }
  x->impl = im;
  return HML_OK;
}

static hml_status xport_send_frag(hml_xport_t *x, const hml_frame_hdr *h, const uint8_t *payload,
                                  size_t payload_len) {
  xport_impl_t *im = (xport_impl_t *)x->impl;
  if (!im || !im->sock) return HML_ERR_ARG;
  if (payload_len > (size_t)HML_MAX_FRAG_PAYLOAD) return HML_ERR_TOOLARGE;

  hml_proto_frame_fill(h, im->buf);
  if (payload_len) memcpy(im->buf + HML_FRAME_HDR_SIZE, payload, payload_len);
  hml_put_be16(im->buf + HML_FRAME_HDR_SIZE + payload_len,
               hml_crc16(im->buf + HML_FRAME_HDR_SIZE, payload_len));
  return hml_sock_send(im->sock, im->buf,
                       (size_t)HML_FRAME_HDR_SIZE + payload_len + (size_t)HML_TRAIL_CRC_SIZE);
}

static hml_status xport_flush(hml_xport_t *x) {
  (void)x;
  return HML_OK; /* unbuffered */
}

static void xport_close(hml_xport_t *x) {
  xport_impl_t *im = (xport_impl_t *)x->impl;
  if (!im) return;
  hml_sock_close(im->sock);
  free(im);
  x->impl = NULL;
}

static const hml_xport_ops g_xport_ops = {"m4-udp", xport_open, xport_send_frag, xport_flush,
                                          xport_close};

/* ---- device (control server + data path) -------------------------------- */

typedef struct {
  hml_control_t *ctl;
  hml_bridge_t *bridge;
  hml_stream_t *st;
  hml_xport_t xport;
  uint16_t stream_id;
  uint32_t frames_total;
  uint32_t frames_sent;
  uint32_t pace_us;
  hml_dest_cfg dest;
  bool have_dest;
  bool streaming;
  volatile sig_atomic_t run;
  pthread_t th;
} dev_t;

static dev_t *g_dev_for_signal = NULL;
static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig) {
  (void)sig;
  g_stop = 1;
  if (g_dev_for_signal) g_dev_for_signal->run = 0;
}

static void sleep_us(uint32_t us) {
  struct timespec ts;
  ts.tv_sec = (time_t)(us / 1000000u);
  ts.tv_nsec = (long)(us % 1000000u) * 1000L;
  (void)nanosleep(&ts, NULL);
}

/* The bridge glue is wired with the bridge as context; the device needs its own
 * context for the data path, so the two are composed here. */
static uint32_t dev_poke(void *ctx, uint32_t addr, uint32_t val, bool *ok) {
  return hml_bridge_control_poke_cb()(((dev_t *)ctx)->bridge, addr, val, ok);
}

static uint32_t dev_read(void *ctx, uint32_t addr, bool *ok) {
  return hml_bridge_control_read_cb()(((dev_t *)ctx)->bridge, addr, ok);
}

static void dev_set_dest(void *ctx, const hml_dest_cfg *cfg) {
  dev_t *d = (dev_t *)ctx;
  hml_bridge_control_set_dest_cb(d->bridge, cfg); /* phase + packetizer words */
  d->dest = *cfg;
  d->have_dest = true;
  printf("device: dest kind=%u ipv4=%u.%u.%u.%u port=%u frame_size=%u\n", (unsigned)cfg->kind,
         (unsigned)((cfg->ipv4 >> 24) & 0xffu), (unsigned)((cfg->ipv4 >> 16) & 0xffu),
         (unsigned)((cfg->ipv4 >> 8) & 0xffu), (unsigned)(cfg->ipv4 & 0xffu),
         (unsigned)cfg->udp_port, (unsigned)cfg->frame_size);
  fflush(stdout);
}

static void dev_start(void *ctx) {
  dev_t *d = (dev_t *)ctx;
  hml_bridge_control_start_cb(d->bridge);
  if (!hml_bridge_is_streaming(d->bridge)) {
    fprintf(stderr, "device: start refused (phase %s)\n",
            hml_bridge_phase_str(hml_bridge_phase_of(d->bridge)));
    fflush(stderr);
    return;
  }
  d->streaming = true;
  printf("device: start phase=%s\n", hml_bridge_phase_str(hml_bridge_phase_of(d->bridge)));
  fflush(stdout);
}

static void dev_stop(void *ctx) {
  dev_t *d = (dev_t *)ctx;
  hml_bridge_control_stop_cb(d->bridge);
  d->streaming = false;
  if (d->st) {
    (void)hml_stream_close(d->st);
    d->st = NULL;
  }
  printf("device: stop frames_sent=%u phase=%s\n", (unsigned)d->frames_sent,
         hml_bridge_phase_str(hml_bridge_phase_of(d->bridge)));
  fflush(stdout);
}

static void dev_send_one(dev_t *d) {
  if (!d->st) {
    /* hml_stream_open adopts an already open transport: open it first. */
    const hml_xport_ops *ops = hml_transport_lookup("m4-udp");
    if (!ops) {
      fprintf(stderr, "device: transport m4-udp not registered\n");
      d->streaming = false;
      return;
    }
    hml_status st = ops->open(&d->xport, &d->dest, d->stream_id);
    if (st == HML_OK) {
      hml_stream_cfg sc;
      memset(&sc, 0, sizeof(sc));
      sc.stream_id = d->stream_id;
      sc.max_frame_bytes = (size_t)DEMO_FRAME_BYTES * 4u;
      sc.seq_start = 1u;
      st = hml_stream_open(&d->st, &sc, &d->xport);
    }
    if (st != HML_OK) {
      fprintf(stderr, "device: stream open failed: %d\n", (int)st);
      if (d->xport.impl) xport_close(&d->xport);
      d->streaming = false;
      return;
    }
  }

  uint8_t payload[DEMO_FRAME_BYTES];
  hml_put_be32(payload, d->frames_sent);
  for (unsigned i = 4u; i < DEMO_FRAME_BYTES; i++)
    payload[i] = (uint8_t)((d->frames_sent * 7u + i) & 0xffu);
  if (hml_stream_send_frag(d->st, payload, sizeof(payload), true, true, hml_clock_us()) == HML_OK)
    d->frames_sent++;
}

/* ---- trace helpers ------------------------------------------------------- */

static unsigned g_step;
static unsigned g_pass;
static unsigned g_fail;

static void step(const char *name, bool ok, const char *fmt, ...) {
  g_step++;
  if (ok) {
    g_pass++;
  } else {
    g_fail++;
  }
  printf("[%2u] %-28s %s", g_step, name, ok ? "PASS" : "FAIL");
  if (fmt && *fmt) {
    va_list ap;
    printf("  ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
  }
  printf("\n");
  fflush(stdout);
}

static void build_dest(hml_dest_cfg *cfg, uint16_t data_port) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->kind = HML_XPORT_UDP;
  cfg->ipv4 = 0x7f000001u; /* 127.0.0.1, the hml_get_be32 form */
  cfg->udp_port = data_port;
  cfg->frame_size = DEMO_FRAME_SIZE;
  cfg->hop_limit = 64u;
}

/* ---- device bring-up ----------------------------------------------------- */

static hml_status open_device(dev_t *d, uint16_t ctrl_port, uint16_t disc_port, uint16_t reply_port,
                              bool beacon) {
  memset(d, 0, sizeof(*d));
  d->xport.ops = &g_xport_ops;
  d->stream_id = 1u;
  d->pace_us = 500u;

  hml_control_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.ctrl_port = ctrl_port;
  cfg.disc_port = disc_port;
  cfg.beacon_enable = beacon;
  cfg.beacon_period_ms = HML_CTRL_DEFAULT_BEACON_PERIOD_MS;
  cfg.uuid = DEMO_UUID;
  cfg.caps = DEMO_CAPS;
  cfg.fw_ver = DEMO_FW;
  cfg.reply_ip = HML_IP_LOOPBACK;
  cfg.reply_port = reply_port;

  hml_status st = hml_bridge_stub_hml_device(&d->bridge);
  if (st != HML_OK) return st;
  st = hml_control_open(&d->ctl, &cfg);
  if (st != HML_OK) {
    hml_bridge_close(d->bridge);
    d->bridge = NULL;
    return st;
  }
  st = hml_control_set_cbs(d->ctl, dev_set_dest, dev_start, dev_stop, dev_poke, d);
  if (st == HML_OK) st = hml_control_set_read_cb(d->ctl, dev_read, d);
  if (st != HML_OK) {
    hml_control_close(d->ctl);
    hml_bridge_close(d->bridge);
    d->ctl = NULL;
    d->bridge = NULL;
    return st;
  }
  printf("device: up ctrl=%u disc=%u reply=127.0.0.1:%u beacon=%s caps=0x%02x windows=%u\n",
         (unsigned)ctrl_port, (unsigned)disc_port, (unsigned)reply_port, beacon ? "on" : "off",
         (unsigned)DEMO_CAPS, hml_bridge_periph_count(d->bridge));
  fflush(stdout);
  return HML_OK;
}

static void close_device(dev_t *d) {
  if (d->st) (void)hml_stream_close(d->st);
  if (d->ctl) hml_control_close(d->ctl);
  if (d->bridge) hml_bridge_close(d->bridge);
  d->st = NULL;
  d->ctl = NULL;
  d->bridge = NULL;
}

static void *dev_thread(void *arg) {
  dev_t *d = (dev_t *)arg;
  while (d->run) {
    hml_status st = hml_control_poll(d->ctl, d->streaming ? 1 : 20);
    if (st == HML_ERR_IO) {
      fprintf(stderr, "device: control poll io error\n");
      d->run = 0;
      break;
    }
    if (d->streaming && d->have_dest &&
        (d->frames_total == 0u || d->frames_sent < d->frames_total)) {
      dev_send_one(d);
      if (d->frames_total != 0u && d->frames_sent == d->frames_total) {
        printf("device: frames done sent=%u\n", (unsigned)d->frames_sent);
        fflush(stdout);
      }
      if (d->pace_us) sleep_us(d->pace_us);
    }
  }
  return NULL;
}

static hml_status dev_start_thread(dev_t *d) {
  d->run = 1;
  if (pthread_create(&d->th, NULL, dev_thread, d) != 0) return HML_ERR_UNKNOWN;
  return HML_OK;
}

static void dev_stop_thread(dev_t *d) {
  d->run = 0;
  (void)pthread_join(d->th, NULL);
}

/* ---- lifecycle mode ------------------------------------------------------ */

static int run_lifecycle(uint16_t ctrl_port, uint16_t data_port, uint16_t disc_port,
                         uint16_t client_port, uint32_t frames, uint32_t pace_us) {
  dev_t d;
  hml_sock_t *rx = NULL;
  hml_client_t *cl = NULL;
  int rc = 1;

  printf("=== hml_bridge_demo --lifecycle (M4 bridge mode) ===\n");
  printf("control=%u data=%u beacon=%u client=%u frames=%u\n\n", (unsigned)ctrl_port,
         (unsigned)data_port, (unsigned)disc_port, (unsigned)client_port, (unsigned)frames);

  if (open_device(&d, ctrl_port, disc_port, client_port, true) != HML_OK) {
    fprintf(stderr, "FAIL: device did not come up (port %u busy?)\n", (unsigned)ctrl_port);
    return 1;
  }
  d.frames_total = frames;
  d.pace_us = pace_us;
  g_dev_for_signal = &d;

  if (hml_sock_udp_create(&rx, HML_IP_LOOPBACK, data_port, false) != HML_OK) {
    fprintf(stderr, "FAIL: cannot bind the data receiver on port %u\n", (unsigned)data_port);
    close_device(&d);
    return 1;
  }
  if (hml_client_open(&cl, client_port) != HML_OK ||
      hml_client_set_peer(cl, HML_IP_LOOPBACK, ctrl_port, disc_port) != HML_OK) {
    fprintf(stderr, "FAIL: control client setup failed\n");
    if (cl) hml_client_close(cl);
    hml_sock_close(rx);
    close_device(&d);
    return 1;
  }
  (void)hml_client_set_timeout(cl, 2000);

  if (dev_start_thread(&d) != HML_OK) {
    fprintf(stderr, "FAIL: device thread did not start\n");
    hml_client_close(cl);
    hml_sock_close(rx);
    close_device(&d);
    return 1;
  }

  /* 1. init */
  {
    uint32_t uuid = 0, caps = 0, fw = 0;
    hml_status st = hml_client_get_caps(cl, &uuid, &caps, &fw);
    step("init (caps)", st == HML_OK && uuid == DEMO_UUID && (caps & HML_CAP_BRIDGE) != 0u,
         "uuid=0x%08x caps=0x%02x fw=0x%08x phase=%s", (unsigned)uuid, (unsigned)caps,
         (unsigned)fw, hml_bridge_phase_str(hml_bridge_phase_of(d.bridge)));
  }

  /* 2. GPIO reset poke */
  {
    uint32_t rb = 0;
    hml_status st =
        hml_client_poke(cl, hml_bridge_stub_gpio_addr(HML_STUB_GPIO_REG_RESET), 1u, &rb);
    uint32_t flag = 0;
    (void)hml_bridge_read32(d.bridge, hml_bridge_stub_gpio_addr(HML_STUB_GPIO_REG_RESET_FLAG),
                            &flag);
    step("gpio reset poke", st == HML_OK && rb == 1u && flag == 1u,
         "reset rback=0x%08x flag=0x%08x", (unsigned)rb, (unsigned)flag);
  }

  /* 3. I2C register writes */
  {
    uint32_t rb1 = 0, rb2 = 0;
    hml_status s1 = hml_client_poke(cl, hml_bridge_stub_i2c_addr(DEMO_I2C_REG_ENABLE),
                                     DEMO_SENSOR_ENABLE, &rb1);
    hml_status s2 = hml_client_poke(cl, hml_bridge_stub_i2c_addr(DEMO_I2C_REG_FRAME),
                                     DEMO_FRAME_SIZE, &rb2);
    step("i2c reg writes",
         s1 == HML_OK && s2 == HML_OK && rb1 == DEMO_SENSOR_ENABLE && rb2 == DEMO_FRAME_SIZE,
         "reg 0x%02x=0x%08x reg 0x%02x=%u", (unsigned)DEMO_I2C_REG_ENABLE, (unsigned)rb1,
         (unsigned)DEMO_I2C_REG_FRAME, (unsigned)rb2);
  }

  /* 4. program the packetizer dest */
  hml_dest_cfg want, got;
  build_dest(&want, data_port);
  memset(&got, 0, sizeof(got));
  {
    hml_status st = hml_client_set_dest(cl, &want);
    hml_status gt = hml_client_get_dest(cl, &got);
    hml_dest_cfg rb;
    memset(&rb, 0, sizeof(rb));
    hml_status pt = hml_bridge_stub_get_dest(d.bridge, &rb);
    bool same = pt == HML_OK && memcmp(&want, &rb, sizeof(want)) == 0 &&
                gt == HML_OK && memcmp(&want, &got, sizeof(want)) == 0;
    uint32_t ip = 0, port = 0;
    (void)hml_bridge_read32(d.bridge, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_IP), &ip);
    (void)hml_bridge_read32(d.bridge, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_PORT), &port);
    step("program packetizer dest", st == HML_OK && same, "phase=%s pktz ip=0x%08x port=%u rt=%s",
         hml_bridge_phase_str(hml_bridge_phase_of(d.bridge)), (unsigned)ip, (unsigned)port,
         same ? "exact" : "DIFF");
  }

  /* 5. START */
  {
    hml_status st = hml_client_start(cl);
    bool streaming = false;
    (void)hml_client_status(cl, &streaming);
    step("start", st == HML_OK && streaming && hml_bridge_is_streaming(d.bridge),
         "status=streaming phase=%s", hml_bridge_phase_str(hml_bridge_phase_of(d.bridge)));
  }

  /* 6. stream N frames, then a mid-stream exposure poke (must not tear) */
  {
    uint32_t got_frames = 0;
    uint32_t bad = 0;
    uint8_t buf[HML_MAX_FRAG_SIZE];
    uint64_t deadline = hml_clock_us() + 5000000ull;
    while (got_frames < frames && hml_clock_us() < deadline) {
      size_t n = 0;
      hml_status st = hml_sock_recv(rx, buf, sizeof(buf), &n, 100);
      if (st == HML_ERR_TIMEOUT || st == HML_ERR_AGAIN) continue;
      if (st != HML_OK) break;
      hml_frame_hdr h;
      const uint8_t *pl = NULL;
      if (hml_proto_frame_parse(buf, n, &h, &pl) != HML_OK) {
        bad++;
        continue;
      }
      if (h.flags & HML_FLAG_END) got_frames++;
    }
    uint32_t rb = 0;
    hml_status ps = hml_client_poke(cl, hml_bridge_stub_i2c_addr(DEMO_I2C_REG_EXPOSURE),
                                    0xa5a5a5a5u, &rb);
    step("stream frames", got_frames == frames && bad == 0u && ps == HML_OK && rb == 0xa5a5a5a5u,
         "frames=%u/%u bad=%u midstream poke=0x%08x", (unsigned)got_frames, (unsigned)frames,
         (unsigned)bad, (unsigned)rb);
  }

  /* 7. STOP, and the mid-stream poke is still there */
  {
    hml_status st = hml_client_stop(cl);
    bool streaming = true;
    (void)hml_client_status(cl, &streaming);
    uint32_t after = 0;
    hml_status rs = hml_client_readback(cl, hml_bridge_stub_i2c_addr(DEMO_I2C_REG_EXPOSURE), &after);
    char dump[192];
    (void)hml_bridge_stub_dump(d.bridge, dump, sizeof(dump));
    step("stop + verify",
         st == HML_OK && !streaming && !hml_bridge_is_streaming(d.bridge) && rs == HML_OK &&
             after == 0xa5a5a5a5u && hml_bridge_phase_of(d.bridge) == HML_BRIDGE_PHASE_CONFIG,
         "phase=%s poke survived=%s | %s", hml_bridge_phase_str(hml_bridge_phase_of(d.bridge)),
         after == 0xa5a5a5a5u ? "yes" : "NO", dump);
  }

  printf("\nlifecycle: %u/%u steps PASS, %u FAIL\n", g_pass, g_step, g_fail);
  rc = (g_fail == 0u) ? 0 : 1;

  dev_stop_thread(&d);
  hml_client_close(cl);
  hml_sock_close(rx);
  close_device(&d);
  g_dev_for_signal = NULL;
  return rc;
}

/* ---- serve mode ---------------------------------------------------------- */

static int run_serve(uint16_t ctrl_port, uint16_t data_port, uint16_t disc_port,
                     uint16_t client_port, uint32_t frames, uint32_t pace_us, bool beacon) {
  dev_t d;
  if (open_device(&d, ctrl_port, disc_port, client_port, beacon) != HML_OK) {
    fprintf(stderr, "serve: device did not come up (port %u busy?)\n", (unsigned)ctrl_port);
    return 1;
  }
  d.frames_total = frames;
  d.pace_us = pace_us;
  d.run = 1;
  g_dev_for_signal = &d;

  printf("serve: ctrl=%u data=%u beacon=%u reply=127.0.0.1:%u - drive with hml_ctrl_host, "
         "SIGINT to stop\n",
         (unsigned)ctrl_port, (unsigned)data_port, (unsigned)disc_port, (unsigned)client_port);
  fflush(stdout);

  int rc = 0;
  while (g_stop == 0) {
    hml_status st = hml_control_poll(d.ctl, d.streaming ? 1 : 20);
    if (st == HML_ERR_IO) {
      fprintf(stderr, "serve: control poll io error\n");
      rc = 1;
      break;
    }
    if (d.streaming && d.have_dest &&
        (d.frames_total == 0u || d.frames_sent < d.frames_total)) {
      dev_send_one(&d);
      if (d.pace_us) sleep_us(d.pace_us);
    }
  }

  printf("serve: down rx=%u bad=%u frames=%u phase=%s\n", (unsigned)hml_control_rx_count(d.ctl),
         (unsigned)hml_control_bad_count(d.ctl), (unsigned)d.frames_sent,
         hml_bridge_phase_str(hml_bridge_phase_of(d.bridge)));
  fflush(stdout);
  close_device(&d);
  g_dev_for_signal = NULL;
  return rc;
}

/* ---- main ---------------------------------------------------------------- */

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [--lifecycle | --serve] [options]\n"
          "  --lifecycle        run the full M4 script in-process (default)\n"
          "  --serve            run the device control server until SIGINT\n"
          "  --ctrl-port N      control plane port (default %u)\n"
          "  --data-port N      data plane port (default %u)\n"
          "  --disc-port N      beacon port (default %u)\n"
          "  --client-port N    port the host client binds / device replies to (default %u)\n"
          "  --no-beacon        do not emit the discovery beacon\n"
          "  --frames N         frames to stream after START, 0 = forever (default 8)\n"
          "  --pace-us N        inter-frame delay in us (default 500)\n",
          argv0, (unsigned)DEMO_CTRL_PORT, (unsigned)DEMO_DATA_PORT, (unsigned)DEMO_DISC_PORT,
          (unsigned)DEMO_CLIENT_PORT);
}

int main(int argc, char **argv) {
  bool serve = false;
  uint16_t ctrl_port = DEMO_CTRL_PORT;
  uint16_t data_port = DEMO_DATA_PORT;
  uint16_t disc_port = DEMO_DISC_PORT;
  uint16_t client_port = DEMO_CLIENT_PORT;
  bool beacon = true;
  uint32_t frames = 8u;
  uint32_t pace_us = 500u;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    bool has_val = (i + 1) < argc;
#define NEED_VAL()                                  \
  do {                                              \
    if (!has_val) {                                 \
      fprintf(stderr, "missing value for %s\n", a); \
      return 2;                                     \
    }                                               \
  } while (0)
    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
      usage(argv[0]);
      return 0;
    } else if (strcmp(a, "--lifecycle") == 0) {
      serve = false;
    } else if (strcmp(a, "--serve") == 0) {
      serve = true;
    } else if (strcmp(a, "--no-beacon") == 0) {
      beacon = false;
    } else if (strcmp(a, "--ctrl-port") == 0) {
      NEED_VAL();
      ctrl_port = (uint16_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--data-port") == 0) {
      NEED_VAL();
      data_port = (uint16_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--disc-port") == 0) {
      NEED_VAL();
      disc_port = (uint16_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--client-port") == 0) {
      NEED_VAL();
      client_port = (uint16_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--frames") == 0) {
      NEED_VAL();
      frames = (uint32_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--pace-us") == 0) {
      NEED_VAL();
      pace_us = (uint32_t)strtoul(argv[++i], NULL, 0);
    } else {
      fprintf(stderr, "unknown option %s\n", a);
      usage(argv[0]);
      return 2;
    }
#undef NEED_VAL
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_sigint;
  (void)sigaction(SIGINT, &sa, NULL);
  (void)sigaction(SIGTERM, &sa, NULL);

  if (hml_transport_register(&g_xport_ops) != HML_OK) {
    fprintf(stderr, "transport m4-udp registration failed\n");
    return 1;
  }
  if (serve) return run_serve(ctrl_port, data_port, disc_port, client_port, frames, pace_us, beacon);
  return run_lifecycle(ctrl_port, data_port, disc_port, client_port, frames, pace_us);
}
