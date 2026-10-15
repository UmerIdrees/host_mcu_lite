#include "hml_client.h"

#include "hml/hml_port.h"
#include "hml/hml_sock.h"

#include <stdlib.h>
#include <string.h>

/*
 * Host-side client. Request/response matching rules:
 *   - a datagram must parse (magic/version/len/CRC) and carry HML_CTRL_FLAG_RESP
 *   - op must equal the request op, or be HML_OP_ERROR (the device answers an
 *     unknown opcode with HML_OP_ERROR + HML_CTRL_FLAG_ERR)
 *   - HML_CTRL_FLAG_ERR maps to HML_ERR_NOTSUP, the 1-byte hml_status in the
 *     payload (when present) is kept in hml_client_last_device_status
 *   - deadline reached without a match is HML_ERR_TIMEOUT
 * Anything else received in the meantime is discarded and the wait continues.
 */

struct hml_client {
  hml_sock_t *ctrl;
  hml_sock_t *disc;
  uint32_t peer_ip;
  uint16_t peer_ctrl_port;
  uint16_t peer_disc_port;
  uint32_t timeout_ms;
  bool peer_set;
  hml_status last_dev_status;
  uint8_t rx[HML_CTRL_MAX_MSG];
  uint8_t tx[HML_CTRL_MAX_MSG];
};

hml_status hml_client_open(hml_client_t **out, uint16_t local_port) {
  if (!out) return HML_ERR_ARG;
  *out = NULL;
  hml_client_t *c = (hml_client_t *)calloc(1, sizeof(*c));
  if (!c) return HML_ERR_NOMEM;
  c->timeout_ms = HML_CTRL_DEFAULT_TIMEOUT_MS;
  hml_status st = hml_sock_udp_create(&c->ctrl, HML_IP_ANY, local_port, true);
  if (st != HML_OK) {
    free(c);
    return st;
  }
  *out = c;
  return HML_OK;
}

hml_status hml_client_set_peer(hml_client_t *c, uint32_t ip, uint16_t ctrl_port,
                               uint16_t disc_port) {
  if (!c) return HML_ERR_ARG;
  hml_status st = hml_sock_set_dest(c->ctrl, ip, ctrl_port);
  if (st != HML_OK) return st;
  c->peer_ip = ip;
  c->peer_ctrl_port = ctrl_port;
  c->peer_disc_port = disc_port;
  c->peer_set = true;

  if (c->disc) {
    hml_sock_close(c->disc);
    c->disc = NULL;
  }
  if (disc_port != 0) {
    st = hml_sock_udp_create(&c->disc, HML_IP_ANY, disc_port, true);
    if (st != HML_OK) return st;
  }
  return HML_OK;
}

hml_status hml_client_set_timeout(hml_client_t *c, uint32_t timeout_ms) {
  if (!c) return HML_ERR_ARG;
  c->timeout_ms = timeout_ms;
  return HML_OK;
}

hml_status hml_client_last_device_status(const hml_client_t *c) {
  return c ? c->last_dev_status : HML_ERR_ARG;
}

void hml_client_close(hml_client_t *c) {
  if (!c) return;
  if (c->disc) hml_sock_close(c->disc);
  hml_sock_close(c->ctrl);
  free(c);
}

/* ---- request / response ------------------------------------------------ */

static hml_status xchg(hml_client_t *c, uint8_t op, const uint8_t *payload, uint16_t plen,
                        uint8_t *resp, uint16_t resp_cap, uint16_t *resp_len) {
  if (!c || !c->peer_set) return HML_ERR_ARG;
  if (plen > HML_CTRL_MAX_PAYLOAD) return HML_ERR_TOOLARGE;
  if (plen > 0 && !payload) return HML_ERR_ARG;

  hml_ctrl_msg req;
  req.op = op;
  req.flags = HML_CTRL_FLAG_REQ;
  req.payload = payload;
  req.payload_len = plen;
  uint16_t n = 0;
  hml_ctrl_fill(&req, c->tx, (uint16_t)sizeof(c->tx), &n);
  if (n == 0) return HML_ERR_ARG;
  hml_status st = hml_sock_send(c->ctrl, c->tx, n);
  if (st != HML_OK) return st;

  uint64_t t0 = hml_clock_us();
  uint64_t budget = (uint64_t)c->timeout_ms * 1000ull;
  for (;;) {
    uint64_t elapsed = hml_clock_us() - t0;
    if (elapsed >= budget) return HML_ERR_TIMEOUT;
    int wait = (int)((budget - elapsed) / 1000ull);
    if (wait <= 0) wait = 1;

    size_t got = 0;
    st = hml_sock_recv(c->ctrl, c->rx, sizeof(c->rx), &got, wait);
    if (st == HML_ERR_TIMEOUT || st == HML_ERR_AGAIN) continue;
    if (st != HML_OK) return st;

    hml_ctrl_msg m;
    const uint8_t *pl = NULL;
    if (hml_ctrl_parse(c->rx, got, &m, &pl) != HML_OK) continue;   /* stray */
    if ((m.flags & HML_CTRL_FLAG_RESP) == 0) continue;              /* not a response */
    if (m.op != op && m.op != HML_OP_ERROR) continue;               /* stale response */

    if (m.flags & HML_CTRL_FLAG_ERR) {
      c->last_dev_status = HML_ERR_NOTSUP;
      if (m.payload_len >= 1 && m.payload) c->last_dev_status = (hml_status)pl[0];
      return HML_ERR_NOTSUP;
    }
    if ((m.flags & HML_CTRL_FLAG_ACK) == 0) {
      return HML_ERR_PROTO;
    }
    c->last_dev_status = HML_OK;
    if (resp && resp_len) {
      if (m.payload_len > resp_cap) return HML_ERR_TOOLARGE;
      if (m.payload_len > 0) memcpy(resp, pl, m.payload_len);
      *resp_len = m.payload_len;
    }
    return HML_OK;
  }
}

static hml_status simple_op(hml_client_t *c, uint8_t op) {
  return xchg(c, op, NULL, 0, NULL, 0, NULL);
}

/* ---- operations -------------------------------------------------------- */

hml_status hml_client_discover(hml_client_t *c, int timeout_ms, uint32_t *uuid_out,
                               uint32_t *caps_out, uint32_t *fw_out, uint64_t *ts_out) {
  if (!c || !c->peer_set) return HML_ERR_ARG;
  if (!c->disc) return HML_ERR_NOTSUP;
  if (timeout_ms < 0) timeout_ms = (int)c->timeout_ms;

  uint64_t t0 = hml_clock_us();
  uint64_t budget = (uint64_t)timeout_ms * 1000ull;
  for (;;) {
    uint64_t elapsed = hml_clock_us() - t0;
    if (elapsed >= budget) return HML_ERR_TIMEOUT;
    int wait = (int)((budget - elapsed) / 1000ull);
    if (wait <= 0) wait = 1;

    size_t got = 0;
    hml_status st = hml_sock_recv(c->disc, c->rx, sizeof(c->rx), &got, wait);
    if (st == HML_ERR_TIMEOUT || st == HML_ERR_AGAIN) continue;
    if (st != HML_OK) return st;

    hml_beacon b;
    if (hml_beacon_parse(c->rx, got, &b) != HML_OK) continue; /* not a beacon */
    if (b.version != HML_DISC_VERSION) continue;
    if (uuid_out) *uuid_out = b.uuid;
    if (caps_out) *caps_out = b.caps;
    if (fw_out) *fw_out = b.fw_ver;
    if (ts_out) *ts_out = b.ts_us;
    return HML_OK;
  }
}

hml_status hml_client_set_dest(hml_client_t *c, const hml_dest_cfg *cfg) {
  if (!c || !cfg) return HML_ERR_ARG;
  uint8_t buf[HML_CFG_SIZE];
  hml_proto_cfg_fill(cfg, buf);
  return xchg(c, HML_OP_SET_DEST, buf, (uint16_t)HML_CFG_SIZE, NULL, 0, NULL);
}

hml_status hml_client_set_dest_roce(hml_client_t *c, uint32_t ip, uint16_t udp_port,
                                    uint32_t qp, uint32_t rkey, uint64_t remote_addr,
                                    uint32_t frame_size, uint16_t window_len, uint32_t psn) {
  if (!c) return HML_ERR_ARG;
  hml_dest_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.kind = HML_XPORT_ROCEV2;
  cfg.ipv4 = ip;
  cfg.udp_port = udp_port ? udp_port : (uint16_t)HML_ROCE_PORT;
  cfg.qp = qp;
  cfg.rkey = rkey;
  cfg.remote_addr = remote_addr;
  cfg.frame_size = frame_size;
  cfg.window_len = window_len;
  cfg.psn = psn;
  cfg.hop_limit = 64;
  return hml_client_set_dest(c, &cfg);
}

hml_status hml_client_get_dest(hml_client_t *c, hml_dest_cfg *out) {
  if (!c || !out) return HML_ERR_ARG;
  uint8_t buf[HML_CFG_SIZE];
  uint16_t n = 0;
  hml_status st = xchg(c, HML_OP_GET_DEST, NULL, 0, buf, (uint16_t)sizeof(buf), &n);
  if (st != HML_OK) return st;
  if (n != HML_CFG_SIZE) return HML_ERR_PROTO;
  return hml_proto_cfg_parse(buf, n, out);
}

hml_status hml_client_start(hml_client_t *c) { return simple_op(c, HML_OP_START); }
hml_status hml_client_stop(hml_client_t *c) { return simple_op(c, HML_OP_STOP); }
hml_status hml_client_reset(hml_client_t *c) { return simple_op(c, HML_OP_RESET); }

hml_status hml_client_ping(hml_client_t *c, uint32_t token, uint32_t *token_out) {
  if (!c) return HML_ERR_ARG;
  uint8_t req[4];
  uint8_t resp[8];
  uint16_t n = 0;
  hml_put_be32(req, token);
  hml_status st = xchg(c, HML_OP_PING, req, 4, resp, (uint16_t)sizeof(resp), &n);
  if (st != HML_OK) return st;
  if (n != 4) return HML_ERR_PROTO;
  if (token_out) *token_out = hml_get_be32(resp);
  return HML_OK;
}

hml_status hml_client_get_caps(hml_client_t *c, uint32_t *uuid, uint32_t *caps, uint32_t *fw) {
  if (!c) return HML_ERR_ARG;
  uint8_t resp[HML_CTRL_CAPS_PAYLOAD];
  uint16_t n = 0;
  hml_status st = xchg(c, HML_OP_GET_CAPS, NULL, 0, resp, (uint16_t)sizeof(resp), &n);
  if (st != HML_OK) return st;
  if (n != HML_CTRL_CAPS_PAYLOAD) return HML_ERR_PROTO;
  if (uuid) *uuid = hml_get_be32(resp);
  if (caps) *caps = hml_get_be32(resp + 4);
  if (fw) *fw = hml_get_be32(resp + 8);
  return HML_OK;
}

hml_status hml_client_status(hml_client_t *c, bool *streaming) {
  if (!c || !streaming) return HML_ERR_ARG;
  uint8_t resp[4];
  uint16_t n = 0;
  hml_status st = xchg(c, HML_OP_STATUS, NULL, 0, resp, (uint16_t)sizeof(resp), &n);
  if (st != HML_OK) return st;
  if (n != 1) return HML_ERR_PROTO;
  *streaming = resp[0] != 0;
  return HML_OK;
}

hml_status hml_client_poke(hml_client_t *c, uint32_t addr, uint32_t val, uint32_t *rval) {
  if (!c) return HML_ERR_ARG;
  uint8_t req[8];
  uint8_t resp[8];
  uint16_t n = 0;
  hml_put_be32(req, addr);
  hml_put_be32(req + 4, val);
  hml_status st = xchg(c, HML_OP_POKE, req, 8, resp, (uint16_t)sizeof(resp), &n);
  if (st != HML_OK) return st;
  if (n != 8) return HML_ERR_PROTO;
  if (rval) *rval = hml_get_be32(resp + 4);
  return HML_OK;
}

hml_status hml_client_readback(hml_client_t *c, uint32_t addr, uint32_t *val) {
  if (!c) return HML_ERR_ARG;
  uint8_t req[4];
  uint8_t resp[8];
  uint16_t n = 0;
  hml_put_be32(req, addr);
  hml_status st = xchg(c, HML_OP_READBACK, req, 4, resp, (uint16_t)sizeof(resp), &n);
  if (st != HML_OK) return st;
  if (n != 8) return HML_ERR_PROTO;
  if (val) *val = hml_get_be32(resp + 4);
  return HML_OK;
}
