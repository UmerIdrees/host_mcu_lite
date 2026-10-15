/*
 * hml_accel_ibverbs.c -- M6 real backend, compiled only when CMake found both
 * <infiniband/verbs.h> and libibverbs (that is what defines HML_HAVE_IBVERBS).
 * There is deliberately no #if in this file: if the compiler can see
 * <infiniband/verbs.h>, this is the backend.
 *
 * What it does: opens the first usable RDMA device, allocates a protection
 * domain and registers one host memory region (ibv_reg_mr) so the buffer is
 * usable by that device. That is HML_ACCEL_HOST_MR.
 *
 * What it does NOT do, and why: no queue pair, no completion queue, no
 * post-send loop, and no GPU BAR registration. The data path lives in
 * host/hml_host; this layer only answers "can this host do protected memory
 * registration, and with which device". HML_ACCEL_GPU is refused outright
 * because registering GPU BAR1 needs the CUDA driver API, which this library
 * never links.
 */

#include "hml_accel_backend.h"

#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Size of the default registered host region. Large enough to hold a couple of
 * HML_MAX_FRAME_SIZE frames, small enough to sit in ordinary host memory. */
#define IBV_MR_BYTES (64u * 1024u)
#define IBV_MR_ALIGN 4096u

struct ibv_impl {
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_mr *mr;
  void *buf;
  size_t len;
  char dev[32];
};

const char *hml_accel_backend_family(void) { return "ibverbs"; }

int hml_accel_backend_have_ibverbs(void) {
  int n = 0;
  struct ibv_device **list = ibv_get_device_list(&n);
  if (list == NULL) return 0;
  if (n <= 0) {
    ibv_free_device_list(list);
    return 0;
  }
  ibv_free_device_list(list);
  return 1;
}

hml_status hml_accel_backend_open(hml_accel_kind want, void **out) {
  if (out == NULL) return HML_ERR_ARG;
  *out = NULL;
  if (want == HML_ACCEL_NONE) return HML_OK; /* CPU passthrough */
  if (want == HML_ACCEL_GPU) return HML_ERR_NOTSUP; /* needs the CUDA driver */

  int n = 0;
  struct ibv_device **list = ibv_get_device_list(&n);
  if (list == NULL || n <= 0) {
    if (list != NULL) ibv_free_device_list(list);
    return HML_ERR_NOTSUP;
  }

  struct ibv_impl *st = (struct ibv_impl *)calloc(1, sizeof(*st));
  if (st == NULL) {
    ibv_free_device_list(list);
    return HML_ERR_NOMEM;
  }
  st->len = IBV_MR_BYTES;

  int opened = 0;
  int i;
  for (i = 0; i < n && !opened; i++) {
    struct ibv_context *ctx = ibv_open_device(list[i]);
    if (ctx == NULL) continue; /* busy, no driver, wrong permissions */
    st->ctx = ctx;
    opened = 1;
    const char *nm = ibv_get_device_name(list[i]);
    size_t k = 0;
    if (nm != NULL) {
      while (k + 1 < sizeof(st->dev) && nm[k] != '\0') {
        st->dev[k] = nm[k];
        k++;
      }
    }
    st->dev[k] = '\0';
  }
  ibv_free_device_list(list);
  if (!opened) {
    free(st);
    return HML_ERR_NOTSUP;
  }

  st->pd = ibv_alloc_pd(st->ctx);
  if (st->pd == NULL) {
    ibv_close_device(st->ctx);
    free(st);
    return HML_ERR_IO;
  }
  /* aligned_alloc is C11; the size is a multiple of the alignment by
   * construction, which is what the C11 contract requires. */
  st->buf = aligned_alloc(IBV_MR_ALIGN, st->len);
  if (st->buf == NULL) {
    ibv_dealloc_pd(st->pd);
    ibv_close_device(st->ctx);
    free(st);
    return HML_ERR_NOMEM;
  }
  memset(st->buf, 0, st->len);

  st->mr = ibv_reg_mr(st->pd, st->buf, st->len, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (st->mr == NULL) {
    free(st->buf);
    ibv_dealloc_pd(st->pd);
    ibv_close_device(st->ctx);
    free(st);
    return HML_ERR_IO;
  }
  *out = st;
  return HML_OK;
}

void hml_accel_backend_close(void *impl) {
  struct ibv_impl *st = (struct ibv_impl *)impl;
  if (st == NULL) return;
  if (st->mr != NULL) (void)ibv_dereg_mr(st->mr);
  free(st->buf);
  if (st->pd != NULL) (void)ibv_dealloc_pd(st->pd);
  if (st->ctx != NULL) (void)ibv_close_device(st->ctx);
  free(st);
}

hml_status hml_accel_backend_describe(void *impl, char *buf, size_t cap) {
  if (buf == NULL || cap == 0) return HML_ERR_ARG;

  struct ibv_impl *st = (struct ibv_impl *)impl;
  char tmp[HML_ACCEL_BACKEND_MAX];
  if (st == NULL) {
    (void)snprintf(tmp, sizeof(tmp), "cpu");
  } else {
    (void)snprintf(tmp, sizeof(tmp), "host-mr:%s", st->dev);
  }
  size_t n = strlen(tmp);
  if (n > cap - 1) n = cap - 1;
  memcpy(buf, tmp, n);
  buf[n] = '\0';
  return HML_OK;
}
