#ifndef HML_TEST_UTIL_H
#define HML_TEST_UTIL_H

#include <stdio.h>
#include <string.h>

/* Tiny check harness: every test returns 0 on success and prints FAIL lines. */
static int g_fails;

#define CHECK(cond)                                                                \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                      \
      g_fails++;                                                                   \
    }                                                                              \
  } while (0)

#define CHECK_EQ_U32(got, want)                                                    \
  do {                                                                             \
    unsigned long g_ = (unsigned long)(got), w_ = (unsigned long)(want);           \
    if (g_ != w_) {                                                                \
      printf("FAIL %s:%d: %s == 0x%08lX, want 0x%08lX\n", __FILE__, __LINE__, #got, g_, w_); \
      g_fails++;                                                                   \
    }                                                                              \
  } while (0)

#define CHECK_EQ_SIZE(got, want)                                                   \
  do {                                                                             \
    size_t g_ = (size_t)(got), w_ = (size_t)(want);                                \
    if (g_ != w_) {                                                                \
      printf("FAIL %s:%d: %s == %zu, want %zu\n", __FILE__, __LINE__, #got, g_, w_); \
      g_fails++;                                                                   \
    }                                                                              \
  } while (0)

#define CHECK_STATUS(expr, want)                                                   \
  do {                                                                             \
    hml_status s_ = (expr);                                                        \
    if (s_ != (want)) {                                                            \
      printf("FAIL %s:%d: %s == %d, want %d\n", __FILE__, __LINE__, #expr, (int)s_, \
             (int)(want));                                                         \
      g_fails++;                                                                   \
    }                                                                              \
  } while (0)

#define TEST_DONE()                                                                \
  do {                                                                             \
    if (g_fails) {                                                                 \
      printf("FAILED %s: %d check(s)\n", __FILE__, g_fails);                       \
      return 1;                                                                    \
    }                                                                              \
    printf("PASS %s\n", __FILE__);                                                 \
    return 0;                                                                      \
  } while (0)

#endif /* HML_TEST_UTIL_H */
