#ifndef HML_TYPES_H
#define HML_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* HML_API / HML_HIDDEN / HML_INLINE. Additive: see hml_export.h. */
#include "hml_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HML_VERSION_STRING "0.2.0"
#define HML_VERSION_MAJOR 0
#define HML_VERSION_MINOR 2
#define HML_VERSION_PATCH 0

typedef enum hml_status {
  HML_OK = 0,
  HML_ERR_ARG,       /* bad argument / null ptr */
  HML_ERR_NOMEM,     /* allocation or pool exhaustion */
  HML_ERR_IO,        /* underlying socket/port error */
  HML_ERR_PROTO,     /* protocol violation (bad magic/version/len) */
  HML_ERR_AGAIN,     /* would block / no data */
  HML_ERR_TIMEOUT,   /* deadline exceeded */
  HML_ERR_NOTSUP,    /* feature not available on this build/port */
  HML_ERR_BUSY,      /* resource busy, retry later */
  HML_ERR_CRC,       /* integrity check failed */
  HML_ERR_SEQ,       /* out-of-order or stale sequence */
  HML_ERR_TOOLARGE,  /* frame/datagram exceeds configured max */
  HML_ERR_STATE,     /* invalid operation for current state */
  HML_ERR_UNKNOWN    /* internal / unspecified */
} hml_status;

#ifdef __cplusplus
}
#endif

#endif /* HML_TYPES_H */