#ifndef HML_BRIDGE_CONTROL_H
#define HML_BRIDGE_CONTROL_H

#include "hml_control.h" /* src/control: hml_control_t + the callback typedefs */

#include "hml_bridge.h"
#include "hml_bridge_stub.h"

/*
 * Glue that makes the bridge register map reachable through the frozen control
 * plane (HML_OP_POKE / HML_OP_READBACK from include/hml/hml_ctrl.h).
 *
 * A device wires this up once, with the hml_bridge_t * as the callback context:
 *
 *   hml_control_t *c;
 *   hml_control_cfg cfg = { .ctrl_port = ..., .reply_ip = HML_IP_LOOPBACK,
 *                           .reply_port = <client port>, .caps = HML_CAP_BRIDGE, ... };
 *   hml_control_open(&c, &cfg);
 *   hml_bridge_stub_hml_device(&bridge);          // or your own peripheral set
 *   hml_bridge_control_wire(c, bridge);
 *
 * hml_bridge_control_wire() is exactly:
 *
 *   hml_control_set_cbs(c, hml_bridge_control_set_dest_cb,
 *                       hml_bridge_control_start_cb,
 *                       hml_bridge_control_stop_cb,
 *                       hml_bridge_control_poke_cb(), bridge);
 *   hml_control_set_read_cb(c, hml_bridge_control_read_cb(), bridge);
 *
 * What each callback does:
 *   POKE      -> hml_bridge_poke32(): write the 4 bytes little-endian and read
 *                the register back. *ok is false on ANY error (unknown or
 *                straddling address, packetizer write before CONFIG, peripheral
 *                fault such as an I2C NAK), so the device answers
 *                HML_OP_POKE with HML_CTRL_FLAG_ERR and the host client sees
 *                HML_ERR_NOTSUP. There is no path that reports success for a
 *                write that did not happen.
 *   READBACK  -> hml_bridge_read32(), same *ok rule.
 *   SET_DEST  -> advances the bridge to CONFIG (from INIT) and programs the
 *                packetizer destination words, i.e. the HSB "program the
 *                packetizer dest" step.
 *   START     -> hml_bridge_set_streaming(true): enters STREAM, refused with
 *                HML_ERR_STATE while the device is still in INIT so the data
 *                plane can never run against an unprogrammed packetizer.
 *   STOP      -> hml_bridge_set_streaming(false): back to CONFIG, configuration
 *                retained.
 *
 * KNOWN LIMITATION of the frozen control API: SET_DEST / START / STOP
 * callbacks return void, so a device cannot answer them with an error status -
 * the server ACKs them unconditionally. The bridge therefore enforces the
 * ordering itself (phase rules in hml_bridge.h) and the failure is reported in
 * the device log; a device that must reject a START should check the phase
 * before enabling its transport and log it.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Factories: the returned callbacks take the hml_bridge_t * as their ctx. */
hml_ctrl_poke_cb hml_bridge_control_poke_cb(void);
hml_ctrl_read_cb hml_bridge_control_read_cb(void);

void hml_bridge_control_set_dest_cb(void *ctx, const hml_dest_cfg *cfg);
void hml_bridge_control_start_cb(void *ctx);
void hml_bridge_control_stop_cb(void *ctx);

hml_status hml_bridge_control_wire(hml_control_t *c, hml_bridge_t *b);

#ifdef __cplusplus
}
#endif

#endif /* HML_BRIDGE_CONTROL_H */
