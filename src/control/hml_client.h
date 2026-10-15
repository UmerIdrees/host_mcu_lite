#ifndef HML_CLIENT_H
#define HML_CLIENT_H

/*
 * Private host-side control client. One socket bound to local_port carries the
 * request/response traffic, a second socket bound to the peer disc_port
 * receives beacons (hml_client_set_peer creates it).
 *
 * Every request is answered within hml_client_set_timeout (default 1000 ms);
 * stray or malformed datagrams are ignored while the deadline holds.
 */

#include "hml/hml_ctrl.h"
#include "hml/hml_discovery.h"
#include "hml/hml_protocol.h"

#include "hml_control.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hml_client hml_client_t;

hml_status hml_client_open(hml_client_t **out, uint16_t local_port);
hml_status hml_client_set_peer(hml_client_t *c, uint32_t ip, uint16_t ctrl_port, uint16_t disc_port);
hml_status hml_client_set_timeout(hml_client_t *c, uint32_t timeout_ms);
hml_status hml_client_discover(hml_client_t *c, int timeout_ms, uint32_t *uuid_out,
                               uint32_t *caps_out, uint32_t *fw_out, uint64_t *ts_out);
hml_status hml_client_set_dest(hml_client_t *c, const hml_dest_cfg *cfg);
hml_status hml_client_set_dest_roce(hml_client_t *c, uint32_t ip, uint16_t udp_port,
                                    uint32_t qp, uint32_t rkey, uint64_t remote_addr,
                                    uint32_t frame_size, uint16_t window_len, uint32_t psn);
hml_status hml_client_get_dest(hml_client_t *c, hml_dest_cfg *out);
hml_status hml_client_start(hml_client_t *c);
hml_status hml_client_stop(hml_client_t *c);
hml_status hml_client_reset(hml_client_t *c);
hml_status hml_client_ping(hml_client_t *c, uint32_t token, uint32_t *token_out);
hml_status hml_client_get_caps(hml_client_t *c, uint32_t *uuid, uint32_t *caps, uint32_t *fw);
hml_status hml_client_status(hml_client_t *c, bool *streaming);
hml_status hml_client_poke(hml_client_t *c, uint32_t addr, uint32_t val, uint32_t *rval);
hml_status hml_client_readback(hml_client_t *c, uint32_t addr, uint32_t *val);
/* hml_status byte carried by an HML_CTRL_FLAG_ERR response, if any. */
hml_status hml_client_last_device_status(const hml_client_t *c);
void hml_client_close(hml_client_t *c);

#ifdef __cplusplus
}
#endif

#endif /* HML_CLIENT_H */
