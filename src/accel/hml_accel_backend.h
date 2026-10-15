/*
 * hml_accel_backend.h -- private contract between the portable accel core
 * (hml_accel.c) and the ONE backend translation unit CMake selects:
 *   src/accel/ibverbs/hml_accel_ibverbs.c      when <infiniband/verbs.h> and
 *                                               libibverbs are both found
 *                                               (HML_HAVE_IBVERBS defined)
 *   src/accel/ibverbs/hml_accel_backend_stub.c otherwise
 * Exactly one of them is compiled, so neither file needs a preprocessor
 * conditional: the build system makes the choice, not the source.
 */

#ifndef HML_ACCEL_BACKEND_H
#define HML_ACCEL_BACKEND_H

#include "hml_accel.h"

/* Backend family string for this binary: "cpu" or "ibverbs". Never NULL. */
const char *hml_accel_backend_family(void);

/* Runtime check: 1 when the build has ibverbs AND at least one device is
 * enumerable now, else 0. Cheap enough to call from probe(). */
int hml_accel_backend_have_ibverbs(void);

/* Acquire a backend-private implementation. HML_ACCEL_NONE sets *out to NULL
 * (the CPU passthrough) and succeeds. Anything unavailable returns
 * HML_ERR_NOTSUP. */
hml_status hml_accel_backend_open(hml_accel_kind want, void **out);

/* Release what hml_accel_backend_open() produced. NULL-safe. */
void hml_accel_backend_close(void *impl);

/* Write a NUL-terminated backend name into buf, truncated to cap. */
hml_status hml_accel_backend_describe(void *impl, char *buf, size_t cap);

#endif /* HML_ACCEL_BACKEND_H */
