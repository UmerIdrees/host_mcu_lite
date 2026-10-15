/*
 * hml_accel_backend_stub.c -- default accel backend: CPU passthrough only.
 *
 * CMake compiles this file when <infiniband/verbs.h> or libibverbs is missing.
 * It reports no ibverbs, hands out the CPU passthrough handle for
 * HML_ACCEL_NONE, and refuses everything else with HML_ERR_NOTSUP so callers
 * fall back cleanly. No conditionals here: being in this file IS the CPU story.
 */

#include "hml_accel_backend.h"

#include <string.h>

#define STUB_NAME "cpu"

const char *hml_accel_backend_family(void) { return STUB_NAME; }

int hml_accel_backend_have_ibverbs(void) { return 0; }

hml_status hml_accel_backend_open(hml_accel_kind want, void **out) {
  if (out == NULL) return HML_ERR_ARG;
  *out = NULL;
  if (want == HML_ACCEL_NONE) return HML_OK; /* CPU passthrough, no private state */
  return HML_ERR_NOTSUP;
}

void hml_accel_backend_close(void *impl) { (void)impl; }

hml_status hml_accel_backend_describe(void *impl, char *buf, size_t cap) {
  (void)impl;
  if (buf == NULL || cap == 0) return HML_ERR_ARG;
  size_t n = strlen(STUB_NAME);
  if (n > cap - 1) n = cap - 1;
  memcpy(buf, STUB_NAME, n);
  buf[n] = '\0';
  return HML_OK;
}
