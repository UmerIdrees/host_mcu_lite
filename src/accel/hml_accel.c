/*
 * hml_accel.c -- portable core of the M6 host-accelerate layer.
 *
 * Platform-conditional free by construction: this file has no preprocessor
 * conditionals and includes no OS or vendor header. The only host facts it needs
 * are "does this path exist", which <sys/stat.h> and <dirent.h> answer, and the
 * chosen backend, which hml_accel_backend.h abstracts. Every optional
 * capability decision is made at runtime and every unsupported request is
 * refused with HML_ERR_NOTSUP so the caller falls back to CPU buffers.
 */

#include "hml_accel.h"
#include "hml_accel_backend.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* The control layer of the M6 layer: a CPU passthrough handle plus the private
 * blob the selected backend produced. */
struct hml_accel_handle {
  hml_accel_kind kind;
  char name[HML_ACCEL_BACKEND_MAX];
  void *impl;
};

static int path_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

/* An NVIDIA driver stack is present if the control node exists. Checking the
 * node rather than a version string keeps this working across driver series. */
static int nvidia_driver_node(void) {
  static const char *const nodes[] = {"/dev/nvidiactl", "/dev/nvidia0", "/dev/nvidia-uvm"};
  size_t i;
  for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
    if (path_exists(nodes[i])) return 1;
  }
  DIR *d = opendir("/dev");
  if (d == NULL) return 0;
  int found = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strncmp(ent->d_name, "nvidia", 6) == 0) {
      found = 1;
      break;
    }
  }
  closedir(d);
  return found;
}

/* nvidia-peermem is the kernel module that exposes GPU BAR to RDMA hardware.
 * Its presence is the cheapest honest GPU-Direct signal available without
 * linking CUDA -- and it is still only a necessary condition. */
static int nvidia_peermem(void) { return path_exists("/sys/bus/pci/drivers/nvidia-peermem"); }

static void probe_fill(hml_accel_caps *c) {
  memset(c, 0, sizeof(*c));
  c->have_ibverbs = hml_accel_backend_have_ibverbs() > 0;
  c->have_peermem = nvidia_peermem() ? 1 : 0;
  c->have_gpu = nvidia_driver_node() ? 1 : 0;

  /* `best` is the best backend hml_accel_open() can really hand over, so the
   * report-only GPU backend never wins; have_gpu/have_peermem still report it. */
  if (c->have_ibverbs) {
    c->best = HML_ACCEL_HOST_MR;
    (void)snprintf(c->backend, sizeof(c->backend), "host-mr");
  } else {
    c->best = HML_ACCEL_NONE;
    (void)snprintf(c->backend, sizeof(c->backend), "cpu");
  }
}

/* Cached so repeated probes are identical by construction. Call during
 * single-threaded start-up; the cache is deliberately unlocked. */
static hml_accel_caps g_caps;
static int g_caps_valid;

hml_status hml_accel_probe(hml_accel_caps *out) {
  if (out == NULL) return HML_ERR_ARG;
  if (!g_caps_valid) {
    probe_fill(&g_caps);
    g_caps_valid = 1;
  }
  *out = g_caps;
  return HML_OK;
}

const char *hml_accel_backend(void) {
  hml_accel_caps c;
  if (hml_accel_probe(&c) == HML_OK && !c.have_ibverbs) {
    return "cpu";
  }
  return hml_accel_backend_family();
}

hml_status hml_accel_open(void **out, hml_accel_kind want) {
  if (out == NULL) return HML_ERR_ARG;
  *out = NULL;
  if (want != HML_ACCEL_NONE && want != HML_ACCEL_HOST_MR && want != HML_ACCEL_GPU)
    return HML_ERR_ARG;

  hml_accel_caps caps;
  (void)hml_accel_probe(&caps);

  /* Refuse a GPU request before touching any backend: stat()-only, so the
   * "fall back in well under a second" contract holds even if device
   * enumeration on the host is slow. */
  if (want == HML_ACCEL_GPU && !caps.have_gpu) return HML_ERR_NOTSUP;
  if (want == HML_ACCEL_HOST_MR && !caps.have_ibverbs) return HML_ERR_NOTSUP;

  void *impl = NULL;
  hml_status st = hml_accel_backend_open(want, &impl);
  if (st != HML_OK) return st;

  struct hml_accel_handle *h = (struct hml_accel_handle *)calloc(1, sizeof(*h));
  if (h == NULL) {
    hml_accel_backend_close(impl);
    return HML_ERR_NOMEM;
  }
  h->kind = want;
  h->impl = impl;
  (void)hml_accel_backend_describe(impl, h->name, sizeof(h->name));
  *out = h;
  return HML_OK;
}

void hml_accel_close(void *handle) {
  struct hml_accel_handle *h = (struct hml_accel_handle *)handle;
  if (h == NULL) return;
  hml_accel_backend_close(h->impl);
  free(h);
}

hml_status hml_accel_status(void *handle, hml_accel_kind *active, char *buf, size_t cap) {
  struct hml_accel_handle *h = (struct hml_accel_handle *)handle;
  if (h == NULL) return HML_ERR_ARG;
  if (active == NULL && (buf == NULL || cap == 0)) return HML_ERR_ARG;

  if (active != NULL) *active = h->kind;
  if (buf != NULL && cap > 0) {
    /* Bounded copy: the caller's buffer is never overflowed, always terminated. */
    size_t n = strlen(h->name);
    if (n > cap - 1) n = cap - 1;
    memcpy(buf, h->name, n);
    buf[n] = '\0';
  }
  return HML_OK;
}
