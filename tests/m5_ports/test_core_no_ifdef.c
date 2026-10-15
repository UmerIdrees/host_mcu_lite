/*
 * M5 / test_core_no_ifdef.c -- the portability rule, enforced mechanically.
 *
 * The plan's rule: portability lives in ports/, not in the shared source. A
 * #if inside src/ is how one platform's idea of the world leaks into every
 * other platform, so the shared tree may not contain one.
 *
 * What is scanned: every .c and .h under src/ (HML_M5_SRC_ROOT, passed by the
 * CMake file). Flagged, with file:line:
 *   - any preprocessor conditional: #if, #ifdef, #ifndef (so the spec's "#if
 *     defined" is covered, plus the bare "#if __linux__" form it is meant to
 *     catch)
 *   - an include of unistd.h, sys/socket.h, or any header under lwip / freertos
 *     or named esp_something
 *   - the strings "posix" or "FreeRTOS" (case-insensitive) anywhere in src/core
 *
 * Two universal C idioms are explicitly allowed, because they are language
 * boilerplate and not platform conditionals:
 *   1. an include guard: "#ifndef X" immediately followed by "#define X"
 *   2. "#ifdef __cplusplus" / "#if defined(__cplusplus)" around extern "C" {
 *
 * Everything else is a violation. No shell, no scripting language: plain C11
 * with <dirent.h> so the test runs identically on any host that builds the
 * project.
 */

#include "hml_t_m5.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef HML_M5_SRC_ROOT
#define HML_M5_SRC_ROOT "src"
#endif

#define HML_M5_MAX_PATH 1024
#define HML_M5_MAX_LINE 4096

/* ---- small text helpers ------------------------------------------------- */

static int ci_contains(const char *hay, const char *needle) {
  size_t nl = strlen(needle);
  size_t i;
  if (nl == 0) return 1;
  for (i = 0; hay[i] != '\0'; i++) {
    size_t j = 0;
    while (j < nl && hay[i + j] != '\0') {
      char a = hay[i + j];
      char b = needle[j];
      if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
      if (a != b) break;
      j++;
    }
    if (j == nl) return 1;
  }
  return 0;
}

static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\r') p++;
  return p;
}

static int ident_eq(const char *a, const char *b) {
  for (; *a != '\0' && *b != '\0'; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return 0;
  }
  return *a == *b;
}

/* Copy the trimmed remainder of a directive line. */
static void tail_of(const char *line, char *out, size_t cap) {
  size_t k = 0;
  const char *p = skip_ws(line);
  while (*p != '\0' && *p != '\r' && *p != '\n' && k + 1 < cap) out[k++] = *p++;
  out[k] = '\0';
}

/* ---- whole-file loader -------------------------------------------------- */

typedef struct doc {
  char *text;   /* whole file; every doc.line entry points into it */
  char **line;  /* NUL-terminated lines, split in place out of text */
  size_t n;
} doc;

static void doc_free(doc *d) {
  free(d->line);
  free(d->text);
  d->line = NULL;
  d->text = NULL;
  d->n = 0;
}

static int doc_load(const char *path, doc *d) {
  FILE *f = fopen(path, "rb");
  size_t cap = 1 << 16, len = 0, count = 1;
  size_t i;
  char *p;
  if (f == NULL) return 0;
  d->text = (char *)malloc(cap);
  if (d->text == NULL) {
    fclose(f);
    return 0;
  }
  for (;;) {
    size_t got;
    if (len + 4096 + 1 > cap) { /* +1 for the NUL we append below */
      char *nt;
      cap *= 2;
      nt = (char *)realloc(d->text, cap);
      if (nt == NULL) {
        fclose(f);
        free(d->text);
        return 0;
      }
      d->text = nt;
    }
    got = fread(d->text + len, 1, 4096, f);
    len += got;
    if (got < 4096) break;
  }
  fclose(f);
  d->text[len] = '\0';

  for (i = 0; i < len; i++) {
    if (d->text[i] == '\n') count++;
  }
  d->line = (char **)calloc(count + 1, sizeof(char *));
  if (d->line == NULL) {
    free(d->text);
    d->text = NULL;
    return 0;
  }
  d->n = 0;
  p = d->text;
  for (i = 0; i < count; i++) {
    char *nl = strchr(p, '\n');
    if (nl != NULL) *nl = '\0';
    d->line[d->n++] = p;
    if (nl == NULL) break;
    p = nl + 1;
  }
  return 1;
}

/* Index of the next line that is neither blank nor a C comment. */
static long next_code_line(const doc *d, long from) {
  long i;
  for (i = from; i < (long)d->n; i++) {
    const char *t = skip_ws(d->line[i]);
    if (*t == '\0') continue;
    if (t[0] == '/' && (t[1] == '/' || t[1] == '*')) continue;
    return i;
  }
  return -1;
}

/* ---- the checks --------------------------------------------------------- */

/* Return non-zero when the line is a preprocessor conditional that the rule
 * does not allow. *why receives a short reason. */
static int bad_conditional(const doc *d, size_t idx, char *why, size_t why_cap) {
  const char *l = skip_ws(d->line[idx]);
  const char *rest;
  char cond[HML_M5_MAX_LINE];
  if (l[0] != '#') return 0;
  l = skip_ws(l + 1);
  if (strncmp(l, "if", 2) != 0) return 0; /* else/endif/ifdef handled below */
  if (l[2] == '\0' || l[2] == ' ' || l[2] == '\t') {
    rest = l + 2;
  } else if (strncmp(l, "ifdef", 5) == 0 && (l[5] == '\0' || l[5] == ' ' || l[5] == '\t')) {
    rest = l + 5;
  } else if (strncmp(l, "ifndef", 6) == 0 && (l[6] == '\0' || l[6] == ' ' || l[6] == '\t')) {
    rest = l + 6;
  } else {
    return 0; /* #include, #define, #pragma, ... */
  }
  (void)snprintf(why, why_cap, "%s", (rest == l + 2) ? "preprocessor #if" : "preprocessor #if*");

  tail_of(rest, cond, sizeof(cond));
  if (ci_contains(cond, "__cplusplus")) return 0; /* extern "C" wrapper: allowed */

  if (rest != l + 2) { /* #ifdef / #ifndef with a bare identifier */
    long j = next_code_line(d, (long)idx + 1);
    if (j >= 0) {
      const char *n = skip_ws(d->line[(size_t)j]);
      if (n[0] == '#') {
        n = skip_ws(n + 1);
        if (strncmp(n, "define", 6) == 0) {
          char mac[HML_M5_MAX_LINE];
          tail_of(n + 6, mac, sizeof(mac));
          if (ident_eq(cond, mac)) return 0; /* include guard: allowed */
        }
      }
    }
  }
  return 1;
}

static int bad_include(const char *line, char *why, size_t why_cap) {
  static const char *const banned[] = {"unistd.h", "sys/socket.h", "lwip", "freertos", "esp_"};
  const char *inc = strstr(line, "#include");
  char target[HML_M5_MAX_LINE];
  size_t k = 0;
  char close_ch;
  size_t b;
  if (inc == NULL) return 0;
  inc += 8;
  inc = skip_ws(inc);
  if (*inc == '<') {
    close_ch = '>';
    inc++;
  } else if (*inc == '"') {
    close_ch = '"';
    inc++;
  } else {
    return 0;
  }
  while (*inc != '\0' && *inc != close_ch && k + 1 < sizeof(target)) target[k++] = *inc++;
  target[k] = '\0';
  for (b = 0; b < sizeof(banned) / sizeof(banned[0]); b++) {
    if (ci_contains(target, banned[b])) {
      (void)snprintf(why, why_cap, "include of %.*s", (int)(why_cap > 16u ? why_cap - 16u : 1u), target);
      return 1;
    }
  }
  return 0;
}

static void scan_file(const char *path, int in_core, int *files) {
  doc d;
  size_t i;
  if (!doc_load(path, &d)) {
    T_CHECK(0, "could not read %s", path);
    return;
  }
  (*files)++;
  for (i = 0; i < d.n; i++) {
    char why[HML_M5_MAX_LINE];
    const char *t = skip_ws(d.line[i]);
    if (t[0] == '#') {
      if (bad_conditional(&d, i, why, sizeof(why))) {
        T_CHECK(0, "%s:%zu: %s in shared source: %s", path, i + 1, why, t);
      }
    }
    if (bad_include(d.line[i], why, sizeof(why))) {
      T_CHECK(0, "%s:%zu: %s in shared source: %s", path, i + 1, why, t);
    }
    if (in_core && (ci_contains(d.line[i], "posix") || ci_contains(d.line[i], "freertos"))) {
      T_CHECK(0, "%s:%zu: platform name (\"posix\"/\"FreeRTOS\") in src/core: %s", path, i + 1, t);
    }
  }
  doc_free(&d);
}

/* Absolute path of <src root>/core, built once: the "posix"/"FreeRTOS" string
 * rule applies to src/core and everything below it. */
static char g_core_path[HML_M5_MAX_PATH];

static void scan_dir(const char *dir, int depth, int *files, int in_core) {
  DIR *d = opendir(dir);
  struct dirent *e;

  if (d == NULL) {
    T_CHECK(0, "cannot open directory %s", dir);
    return;
  }
  if (strcmp(dir, g_core_path) == 0) in_core = 1;

  while ((e = readdir(d)) != NULL) {
    char path[HML_M5_MAX_PATH];
    struct stat st;
    size_t n = strlen(e->d_name);
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    if (n + strlen(dir) + 2 >= sizeof(path)) continue;
    (void)snprintf(path, sizeof(path), "%.*s/%s", (int)(sizeof(path) - n - 3u), dir, e->d_name);
    if (stat(path, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (depth < 8) scan_dir(path, depth + 1, files, in_core);
    } else if (S_ISREG(st.st_mode)) {
      const char *dot = strrchr(e->d_name, '.');
      if (dot != NULL && (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0)) {
        scan_file(path, in_core, files);
      }
    }
  }
  closedir(d);
}

int main(void) {
  int files = 0;
  DIR *probe;
  (void)snprintf(g_core_path, sizeof(g_core_path), "%s/core", HML_M5_SRC_ROOT);
  probe = opendir(HML_M5_SRC_ROOT);
  if (probe == NULL) {
    T_CHECK(0, "source root %s not found (is HML_M5_SRC_ROOT set?)", HML_M5_SRC_ROOT);
    T_DONE("test_core_no_ifdef");
  }
  closedir(probe);

  scan_dir(HML_M5_SRC_ROOT, 0, &files, 0);
  T_CHECK(files > 0, "no .c/.h files found under %s", HML_M5_SRC_ROOT);
  if (g_fails == 0) fprintf(stdout, "scanned %d .c/.h files under %s\n", files, HML_M5_SRC_ROOT);
  T_DONE("test_core_no_ifdef");
}
