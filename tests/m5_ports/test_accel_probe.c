/*
 * M5 / test_accel_probe.c -- the M6 host-accelerate layer has to be useful on a
 * box with no RoCE NIC and no GPU, and it has to say so quickly and honestly.
 *
 * Asserted here:
 *   - hml_accel_probe() returns HML_OK for any non-NULL out, whatever the host
 *     looks like, and its answer is self-consistent
 *   - two probes return the same thing (the result is cached, not re-detected)
 *   - hml_accel_open(HML_ACCEL_GPU) on a GPU-less box is refused in well under
 *     a second, measured with the same hml_clock_us() the library uses
 *   - HML_ACCEL_NONE opens, describes itself and closes cleanly, twice over
 *   - argument handling and the status buffer's capacity bound are safe
 *
 * "Fast" is 1 s per the M6 plan ("unsupported configs fall back in <1s"); the
 * assertion uses 100 ms of slack below that so a slow shared CI box cannot
 * produce a flaky pass/fail, while a real blocking probe would still fail.
 */

#include "hml_accel.h"
#include "hml/hml_port.h"

#include "hml_t_m5.h"

#include <string.h>

#define FALLBACK_BUDGET_US 1000000ull /* the plan's <1 s */
#define FALLBACK_SLACK_US 100000ull  /* 900 ms asserted, so a busy box is not flaky */

/* strnlen without the POSIX dependency. */
static size_t term_len(const char *s, size_t max) {
  size_t n = 0;
  while (n < max && s[n] != '\0') n++;
  return n;
}

static void check_self_consistent(const hml_accel_caps *c) {
  T_CHECK(c->best == HML_ACCEL_NONE || c->best == HML_ACCEL_HOST_MR || c->best == HML_ACCEL_GPU,
          "best = %d is not a valid hml_accel_kind", (int)c->best);
  T_CHECK(c->backend[0] != '\0', "backend name is empty");
  T_CHECK(term_len(c->backend, sizeof(c->backend)) < sizeof(c->backend),
          "backend name is not NUL-terminated inside 32 bytes");
  /* The GPU backend is report-only in this milestone, so it must never be
   * advertised as something hml_accel_open() could hand over. */
  T_CHECK(c->best != HML_ACCEL_GPU, "best = GPU, but the GPU backend refuses to open");
  if (c->best == HML_ACCEL_HOST_MR) {
    T_CHECK(c->have_ibverbs, "best = HOST_MR but have_ibverbs is false");
  }
  if (c->best == HML_ACCEL_NONE) {
    T_CHECK(!c->have_ibverbs, "best = NONE while have_ibverbs is true");
  }
  if (c->have_peermem) T_CHECK(c->have_gpu, "peermem reported without an NVIDIA driver node");
  if (!c->have_gpu) T_CHECK(!c->have_peermem, "GPU reported absent but peermem present");
}

static void test_probe(void) {
  hml_accel_caps a, b;
  memset(&a, 0xcd, sizeof(a));
  memset(&b, 0xab, sizeof(b));

  T_CHECK(hml_accel_probe(NULL) == HML_ERR_ARG, "probe(NULL) must be HML_ERR_ARG");
  T_CHECK(hml_accel_probe(&a) == HML_OK, "probe() must always report HML_OK");
  check_self_consistent(&a);

  T_CHECK(hml_accel_probe(&b) == HML_OK, "second probe must report HML_OK");
  T_CHECK(memcmp(&a, &b, sizeof(a)) == 0, "two probes disagree: %d/%d/%d \"%s\" vs %d/%d/%d \"%s\"",
          (int)a.best, (int)a.have_ibverbs, (int)a.have_gpu, a.backend, (int)b.best,
          (int)b.have_ibverbs, (int)b.have_gpu, b.backend);

  T_CHECK(hml_accel_backend() != NULL, "hml_accel_backend() must not be NULL");
  const char *fam = hml_accel_backend();
  T_CHECK(fam[0] != '\0', "hml_accel_backend() returned an empty string");
  if (a.have_ibverbs) {
    T_CHECK(strcmp(fam, "ibverbs") == 0, "family is \"%s\" but devices were found", fam);
  } else {
    T_CHECK(strcmp(fam, "cpu") == 0, "family is \"%s\" but no device was found", fam);
  }
  if (strcmp(fam, "cpu") == 0) {
    T_CHECK(a.best == HML_ACCEL_NONE, "a cpu build must not advertise a device backend");
  }
}

static void test_gpu_fallback_is_fast(void) {
  hml_accel_caps c;
  (void)hml_accel_probe(&c);

  void *h = (void *)0x1;
  uint64_t t0 = hml_clock_us();
  hml_status st = hml_accel_open(&h, HML_ACCEL_GPU);
  uint64_t dt = hml_clock_us() - t0;

  if (!c.have_gpu) {
    T_CHECK(st == HML_ERR_NOTSUP, "open(GPU) without a GPU must be HML_ERR_NOTSUP, got %d",
            (int)st);
    T_CHECK(h == NULL, "a refused open must leave the out pointer NULL");
    T_CHECK(dt < FALLBACK_BUDGET_US - FALLBACK_SLACK_US,
            "open(GPU) refusal took %llu us, budget is <%llu us", (unsigned long long)dt,
            (unsigned long long)(FALLBACK_BUDGET_US - FALLBACK_SLACK_US));
  } else {
    /* Report-only backend: refused on purpose, but never a crash. */
    T_CHECK(st == HML_OK || st == HML_ERR_NOTSUP, "open(GPU) returned %d", (int)st);
    if (st == HML_OK) hml_accel_close(h);
  }
  T_CHECK(dt < FALLBACK_BUDGET_US, "open(GPU) took %llu us, the plan allows <%llu us",
          (unsigned long long)dt, (unsigned long long)FALLBACK_BUDGET_US);

  /* A host-MR request is only ever answered when the probe said so. */
  h = (void *)0x1;
  t0 = hml_clock_us();
  st = hml_accel_open(&h, HML_ACCEL_HOST_MR);
  dt = hml_clock_us() - t0;
  if (!c.have_ibverbs) {
    T_CHECK(st == HML_ERR_NOTSUP, "open(HOST_MR) without verbs must be HML_ERR_NOTSUP, got %d",
            (int)st);
    T_CHECK(h == NULL, "a refused open must leave the out pointer NULL");
  } else if (st == HML_OK) {
    hml_accel_close(h);
  } else {
    T_CHECK(st == HML_ERR_NOTSUP || st == HML_ERR_IO, "open(HOST_MR) returned %d", (int)st);
  }
  T_CHECK(dt < FALLBACK_BUDGET_US, "open(HOST_MR) took %llu us", (unsigned long long)dt);

  /* Out-of-range backend ids are an argument error, never a crash. */
  h = (void *)0x1;
  T_CHECK(hml_accel_open(&h, (hml_accel_kind)77) == HML_ERR_ARG, "open(77) must be HML_ERR_ARG");
  T_CHECK(h == NULL, "a refused open must leave the out pointer NULL");
  T_CHECK(hml_accel_open(NULL, HML_ACCEL_NONE) == HML_ERR_ARG, "open(NULL) must be HML_ERR_ARG");
}

static void test_cpu_passthrough(void) {
  int i;
  for (i = 0; i < 2; i++) {
    void *h = NULL;
    T_CHECK(hml_accel_open(&h, HML_ACCEL_NONE) == HML_OK,
            "open(NONE) must always succeed (iteration %d)", i);
    T_CHECK(h != NULL, "open(NONE) returned a NULL handle (iteration %d)", i);

    hml_accel_kind active = (hml_accel_kind)99;
    char name[HML_ACCEL_BACKEND_MAX];
    memset(name, 0x5a, sizeof(name));
    T_CHECK(hml_accel_status(h, &active, name, sizeof(name)) == HML_OK, "status() failed");
    T_CHECK(active == HML_ACCEL_NONE, "status() reported kind %d, want NONE", (int)active);
    T_CHECK(name[0] != '\0', "status() returned an empty backend name");
    T_CHECK(term_len(name, sizeof(name)) < sizeof(name), "status() name is not terminated");

    /* Small capacity: truncated, terminated, never past the end. */
    char tiny[4];
    memset(tiny, 0x33, sizeof(tiny));
    T_CHECK(hml_accel_status(h, NULL, tiny, 1) == HML_OK, "status() with cap 1 failed");
    T_CHECK(tiny[0] == '\0', "status() with cap 1 must write only a terminator");
    T_CHECK(tiny[1] == 0x33 && tiny[2] == 0x33 && tiny[3] == 0x33,
            "status() with cap 1 wrote past the buffer");

    T_CHECK(hml_accel_status(h, &active, NULL, 0) == HML_OK, "status(active only) failed");
    T_CHECK(hml_accel_status(h, NULL, NULL, 0) == HML_ERR_ARG,
            "status() with nothing to write must be HML_ERR_ARG");
    T_CHECK(hml_accel_status(NULL, &active, name, sizeof(name)) == HML_ERR_ARG,
            "status(NULL) must be HML_ERR_ARG");

    hml_accel_close(h);
    T_CHECK(1, "close() survived (iteration %d)", i);
  }
  hml_accel_close(NULL); /* documented no-op */
  T_CHECK(1, "close(NULL) survived");
}

int main(void) {
  test_probe();
  test_gpu_fallback_is_fast();
  test_cpu_passthrough();
  T_DONE("test_accel_probe");
}
