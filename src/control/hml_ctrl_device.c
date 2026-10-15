/*
 * hml_ctrl_device - device-side control plane demo (M2, local mode).
 *
 * Runs the control server (hml_control) plus the discovery beacon, and answers
 * the host CLI hml_ctrl_host. With --frames N it also streams N synthetic
 * 64-byte counter datagrams to the destination configured through SET_DEST
 * after a START, so the host can observe the data path.
 */

#include "hml_control.h"

#include "hml/hml_sock.h"
#include "hml/hml_transport.h"
#include "transports/rocev2/hml_roce_priv.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEV_REGS 64u
#define DEV_FRAME_BYTES 64u

typedef struct {
  hml_control_t *ctl;
  hml_sock_t *tx;
  uint16_t data_port;
  uint32_t frames_total; /* 0 = unlimited */
  uint32_t frames_sent;
  uint32_t pace_us;

  hml_dest_cfg dest;
  bool have_dest;
  bool streaming;
  uint32_t starts;
  uint32_t stops;
  uint32_t pokes;

  hml_xport_t roce_xport;
  bool roce_opened;
  uint8_t qp_type; /* 0 = UC, 1 = RC */
  bool use_imm;

  uint32_t regs[DEV_REGS];
} dev_t;

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig) {
  (void)sig;
  g_stop = 1;
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [options]\n"
          "  --ctrl-port N     control plane port (default 55102)\n"
          "  --disc-port N     beacon port, 0 disables (default 55100)\n"
          "  --beacon          enable beacon (default)\n"
          "  --no-beacon       disable beacon\n"
          "  --beacon-ms N     beacon period in ms (default 1000)\n"
          "  --reply-ip A.B.C.D  where responses are sent (default 127.0.0.1)\n"
          "  --reply-port N    client port that receives responses (default 45599)\n"
          "  --uuid N          device uuid (default 1)\n"
          "  --caps N          HML_CAP_* bitmask (default 0x11 = UDP|MCU)\n"
          "  --fw N            firmware version hex (default 0x00010000)\n"
          "  --frames N        synthetic datagrams to send after START, 0 = forever\n"
          "  --data-port N     fallback data port when no dest was set (default 55101)\n"
          "  --qp-type uc|rc   RoCEv2 QP transport service (default rc)\n"
          "  --imm             enable RDMA Write with Immediate signaling\n"
          "  --pace-us N       inter-frame delay in us (default 2000)\n",
          argv0);
}

static bool parse_u32(const char *s, uint32_t *out) {
  if (!s || !*s) return false;
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 0);
  if (!end || *end != '\0' || v > 0xffffffffu) return false;
  *out = (uint32_t)v;
  return true;
}

/* Dotted quad to host byte order (hml_sock_* takes host order). */
static bool ip4_parse(const char *s, uint32_t *out) {
  uint32_t v = 0;
  int i = 0;
  const char *p = s;
  for (i = 0; i < 4; i++) {
    if (*p < '0' || *p > '9') return false;
    uint32_t o = 0;
    while (*p >= '0' && *p <= '9') {
      o = o * 10u + (uint32_t)(*p - '0');
      if (o > 255u) return false;
      p++;
    }
    v = (v << 8) | o;
    if (i < 3) {
      if (*p != '.') return false;
      p++;
    }
  }
  if (*p != '\0') return false;
  *out = v;
  return true;
}

/* ---- device callbacks -------------------------------------------------- */

static void tx_ensure(dev_t *d) {
  if (d->have_dest && d->dest.kind == HML_XPORT_ROCEV2) {
    if (!d->roce_opened) {
      d->roce_xport.ops = hml_rocev2_ops();
      if (d->roce_xport.ops && d->roce_xport.ops->open) {
        if (d->roce_xport.ops->open(&d->roce_xport, &d->dest, 1) == HML_OK) {
          d->roce_opened = true;
          if (d->qp_type == 1) {
            (void)hml_roce_set_qp_type(&d->roce_xport, 1);
          }
          if (d->use_imm) {
            (void)hml_roce_set_imm_mode(&d->roce_xport, true, 0);
          }
        }
      }
    }
    return;
  }
  if (d->tx) return;
  uint32_t ip = d->have_dest ? d->dest.ipv4 : HML_IP_LOOPBACK;
  uint16_t port = d->have_dest ? d->dest.udp_port : d->data_port;
  /* hml_dest_cfg.ipv4 is network byte order, hml_sock_set_dest wants host order */
  if (hml_sock_udp_create(&d->tx, HML_IP_ANY, 0, false) != HML_OK) return;
  (void)hml_sock_set_dest(d->tx, ip, port);
}

static void on_set_dest(void *ctx, const hml_dest_cfg *cfg) {
  dev_t *d = (dev_t *)ctx;
  if (d->roce_opened && d->roce_xport.ops && d->roce_xport.ops->close) {
    d->roce_xport.ops->close(&d->roce_xport);
    d->roce_opened = false;
  }
  d->dest = *cfg;
  d->have_dest = true;
  if (d->dest.kind == HML_XPORT_UDP && d->tx) {
    (void)hml_sock_set_dest(d->tx, d->dest.ipv4, d->dest.udp_port);
  }
  printf("device dest set kind=%u ipv4=%u.%u.%u.%u port=%u frame_size=%u\n", (unsigned)cfg->kind,
         (unsigned)((cfg->ipv4 >> 24) & 0xffu), (unsigned)((cfg->ipv4 >> 16) & 0xffu),
         (unsigned)((cfg->ipv4 >> 8) & 0xffu), (unsigned)(cfg->ipv4 & 0xffu),
         (unsigned)cfg->udp_port, (unsigned)cfg->frame_size);
  fflush(stdout);
}

static void on_start(void *ctx) {
  dev_t *d = (dev_t *)ctx;
  d->starts++;
  d->streaming = true;
  tx_ensure(d);
  printf("device start #%u\n", d->starts);
  fflush(stdout);
}

static void on_stop(void *ctx) {
  dev_t *d = (dev_t *)ctx;
  d->stops++;
  d->streaming = false;
  if (d->roce_opened && d->roce_xport.ops && d->roce_xport.ops->close) {
    d->roce_xport.ops->close(&d->roce_xport);
    d->roce_opened = false;
  }
  printf("device stop #%u\n", d->stops);
  fflush(stdout);
}

static uint32_t on_poke(void *ctx, uint32_t addr, uint32_t val, bool *ok) {
  dev_t *d = (dev_t *)ctx;
  if (addr >= DEV_REGS) {
    *ok = false;
    return 0;
  }
  d->regs[addr] = val;
  d->pokes++;
  *ok = true;
  return d->regs[addr];
}

static uint32_t on_read(void *ctx, uint32_t addr, bool *ok) {
  dev_t *d = (dev_t *)ctx;
  if (addr >= DEV_REGS) {
    *ok = false;
    return 0;
  }
  *ok = true;
  return d->regs[addr];
}

static void sleep_us(uint32_t us) {
  struct timespec ts;
  ts.tv_sec = (time_t)(us / 1000000u);
  ts.tv_nsec = (long)(us % 1000000u) * 1000L;
  (void)nanosleep(&ts, NULL);
}

/* 64-byte synthetic payload: counter u32 BE then an incrementing pattern. */
static void stream_one(dev_t *d) {
  uint8_t buf[DEV_FRAME_BYTES];
  hml_put_be32(buf, d->frames_sent);
  for (unsigned i = 4; i < DEV_FRAME_BYTES; i++) buf[i] = (uint8_t)(d->frames_sent + i);
  tx_ensure(d);
  if (d->have_dest && d->dest.kind == HML_XPORT_ROCEV2) {
    if (!d->roce_opened) return;
    hml_frame_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.flags = HML_FLAG_START | HML_FLAG_END;
    hdr.stream_id = 1;
    hdr.seq = d->frames_sent;
    hdr.payload_len = DEV_FRAME_BYTES;
    if (d->roce_xport.ops->send_frag(&d->roce_xport, &hdr, buf, sizeof(buf)) == HML_OK) {
      d->frames_sent++;
    }
    return;
  }
  if (!d->tx) return;
  if (hml_sock_send(d->tx, buf, sizeof(buf)) == HML_OK) d->frames_sent++;
}

int main(int argc, char **argv) {
  dev_t d;
  memset(&d, 0, sizeof(d));
  d.data_port = (uint16_t)HML_DATA_PORT;
  d.pace_us = 2000;
  d.qp_type = 1; /* default: RC */

  hml_control_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.ctrl_port = (uint16_t)HML_CTRL_PORT;
  cfg.disc_port = (uint16_t)HML_DISC_PORT;
  cfg.beacon_enable = true;
  cfg.beacon_period_ms = HML_CTRL_DEFAULT_BEACON_PERIOD_MS;
  cfg.uuid = 1;
  cfg.caps = HML_CAP_UDP | HML_CAP_MCU;
  cfg.fw_ver = 0x00010000u;
  cfg.reply_ip = HML_IP_LOOPBACK;
  cfg.reply_port = 45599;

  uint32_t reply_ip = HML_IP_LOOPBACK;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    bool has_val = (i + 1) < argc;
#define NEED_VAL()                                    \
  do {                                                \
    if (!has_val) {                                   \
      fprintf(stderr, "missing value for %s\n", a);   \
      return 2;                                       \
    }                                                 \
  } while (0)
    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
      usage(argv[0]);
      return 0;
    } else if (strcmp(a, "--no-beacon") == 0) {
      cfg.beacon_enable = false;
    } else if (strcmp(a, "--beacon") == 0) {
      cfg.beacon_enable = true;
    } else if (strcmp(a, "--ctrl-port") == 0) {
      NEED_VAL();
      cfg.ctrl_port = (uint16_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--disc-port") == 0) {
      NEED_VAL();
      cfg.disc_port = (uint16_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--beacon-ms") == 0) {
      NEED_VAL();
      if (!parse_u32(argv[++i], &cfg.beacon_period_ms)) {
        fprintf(stderr, "bad --beacon-ms\n");
        return 2;
      }
    } else if (strcmp(a, "--reply-ip") == 0) {
      NEED_VAL();
      if (!ip4_parse(argv[++i], &reply_ip)) {
        fprintf(stderr, "bad --reply-ip\n");
        return 2;
      }
    } else if (strcmp(a, "--reply-port") == 0) {
      NEED_VAL();
      cfg.reply_port = (uint16_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--uuid") == 0) {
      NEED_VAL();
      if (!parse_u32(argv[++i], &cfg.uuid)) return 2;
    } else if (strcmp(a, "--caps") == 0) {
      NEED_VAL();
      if (!parse_u32(argv[++i], &cfg.caps)) return 2;
    } else if (strcmp(a, "--fw") == 0) {
      NEED_VAL();
      if (!parse_u32(argv[++i], &cfg.fw_ver)) return 2;
    } else if (strcmp(a, "--frames") == 0) {
      NEED_VAL();
      if (!parse_u32(argv[++i], &d.frames_total)) return 2;
    } else if (strcmp(a, "--data-port") == 0) {
      NEED_VAL();
      d.data_port = (uint16_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--pace-us") == 0) {
      NEED_VAL();
      if (!parse_u32(argv[++i], &d.pace_us)) return 2;
    } else if (strcmp(a, "--qp-type") == 0) {
      NEED_VAL();
      const char *qt = argv[++i];
      if (strcmp(qt, "rc") == 0 || strcmp(qt, "RC") == 0) {
        d.qp_type = 1;
      } else {
        d.qp_type = 0;
      }
    } else if (strcmp(a, "--imm") == 0) {
      d.use_imm = true;
    } else {
      fprintf(stderr, "unknown option %s\n", a);
      usage(argv[0]);
      return 2;
    }
#undef NEED_VAL
  }
  cfg.reply_ip = reply_ip;

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_sigint;
  (void)sigaction(SIGINT, &sa, NULL);
  (void)sigaction(SIGTERM, &sa, NULL);

  hml_status st = hml_control_open(&d.ctl, &cfg);
  if (st != HML_OK) {
    fprintf(stderr, "hml_control_open failed: %d\n", (int)st);
    return 1;
  }
  (void)hml_control_set_cbs(d.ctl, on_set_dest, on_start, on_stop, on_poke, &d);
  (void)hml_control_set_read_cb(d.ctl, on_read, &d);

  printf("device up ctrl=%u disc=%u reply=%u.%u.%u.%u:%u beacon=%s period=%ums uuid=%u caps=0x%02x fw=0x%08x\n",
         (unsigned)cfg.ctrl_port, (unsigned)cfg.disc_port,
         (unsigned)((reply_ip >> 24) & 0xffu), (unsigned)((reply_ip >> 16) & 0xffu),
         (unsigned)((reply_ip >> 8) & 0xffu), (unsigned)(reply_ip & 0xffu),
         (unsigned)cfg.reply_port, cfg.beacon_enable ? "on" : "off",
         (unsigned)cfg.beacon_period_ms, cfg.uuid, cfg.caps, cfg.fw_ver);
  printf("device streaming data to %s:%u, frames=%s\n",
         d.have_dest ? "configured" : "fallback", (unsigned)d.data_port,
         d.frames_total ? "limited" : "unlimited");
  fflush(stdout);

  int rc = 0;
  while (g_stop == 0) {
    uint32_t rx0 = hml_control_rx_count(d.ctl);
    uint32_t bad0 = hml_control_bad_count(d.ctl);
    /* poll often while streaming so --pace-us is roughly honoured */
    hml_status ps = hml_control_poll(d.ctl, d.streaming ? 1 : 20);
    if (ps == HML_ERR_IO) {
      fprintf(stderr, "control poll io error\n");
      rc = 1;
      break;
    }
    if (ps == HML_OK && hml_control_rx_count(d.ctl) > rx0) {
      if (hml_control_bad_count(d.ctl) > bad0) {
        printf("control dropped malformed datagram (bad=%u)\n",
               (unsigned)hml_control_bad_count(d.ctl));
      } else {
        uint8_t op = 0;
        uint16_t flags = 0;
        uint16_t len = 0;
        if (hml_control_last_req(d.ctl, &op, &flags, &len))
          printf("control op=0x%02x flags=0x%04x len=%u\n", (unsigned)op, (unsigned)flags,
                 (unsigned)len);
      }
      fflush(stdout);
    }
    if (d.streaming && (d.frames_total == 0 || d.frames_sent < d.frames_total)) {
      stream_one(&d);
      if (d.frames_sent == d.frames_total) {
        printf("device frames done sent=%u\n", d.frames_sent);
        fflush(stdout);
      }
      if (d.pace_us) sleep_us(d.pace_us);
    }
  }

  printf("device shutting down: rx=%u bad=%u starts=%u stops=%u pokes=%u frames=%u\n",
         (unsigned)hml_control_rx_count(d.ctl), (unsigned)hml_control_bad_count(d.ctl),
         d.starts, d.stops, d.pokes, d.frames_sent);
  fflush(stdout);

  if (d.tx) hml_sock_close(d.tx);
  if (d.roce_opened && d.roce_xport.ops && d.roce_xport.ops->close) {
    d.roce_xport.ops->close(&d.roce_xport);
    d.roce_opened = false;
  }
  hml_control_close(d.ctl);
  return rc;
}
