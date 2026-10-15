#ifndef HML_T_M3_H
#define HML_T_M3_H

#include <stdio.h>

static int g_checks = 0;
static int g_fails = 0;

#define T_CHECK(cond, ...)                                 \
  do {                                                     \
    g_checks++;                                            \
    if (!(cond)) {                                         \
      g_fails++;                                           \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                        \
      fprintf(stderr, "\n");                               \
    }                                                      \
  } while (0)

#define T_DONE(name)                                                                    \
  do {                                                                                  \
    if (g_fails) {                                                                      \
      fprintf(stderr, "FAIL %s: %d of %d checks failed\n", (name), g_fails, g_checks);  \
      return 1;                                                                         \
    }                                                                                   \
    printf("PASS %s: %d checks\n", (name), g_checks);                                   \
    return 0;                                                                           \
  } while (0)

#endif /* HML_T_M3_H */
