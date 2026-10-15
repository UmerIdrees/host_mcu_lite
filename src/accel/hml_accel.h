/*
 * hml_accel.h -- M6 "host accelerate" layer: capability detection and backend
 * abstraction with a guaranteed CPU fallback.
 *
 * SCOPE / HONEST STATUS
 * ---------------------
 * M6 is "report or T2 deferred; clean fallback". This layer answers two
 * questions -- "what could this host do?" and "may I have a handle for that?" --
 * and nothing else. It generates no traffic, opens no queue pair and moves no
 * frames; the data path stays in host/hml_host.
 *
 * There are NO performance claims anywhere in this layer. Nothing here can be
 * called "zero-copy" or "GPU Direct" on the strength of a probe: a NIC
 * registered through ibverbs is not a ConnectX data path, and an
 * nvidia-peermem module being loaded does not prove the NIC can peer with the
 * GPU. Every claim in docs/capability_matrix.md stays "not present" until
 * someone measures it on a ConnectX machine.
 *
 * CUDA IS NEVER LINKED. GPU presence is probed at runtime from /dev and /sys.
 *
 * BACKENDS
 *   HML_ACCEL_NONE     plain CPU buffers; always available, never refused.
 *   HML_ACCEL_HOST_MR  ibverbs: device + protection domain + a registered host
 *                      memory region (ibv_reg_mr). Built only when CMake finds
 *                      both <infiniband/verbs.h> and libibverbs, which defines
 *                      HML_HAVE_IBVERBS.
 *   HML_ACCEL_GPU      report-only in this milestone. hml_accel_open(GPU)
 *                      returns HML_ERR_NOTSUP on every build, including a
 *                      peermem box: registering GPU BAR1 needs the CUDA driver
 *                      API, which this library refuses to depend on. Use
 *                      HML_ACCEL_HOST_MR and hand the buffer to an external
 *                      CUDA pipeline instead.
 *
 * TIMING: hml_accel_probe() and a refused hml_accel_open() only stat() a few
 * paths (and, on an ibverbs build, enumerate devices), so an unsupported
 * request is answered in well under 1 s. "Fall back fast" is a test assertion
 * in tests/m5_ports/test_accel_probe.c, not a hope.
 *
 * THREADING: hml_accel_probe() caches its result in a static, so the cap
 * answer is stable for the life of the process. The cache is not locked; call
 * probe() once during single-threaded start-up.
 */

#ifndef HML_ACCEL_H
#define HML_ACCEL_H

#include "hml/hml_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Size of the backend name field, in hml_accel_caps and in the status buffer. */
#define HML_ACCEL_BACKEND_MAX 32

typedef enum hml_accel_kind {
  HML_ACCEL_NONE = 0,   /* CPU buffers, guaranteed fallback */
  HML_ACCEL_HOST_MR = 1, /* ibverbs-protected host memory region */
  HML_ACCEL_GPU = 2      /* report-only, see the header comment */
} hml_accel_kind;

/* What this host could do. `best` is the best backend hml_accel_open() can
 * actually hand out right now, so it is never HML_ACCEL_GPU in this milestone.
 * have_gpu / have_peermem are necessary-not-sufficient proxies for GPU Direct:
 * they read /dev and /sys only. `backend` names the backend `best` refers to. */
typedef struct hml_accel_caps {
  hml_accel_kind best;
  bool have_ibverbs;
  bool have_peermem;
  bool have_gpu;
  char backend[32];
} hml_accel_caps;

/* Detect capabilities. Never fails for a non-NULL out: no NIC, no driver, no
 * permissions all report HML_OK with a CPU-only capability set. HML_ERR_ARG is
 * returned only for out == NULL. The result is cached and self-consistent. */
HML_API hml_status hml_accel_probe(hml_accel_caps *out);

/* Backend family this binary was built with: "cpu" or "ibverbs". Never NULL. */
HML_API const char *hml_accel_backend(void);

/* Acquire a handle. want == HML_ACCEL_NONE always succeeds (passthrough CPU
 * handle). A backend this build or this host cannot provide returns
 * HML_ERR_NOTSUP and leaves *out NULL, fast. HML_ERR_ARG for a NULL out or an
 * out-of-range want. */
HML_API hml_status hml_accel_open(void **out, hml_accel_kind want);

/* Release a handle. NULL-safe and idempotent. */
HML_API void hml_accel_close(void *handle);

/* Describe a handle. *active (optional) receives the backend actually in use;
 * buf (optional) receives a NUL-terminated backend name, truncated to cap
 * without overflowing. At least one of the two must be given. */
HML_API hml_status hml_accel_status(void *handle, hml_accel_kind *active, char *buf,
                                     size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HML_ACCEL_H */
