#ifndef HML_CONTROL_H
#define HML_CONTROL_H

/*
 * Private (device-side) control server + host-side client for the hml control
 * plane. Includes the frozen wire headers, adds only the server/client state.
 *
 * REPLY ADDRESS: hml_sock_recv() does not expose the peer address, so the
 * server cannot learn where to answer. Responses therefore always go to the
 * address configured in hml_control_cfg (reply_ip / reply_port, host byte order
 * ip / network order port, both settable at runtime with hml_control_set_reply).
 * A client must bind a known port and the device must be told that port.
 *
 * THREADING: not internally locked. One poller per hml_control_t.
 */

#include "hml/hml_ctrl.h"
#include "hml/hml_discovery.h"
#include "hml/hml_protocol.h"

/* Fixed control header size, see hml_ctrl_codec.c. */
#define HML_CTRL_HDR_SIZE 12u
/* Largest control datagram: header + max payload + trailing crc. */
#define HML_CTRL_MAX_MSG (HML_CTRL_HDR_SIZE + HML_CTRL_MAX_PAYLOAD + 2u)
#define HML_CTRL_DEFAULT_BEACON_PERIOD_MS 1000u
#define HML_CTRL_MIN_BEACON_PERIOD_MS 10u
#define HML_CTRL_DEFAULT_TIMEOUT_MS 1000u
#define HML_CTRL_CAPS_PAYLOAD 12u /* uuid u32 BE | caps u32 BE | fw_ver u32 BE */

#define HML_IP_ANY      0x00000000u
#define HML_IP_LOOPBACK 0x7f000001u
#define HML_IP_BROADCAST 0xffffffffu

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hml_control hml_control_t;

typedef void (*hml_ctrl_set_dest_cb)(void *ctx, const hml_dest_cfg *cfg);
typedef void (*hml_ctrl_start_cb)(void *ctx);
typedef void (*hml_ctrl_stop_cb)(void *ctx);
/* Write val to addr; on return *ok tells the server whether it succeeded and
 * the return value is the read-back value reported in the response. */
typedef uint32_t (*hml_ctrl_poke_cb)(void *ctx, uint32_t addr, uint32_t val, bool *ok);
/* Read addr; on return *ok tells the server whether it succeeded. */
typedef uint32_t (*hml_ctrl_read_cb)(void *ctx, uint32_t addr, bool *ok);

typedef struct hml_control_cfg {
  uint16_t ctrl_port;          /* control plane UDP port, must be != 0 */
  uint16_t disc_port;          /* beacon destination port, 0 disables beacon */
  bool beacon_enable;          /* send one beacon at open, then every period */
  uint32_t beacon_period_ms;   /* 0 => HML_CTRL_DEFAULT_BEACON_PERIOD_MS */
  uint32_t uuid;               /* reported by HML_OP_GET_CAPS and the beacon */
  uint32_t caps;               /* HML_CAP_* bitmask */
  uint32_t fw_ver;             /* reported by HML_OP_GET_CAPS and the beacon */
  uint32_t reply_ip;           /* response destination, host byte order */
  uint16_t reply_port;         /* 0 => responses are built but not sent */
} hml_control_cfg;

hml_status hml_control_open(hml_control_t **out, const hml_control_cfg *cfg);
hml_status hml_control_set_cbs(hml_control_t *c, hml_ctrl_set_dest_cb on_set_dest,
                               hml_ctrl_start_cb on_start, hml_ctrl_stop_cb on_stop,
                               hml_ctrl_poke_cb on_poke, void *ctx);
hml_status hml_control_set_read_cb(hml_control_t *c, hml_ctrl_read_cb on_read, void *ctx);
hml_status hml_control_set_beacon(hml_control_t *c, bool enable);
hml_status hml_control_set_reply(hml_control_t *c, uint32_t ip, uint16_t port);
/* timeout_ms < 0: block until one control datagram arrives. >= 0: serve at most
 * one datagram; HML_ERR_AGAIN when the budget expired with nothing received.
 * Beacons are emitted while waiting. */
hml_status hml_control_poll(hml_control_t *c, int timeout_ms);
bool hml_control_get_dest(const hml_control_t *c, hml_dest_cfg *out);
void hml_control_close(hml_control_t *c);

/* Introspection for CLIs / tests. */
bool hml_control_streaming(const hml_control_t *c);
uint32_t hml_control_bad_count(const hml_control_t *c);
uint32_t hml_control_rx_count(const hml_control_t *c);
/* Details of the datagram served by the last hml_control_poll, if it parsed. */
bool hml_control_last_req(const hml_control_t *c, uint8_t *op, uint16_t *flags,
                          uint16_t *payload_len);

#ifdef __cplusplus
}
#endif

#endif /* HML_CONTROL_H */
