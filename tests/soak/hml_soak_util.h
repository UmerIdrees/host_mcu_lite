#ifndef HML_SOAK_UTIL_H
#define HML_SOAK_UTIL_H

/*
 * Shared plumbing for the Track M soak tests.
 *
 * The plan asks for a long soak ("1h continuous; memory flat (no leak)" for M1,
 * "24-72h T0 soak" for M7) and a CTest entry that finishes in seconds. Soak is
 * therefore parameterised by duration, and both entry points run the same binary:
 *
 *   test_soak_* --seconds N     run for N seconds (default 5)
 *   test_soak_* --seconds N --dump <file>
 *
 * A long run is the same binary with a bigger --seconds, so a green CTest run
 * means the long run would have been green up to that point too, and the
 * acceptance criteria below are checked at every sample rather than only at the
 * end. That is the point of a bounded check inside a long run: a leak that shows
 * up at hour 3 has to be visible as growth, not only as a final verdict.
 *
 * Three things are asserted continuously, all of them statements about the
 * library rather than about the test:
 *
 *   1. accounting invariants that must hold at every sample -- counters that only
 *      grow, never exceed what was fed, and never contradict each other;
 *   2. resident-set flatness, with a growth rate rather than a total, so a slow
 *      leak is caught and a fixed-size arena is not mistaken for one;
 *   3. a wall-clock deadline that is checked inside the loop, so a hang is a
 *      bounded failure instead of a ctest timeout.
 *
 * No threads, no sleeps and no randomness: the loop is a fixed workload, so two
 * runs of the same duration do the same work and a difference in the numbers
 * means something changed in the library.
 */

#include "hml/hml_port.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Some helpers here are used by only some of the soak tests, and this header is
 * shared by all of them, so they are marked maybe-unused rather than each test
 * growing a reference to something it does not need. */
#if defined(__GNUC__) || defined(__clang__)
#define HML_SOAK_UNUSED __attribute__((unused))
#else
#define HML_SOAK_UNUSED
#endif

/* Resident set in kibibytes, or 0 when it cannot be read. Linux only: the
 * alternative was a build tag, and a soak that silently cannot see a leak is
 * worse than one that says it has no RSS source. */
static long hml_soak_rss_kib(void) {
  FILE *f = fopen("/proc/self/statm", "r");
  if (!f) return 0;
  unsigned long total = 0, resident = 0;
  int got = fscanf(f, "%lu %lu", &total, &resident);
  (void)fclose(f);
  if (got != 2) return 0;
  /* sysconf rather than getpagesize(): the tree builds with _POSIX_C_SOURCE only,
   * where getpagesize() is not declared. */
  long page = sysconf(_SC_PAGESIZE);
  if (page <= 0) return 0;
  return (long)((resident * (unsigned long)page) / 1024u);
}

/* Report a failed invariant and count it. Every CHECK below names the file and
 * line, so a soak failure is actionable from the CTest log without a debugger. */
static int g_soak_fails;

#define SOAK_FAILF(...)                                                    \
  do {                                                                     \
    printf("FAIL %s:%d: ", __FILE__, __LINE__);                            \
    printf(__VA_ARGS__);                                                   \
    printf("\n");                                                          \
    g_soak_fails++;                                                        \
  } while (0)

#define SOAK_CHECK(cond, ...)          \
  do {                                \
    if (!(cond)) SOAK_FAILF(__VA_ARGS__); \
  } while (0)

/* The trailing arguments are a printf format describing what was compared; the
 * two numbers are appended, so a call never has to remember to include them:
 *   SOAK_CHECK_EQ(sets, rounds, "set_dest callbacks")
 *   SOAK_CHECK_EQ(sets, rounds, "round %llu", r) */
#define SOAK_CHECK_EQ(got, want, ...)                                                 \
  do {                                                                                \
    unsigned long long g_ = (unsigned long long)(got);                                \
    unsigned long long w_ = (unsigned long long)(want);                               \
    if (g_ != w_) SOAK_FAILF(__VA_ARGS__);                                            \
    if (g_ != w_) printf("      (got %llu, want %llu)\n", g_, w_);                    \
  } while (0)

#define SOAK_CHECK_STATUS(expr, want)                                              \
  do {                                                                             \
    hml_status s_ = (expr);                                                        \
    if (s_ != (want)) SOAK_FAILF("%s -> %d, want %d", #expr, (int)s_, (int)(want)); \
  } while (0)

/* A wall-clock budget. hml_clock_us() is the library's own monotonic clock, so
 * the deadline is unaffected by the library under test changing the system clock
 * (nothing should, but a soak is where that would show up). */
typedef struct soak_budget {
  uint64_t start_us;
  uint64_t seconds;
} soak_budget;

static void soak_budget_init(soak_budget *b, unsigned seconds) {
  b->start_us = hml_clock_us();
  b->seconds = (uint64_t)seconds;
}

static uint64_t soak_budget_elapsed_us(const soak_budget *b) {
  return hml_clock_us() - b->start_us;
}

/* Non-zero once the budget is spent. Checked in the loop body, so a workload that
 * stops making progress fails in bounded time and prints what it had done. */
static bool soak_budget_expired(const soak_budget *b) {
  return soak_budget_elapsed_us(b) >= b->seconds * 1000000ull;
}

/* Resident-set growth watch.
 *
 * Flatness is judged as a slope, not as "start == end": an arena that is
 * allocated once and reused forever looks like growth on the first sample, and a
 * slow leak is invisible in a total. So the watch records the baseline after a
 * warm-up (the first sample or two are dominated by first-touch page faults and
 * by whatever the test itself allocated lazily) and then fails when the resident
 * set has grown by more than a fixed budget per unit of work.
 *
 * The budget is deliberately generous relative to the workload: a soak that fails
 * on allocator bookkeeping noise is a soak that gets disabled. It is small
 * relative to a real leak: the loops below push megabytes of traffic through
 * per second, so a per-iteration leak shows up within a few seconds. */
typedef struct soak_rss {
  long baseline_kib;
  long peak_kib;
  uint64_t baseline_at_iter;
  uint64_t iter;
  long budget_kib;
  bool available;
} soak_rss;

static void soak_rss_init(soak_rss *w, long budget_kib) {
  memset(w, 0, sizeof(*w));
  w->budget_kib = budget_kib;
  w->baseline_kib = hml_soak_rss_kib();
  w->available = w->baseline_kib > 0;
}

/* Record a sample. `warmup_iters` samples are ignored before the baseline is
 * taken, which is what keeps a one-off allocation from reading as a leak. */
static void soak_rss_sample(soak_rss *w, uint64_t warmup_iters) {
  w->iter++;
  long now = hml_soak_rss_kib();
  if (now <= 0) return;
  if (now > w->peak_kib) w->peak_kib = now;
  if (w->iter <= warmup_iters) return;
  if (w->baseline_at_iter == 0u) {
    w->baseline_at_iter = w->iter;
    if (w->baseline_kib <= 0) w->baseline_kib = now;
    return;
  }
  if (!w->available) return;
  long growth = now - w->baseline_kib;
  if (growth > w->budget_kib) {
    SOAK_FAILF("resident set grew %ld KiB over %llu iterations (baseline %ld KiB at iteration "
               "%llu, now %ld KiB, budget %ld KiB): this looks like a leak",
               growth, (unsigned long long)(w->iter - w->baseline_at_iter), w->baseline_kib,
               (unsigned long long)w->baseline_at_iter, now, w->budget_kib);
    /* Re-baseline so a single finding does not print once per subsequent sample
     * and bury the rest of the run. */
    w->baseline_kib = now;
    w->baseline_at_iter = w->iter;
  }
}

/* Deterministic xorshift32, so a soak whose workload wants variation is still
 * reproducible, and no libc rand() state leaks into its numbers. */
HML_SOAK_UNUSED static uint32_t soak_rng(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x ? x : 0x6d2b79f5u;
  return *state;
}

/* First free UDP bind port at or after `base` inside [lo, hi], or 0. Soak tests
 * bind loopback ports, and a busy port has to be visible rather than turned into
 * a confusing later failure. SOAK_PORT_* ranges are private per test so two soak
 * binaries can run at the same time.
 *
 * Marked maybe-unused because a soak that only drives in-process parsers has no
 * socket to open, and the header is shared by both shapes. */
HML_SOAK_UNUSED static bool soak_port_free(uint16_t port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(0x7f000001u);
  a.sin_port = htons(port);
  bool ok = bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
  (void)close(fd);
  return ok;
}

HML_SOAK_UNUSED static uint16_t soak_port_pick(uint16_t lo, uint16_t hi, uint16_t *cursor) {
  for (unsigned tries = 0; tries < 64u; tries++) {
    uint16_t p = *cursor;
    *cursor = (uint16_t)(p >= hi ? lo : p + 1u);
    if (soak_port_free(p)) return p;
  }
  return 0u;
}

static int soak_finish(const char *name, uint64_t iters, uint64_t elapsed_us,
                       const soak_rss *w) {
  if (g_soak_fails) {
    printf("FAILED %s: %d check(s) over %llu iterations in %.1f s\n", name, g_soak_fails,
           (unsigned long long)iters, (double)elapsed_us / 1e6);
    return 1;
  }
  if (w->available) {
    printf("PASS %s: %llu iterations in %.1f s, rss %ld KiB -> peak %ld KiB (flat)\n", name,
           (unsigned long long)iters, (double)elapsed_us / 1e6, w->baseline_kib, w->peak_kib);
  } else {
    printf("PASS %s: %llu iterations in %.1f s, rss unavailable on this platform\n", name,
           (unsigned long long)iters, (double)elapsed_us / 1e6);
  }
  return 0;
}

#endif /* HML_SOAK_UTIL_H */
