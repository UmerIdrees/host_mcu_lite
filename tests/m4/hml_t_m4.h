#ifndef HML_T_M4_H
#define HML_T_M4_H

#include <stdio.h>

/* Shared check macros for the M4 bridge tests (same shape as the M3 tests).
 * The trailing "what" is a printf format string, so a failing check names the
 * case it was in. */

static int g_checks = 0;
static int g_fails = 0;

#define T_CHECK(cond, ...)                                                   \
  do {                                                                       \
    g_checks++;                                                              \
    if (!(cond)) {                                                           \
      g_fails++;                                                             \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                   \
      fprintf(stderr, __VA_ARGS__);                                          \
      fprintf(stderr, "\n");                                                 \
    }                                                                        \
  } while (0)

#define T_EQ_U(actual, expect, ...)                                          \
  do {                                                                       \
    unsigned long long a_ = (unsigned long long)(actual);                    \
    unsigned long long e_ = (unsigned long long)(expect);                    \
    g_checks++;                                                              \
    if (a_ != e_) {                                                          \
      g_fails++;                                                             \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                   \
      fprintf(stderr, __VA_ARGS__);                                          \
      fprintf(stderr, " = 0x%llx, expected 0x%llx\n", a_, e_);               \
    }                                                                        \
  } while (0)

#define T_ST(expr, expect, ...)                                              \
  do {                                                                       \
    hml_status s_ = (expr);                                                  \
    hml_status e_ = (expect);                                                \
    g_checks++;                                                              \
    if (s_ != e_) {                                                          \
      g_fails++;                                                             \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                   \
      fprintf(stderr, __VA_ARGS__);                                          \
      fprintf(stderr, " -> %d, expected %d\n", (int)s_, (int)e_);           \
    }                                                                        \
  } while (0)

#define T_DONE(name)                                                                    \
  do {                                                                                  \
    if (g_fails) {                                                                      \
      fprintf(stderr, "FAIL %s: %d of %d checks failed\n", (name), g_fails, g_checks);  \
      return 1;                                                                         \
    }                                                                                   \
    printf("PASS %s: %d checks\n", (name), g_checks);                                    \
    return 0;                                                                           \
  } while (0)

#endif /* HML_T_M4_H */
