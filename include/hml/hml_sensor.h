#ifndef HML_SENSOR_H
#define HML_SENSOR_H

#include "hml_types.h"

/*
 * Sensor HAL (Emulator-style): open/acquire/release. Sensor-agnostic.
 * acquire() fills dst with raw/packed samples for one payload-chunk (or a full
 * frame chunk); release() signals end of that unit. No device-specific code in core.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hml_sensor hml_sensor_t;

typedef struct hml_sensor_ops {
  const char *name;
  hml_status (*open)(hml_sensor_t *s, void *param);
  hml_status (*acquire)(hml_sensor_t *s, uint8_t *dst, size_t cap, size_t *out);
  hml_status (*release)(hml_sensor_t *s);
  void (*close)(hml_sensor_t *s);
} hml_sensor_ops;

typedef struct hml_sensor {
  const hml_sensor_ops *ops;
  void *impl;
} hml_sensor_t;

#ifdef __cplusplus
}
#endif

#endif /* HML_SENSOR_H */