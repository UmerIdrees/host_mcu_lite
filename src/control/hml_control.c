#include "hml_control.h"

#include "hml/hml_sock.h"

#include <stdlib.h>
#include <string.h>

/*
 * Device-side control server.
 *
 * Two UDP sockets: ctrl_sock bound to 0.0.0.0:ctrl_port serving requests, and
 * beacon_sock (SO_BROADCAST) broadcasting the beacon to 255.255.255.255:disc_port.
 * Responses go to the configured reply address (see hml_control.h), because
 * hml_sock_recv() hides the peer address.
 *
 * Request handling (hml_ctrl.h opcodes):
 *   PING      -> resp PING, same payload
 *   SET_DEST  -> req payload hml_dest_cfg (HML_CFG_SIZE bytes); store + on_set_dest
 *   GET_DEST  -> resp payload hml_dest_cfg
 *   START     -> on_start (one call per START command, must be idempotent), resp
 *   STOP      -> on_stop (one call per STOP command), resp
 *   RESET     -> clear dest + on_stop, resp
 *   GET_CAPS  -> resp payload uuid u32 BE | caps u32 BE | fw_ver u32 BE (12 bytes)
 *   STATUS    -> resp payload 1 byte: 0 stopped, 1 streaming
 *   POKE      -> req payload addr u32 BE | val u32 BE;
 *                resp payload addr u32 BE | rval u32 BE, or FLAG_ERR + hml_status
 *   READBACK  -> req payload addr u32 BE; resp payload addr u32 BE | val u32 BE
 *   NOP       -> resp ACK, empty payload
 *   unknown   -> resp op HML_OP_ERROR, FLAG_ERR, payload 1 byte = HML_ERR_NOTSUP
 * Every response carries HML_CTRL_FLAG_RESP plus HML_CTRL_FLAG_ACK or
 * HML_CTRL_FLAG_ERR. Malformed / short / bad-CRC datagrams are dropped silently
 * (no response, no state change) and counted in bad_count.
 */

struct hml_control {
  hml_sock_t *ctrl_sock;
  hml_sock_t *beacon_sock;
  hml_control_cfg cfg;

  hml_ctrl_set_dest_cb on_set_dest;
  hml_ctrl_start_cb on_start;
  hml_ctrl_stop_cb on_stop;
  hml_ctrl_poke_cb on_poke;
  hml_ctrl_read_cb on_read;
  void *cb_ctx;

  hml_dest_cfg dest;
  bool have_dest;
  bool streaming;
  bool has_reply;
  bool last_bad;

  uint64_t next_beacon_us;
  uint32_t bad_count;
  uint32_t rx_count;
  uint8_t last_op;
  uint16_t last_flags;
  uint16_t last_len;

  uint8_t tx[HML_CTRL_MAX_MSG];
  uint8_t rx[HML_CTRL_MAX_MSG];
};

static uint32_t now_ms_since(uint64_t t0_us) { return (uint32_t)((hml_clock_us() - t0_us) / 1000ull); }

hml_status hml_control_open(hml_control_t **out, const hml_control_cfg *cfg) {
  if (!out || !cfg) return HML_ERR_ARG;
  *out = NULL;
  if (cfg->ctrl_port == 0) return HML_ERR_ARG;

  hml_control_t *c = (hml_control_t *)calloc(1, sizeof(*c));
  if (!c) return HML_ERR_NOMEM;
  c->cfg = *cfg;
  if (c->cfg.beacon_period_ms < HML_CTRL_MIN_BEACON_PERIOD_MS)
    c->cfg.beacon_period_ms = HML_CTRL_DEFAULT_BEACON_PERIOD_MS;

  hml_status st = hml_sock_udp_create(&c->ctrl_sock, HML_IP_ANY, c->cfg.ctrl_port, false);
  if (st != HML_OK) {
    free(c);
    return st;
  }

  if (c->cfg.disc_port != 0) {
    st = hml_sock_udp_create(&c->beacon_sock, HML_IP_ANY, 0, true);
    if (st != HML_OK) {
      hml_sock_close(c->ctrl_sock);
      free(c);
      return st;
    }
    st = hml_sock_set_dest(c->beacon_sock, HML_IP_BROADCAST, c->cfg.disc_port);
    if (st != HML_OK) {
      hml_sock_close(c->beacon_sock);
      hml_sock_close(c->ctrl_sock);
      free(c);
      return st;
    }
  } else {
    c->cfg.beacon_enable = false;
  }

  c->has_reply = c->cfg.reply_port != 0;
  if (c->has_reply) (void)hml_sock_set_dest(c->ctrl_sock, c->cfg.reply_ip, c->cfg.reply_port);
  c->next_beacon_us = 0; /* beacon is due immediately */

  *out = c;
  return HML_OK;
}

hml_status hml_control_set_cbs(hml_control_t *c, hml_ctrl_set_dest_cb on_set_dest,
                               hml_ctrl_start_cb on_start, hml_ctrl_stop_cb on_stop,
                               hml_ctrl_poke_cb on_poke, void *ctx) {
  if (!c) return HML_ERR_ARG;
  c->on_set_dest = on_set_dest;
  c->on_start = on_start;
  c->on_stop = on_stop;
  c->on_poke = on_poke;
  c->cb_ctx = ctx;
  return HML_OK;
}

hml_status hml_control_set_read_cb(hml_control_t *c, hml_ctrl_read_cb on_read, void *ctx) {
  if (!c) return HML_ERR_ARG;
  c->on_read = on_read;
  if (on_read) c->cb_ctx = ctx;
  return HML_OK;
}

hml_status hml_control_set_beacon(hml_control_t *c, bool enable) {
  if (!c) return HML_ERR_ARG;
  if (enable && !c->beacon_sock) return HML_ERR_NOTSUP;
  c->cfg.beacon_enable = enable;
  if (enable) c->next_beacon_us = 0; /* send one right away */
  return HML_OK;
}

hml_status hml_control_set_reply(hml_control_t *c, uint32_t ip, uint16_t port) {
  if (!c) return HML_ERR_ARG;
  hml_status st = hml_sock_set_dest(c->ctrl_sock, ip, port);
  if (st != HML_OK) return st;
  c->cfg.reply_ip = ip;
  c->cfg.reply_port = port;
  c->has_reply = port != 0;
  return HML_OK;
}

bool hml_control_get_dest(const hml_control_t *c, hml_dest_cfg *out) {
  if (!c || !out) return false;
  if (!c->have_dest) {
    memset(out, 0, sizeof(*out));
    return false;
  }
  *out = c->dest;
  return true;
}

bool hml_control_streaming(const hml_control_t *c) { return c ? c->streaming : false; }
uint32_t hml_control_bad_count(const hml_control_t *c) { return c ? c->bad_count : 0u; }
uint32_t hml_control_rx_count(const hml_control_t *c) { return c ? c->rx_count : 0u; }

bool hml_control_last_req(const hml_control_t *c, uint8_t *op, uint16_t *flags,
                          uint16_t *payload_len) {
  if (!c || c->last_bad || c->rx_count == 0) return false;
  if (op) *op = c->last_op;
  if (flags) *flags = c->last_flags;
  if (payload_len) *payload_len = c->last_len;
  return true;
}

void hml_control_close(hml_control_t *c) {
  if (!c) return;
  if (c->beacon_sock) hml_sock_close(c->beacon_sock);
  hml_sock_close(c->ctrl_sock);
  free(c);
}

/* ---- beacon ------------------------------------------------------------ */

static void beacon_maybe_send(hml_control_t *c) {
  if (!c->beacon_sock || !c->cfg.beacon_enable) return;
  uint64_t now = hml_clock_us();
  if (now < c->next_beacon_us) return;

  hml_beacon b;
  b.version = HML_DISC_VERSION;
  b.flags = 0;
  b.ttl = 1;
  b.uuid = c->cfg.uuid;
  b.caps = c->cfg.caps;
  b.fw_ver = c->cfg.fw_ver;
  b.ts_us = now;
  hml_beacon_fill(&b, c->tx);
  (void)hml_sock_send(c->beacon_sock, c->tx, HML_DISC_SIZE);
  c->next_beacon_us = now + (uint64_t)c->cfg.beacon_period_ms * 1000ull;
}

/* Milliseconds until the next beacon is due, 0 when due now. */
static int beacon_wait_ms(const hml_control_t *c) {
  if (!c->beacon_sock || !c->cfg.beacon_enable) return -1;
  uint64_t now = hml_clock_us();
  if (now >= c->next_beacon_us) return 0;
  uint64_t d = (c->next_beacon_us - now + 999ull) / 1000ull;
  return d > 0x7fffffffu ? 0x7fffffff : (int)d;
}

/* ---- responses --------------------------------------------------------- */

static void send_response(hml_control_t *c, uint8_t op, bool ok, const uint8_t *payload,
                          uint16_t payload_len) {
  hml_ctrl_msg r;
  r.op = op;
  r.flags = (uint16_t)(HML_CTRL_FLAG_RESP | (ok ? HML_CTRL_FLAG_ACK : HML_CTRL_FLAG_ERR));
  r.payload = payload;
  r.payload_len = payload_len;
  uint16_t n = 0;
  hml_ctrl_fill(&r, c->tx, (uint16_t)sizeof(c->tx), &n);
  if (n == 0 || !c->has_reply) return;
  (void)hml_sock_send(c->ctrl_sock, c->tx, n);
}

static void send_status(hml_control_t *c, uint8_t op, hml_status st) {
  uint8_t b = (uint8_t)st;
  send_response(c, op, false, &b, 1);
}

/* ---- request handling -------------------------------------------------- */

static void handle_req(hml_control_t *c, const hml_ctrl_msg *m, const uint8_t *pl) {
  uint8_t scratch[HML_CTRL_MAX_PAYLOAD];

  switch (m->op) {
    case HML_OP_NOP:
      send_response(c, HML_OP_NOP, true, NULL, 0);
      break;

    case HML_OP_PING:
      send_response(c, HML_OP_PING, true, pl, m->payload_len);
      break;

    case HML_OP_SET_DEST: {
      hml_dest_cfg cfg;
      if (m->payload_len != HML_CFG_SIZE || hml_proto_cfg_parse(pl, m->payload_len, &cfg) != HML_OK) {
        send_status(c, HML_OP_SET_DEST, HML_ERR_PROTO);
        break;
      }
      c->dest = cfg;
      c->have_dest = true;
      if (c->on_set_dest) c->on_set_dest(c->cb_ctx, &cfg);
      send_response(c, HML_OP_SET_DEST, true, NULL, 0);
      break;
    }

    case HML_OP_GET_DEST: {
      if (!c->have_dest) {
        send_status(c, HML_OP_GET_DEST, HML_ERR_STATE);
        break;
      }
      hml_proto_cfg_fill(&c->dest, scratch);
      send_response(c, HML_OP_GET_DEST, true, scratch, HML_CFG_SIZE);
      break;
    }

    case HML_OP_START:
      c->streaming = true;
      if (c->on_start) c->on_start(c->cb_ctx);
      send_response(c, HML_OP_START, true, NULL, 0);
      break;

    case HML_OP_STOP:
      c->streaming = false;
      if (c->on_stop) c->on_stop(c->cb_ctx);
      send_response(c, HML_OP_STOP, true, NULL, 0);
      break;

    case HML_OP_RESET:
      c->streaming = false;
      memset(&c->dest, 0, sizeof(c->dest));
      c->have_dest = false;
      if (c->on_stop) c->on_stop(c->cb_ctx);
      send_response(c, HML_OP_RESET, true, NULL, 0);
      break;

    case HML_OP_GET_CAPS: {
      hml_put_be32(scratch + 0, c->cfg.uuid);
      hml_put_be32(scratch + 4, c->cfg.caps);
      hml_put_be32(scratch + 8, c->cfg.fw_ver);
      send_response(c, HML_OP_GET_CAPS, true, scratch, HML_CTRL_CAPS_PAYLOAD);
      break;
    }

    case HML_OP_STATUS: {
      uint8_t s = c->streaming ? 1u : 0u;
      send_response(c, HML_OP_STATUS, true, &s, 1);
      break;
    }

    case HML_OP_POKE: {
      if (m->payload_len != 8) {
        send_status(c, HML_OP_POKE, HML_ERR_PROTO);
        break;
      }
      uint32_t addr = hml_get_be32(pl);
      uint32_t val = hml_get_be32(pl + 4);
      if (!c->on_poke) {
        send_status(c, HML_OP_POKE, HML_ERR_NOTSUP);
        break;
      }
      bool ok = false;
      uint32_t rval = c->on_poke(c->cb_ctx, addr, val, &ok);
      if (!ok) {
        send_status(c, HML_OP_POKE, HML_ERR_STATE);
        break;
      }
      hml_put_be32(scratch, addr);
      hml_put_be32(scratch + 4, rval);
      send_response(c, HML_OP_POKE, true, scratch, 8);
      break;
    }

    case HML_OP_READBACK: {
      if (m->payload_len != 4) {
        send_status(c, HML_OP_READBACK, HML_ERR_PROTO);
        break;
      }
      uint32_t addr = hml_get_be32(pl);
      if (!c->on_read) {
        send_status(c, HML_OP_READBACK, HML_ERR_NOTSUP);
        break;
      }
      bool ok = false;
      uint32_t val = c->on_read(c->cb_ctx, addr, &ok);
      if (!ok) {
        send_status(c, HML_OP_READBACK, HML_ERR_STATE);
        break;
      }
      hml_put_be32(scratch, addr);
      hml_put_be32(scratch + 4, val);
      send_response(c, HML_OP_READBACK, true, scratch, 8);
      break;
    }

    default:
      /* Unknown opcode: answer HML_OP_ERROR so the client never hangs. */
      send_status(c, HML_OP_ERROR, HML_ERR_NOTSUP);
      break;
  }
}

static void serve_datagram(hml_control_t *c, size_t n) {
  c->rx_count++;
  hml_ctrl_msg m;
  const uint8_t *pl = NULL;
  hml_status st = hml_ctrl_parse(c->rx, n, &m, &pl);
  if (st != HML_OK) {
    /* malformed / short / bad CRC: silent drop, no state change */
    c->bad_count++;
    c->last_bad = true;
    return;
  }
  c->last_bad = false;
  c->last_op = m.op;
  c->last_flags = m.flags;
  c->last_len = m.payload_len;
  handle_req(c, &m, pl);
}

hml_status hml_control_poll(hml_control_t *c, int timeout_ms) {
  if (!c) return HML_ERR_ARG;
  if (timeout_ms < -1) timeout_ms = -1;

  uint64_t t0 = hml_clock_us();
  for (;;) {
    beacon_maybe_send(c);

    int wait;
    if (timeout_ms < 0) {
      wait = -1;
    } else {
      uint32_t spent = now_ms_since(t0);
      if (spent >= (uint32_t)timeout_ms) return HML_ERR_AGAIN;
      wait = timeout_ms - (int)spent;
    }
    int bw = beacon_wait_ms(c);
    if (bw >= 0 && (wait < 0 || bw < wait)) wait = bw;

    size_t n = 0;
    hml_status st = hml_sock_recv(c->ctrl_sock, c->rx, sizeof(c->rx), &n, wait);
    if (st == HML_OK) {
      serve_datagram(c, n);
      return HML_OK;
    }
    if (st != HML_ERR_TIMEOUT && st != HML_ERR_AGAIN) return st;

    /* Timed out or interrupted: the beacon may now be due, then keep waiting. */
    if (timeout_ms == 0) return HML_ERR_AGAIN;
  }
}
