#ifndef HML_SYNTH_PRIV_H
#define HML_SYNTH_PRIV_H

#include "hml/hml_sensor.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* open() parameter for the "synthetic" sensor. */
typedef struct hml_synth_cfg {
  uint32_t seed;       /* pattern seed */
  size_t frame_bytes;  /* bytes produced per acquire(); 0 == one max fragment */
} hml_synth_cfg;

#define HML_SYNTH_MAX_ACQUIRE HML_MAX_FRAG_PAYLOAD

/* The "synthetic" hml_sensor_ops vtable. */
const hml_sensor_ops *hml_synth_ops(void);

/*
 * There is no sensor registry in the v1 frozen headers, so this is a no-op kept
 * for symmetry with the transports: hml_synth_ops() is the only way in.
 */
hml_status hml_synth_register(void);

/* Deterministic pattern: byte at absolute index i depends only on (i, seed). */
uint8_t hml_synth_pattern_byte(uint64_t index, uint32_t seed);
void hml_synth_pattern_fill(uint8_t *dst, size_t n, uint64_t first_index, uint32_t seed);

/* FNV-1a 32. Defined here so the host receiver hashes with the same function. */
uint32_t hml_fnv1a32(const void *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* HML_SYNTH_PRIV_H */
