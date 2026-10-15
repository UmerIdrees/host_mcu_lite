#include "hml_bridge_control.h"

#include <stdio.h>

/*
 * Control plane glue. Every path that could fail sets *ok = false, which the
 * frozen control server turns into HML_CTRL_FLAG_ERR (see hml_ctrl.h and
 * hml_control.c), so a failed bridge access is never reported to the host as a
 * success. See hml_bridge_control.h for the wiring.
 */

static void log_step(const char *what, hml_status st) {
  fprintf(stderr, "bridge[%s]: %d\n", what, (int)st);
  fflush(stderr);
}

static uint32_t poke_tramp(void *ctx, uint32_t addr, uint32_t val, bool *ok) {
  hml_bridge_t *b = (hml_bridge_t *)ctx;
  uint32_t readback = 0;
  hml_status st = hml_bridge_poke32(b, addr, val, &readback);
  if (st != HML_OK) {
    log_step("poke refused", st);
    if (ok) *ok = false;
    return 0;
  }
  if (ok) *ok = true;
  return readback;
}

static uint32_t read_tramp(void *ctx, uint32_t addr, bool *ok) {
  hml_bridge_t *b = (hml_bridge_t *)ctx;
  uint32_t val = 0;
  hml_status st = hml_bridge_read32(b, addr, &val);
  if (st != HML_OK) {
    log_step("readback refused", st);
    if (ok) *ok = false;
    return 0;
  }
  if (ok) *ok = true;
  return val;
}

hml_ctrl_poke_cb hml_bridge_control_poke_cb(void) { return poke_tramp; }
hml_ctrl_read_cb hml_bridge_control_read_cb(void) { return read_tramp; }

void hml_bridge_control_set_dest_cb(void *ctx, const hml_dest_cfg *cfg) {
  hml_bridge_t *b = (hml_bridge_t *)ctx;
  if (!b || !cfg) return;
  /* SET_DEST is the configure step: leave INIT, then program the packetizer. */
  if (hml_bridge_phase_of(b) < HML_BRIDGE_PHASE_CONFIG)
    (void)hml_bridge_set_phase(b, HML_BRIDGE_PHASE_CONFIG);
  log_step("program dest", hml_bridge_stub_program_dest(b, cfg));
}

void hml_bridge_control_start_cb(void *ctx) {
  hml_bridge_t *b = (hml_bridge_t *)ctx;
  if (!b) return;
  log_step("start", hml_bridge_set_streaming(b, true));
}

void hml_bridge_control_stop_cb(void *ctx) {
  hml_bridge_t *b = (hml_bridge_t *)ctx;
  if (!b) return;
  log_step("stop", hml_bridge_set_streaming(b, false));
}

hml_status hml_bridge_control_wire(hml_control_t *c, hml_bridge_t *b) {
  if (!c || !b) return HML_ERR_ARG;
  hml_status st = hml_control_set_cbs(c, hml_bridge_control_set_dest_cb,
                                       hml_bridge_control_start_cb,
                                       hml_bridge_control_stop_cb,
                                       hml_bridge_control_poke_cb(), b);
  if (st != HML_OK) return st;
  return hml_control_set_read_cb(c, hml_bridge_control_read_cb(), b);
}
