/*
 * fuzz_replay -- the deterministic driver for the hml fuzz harnesses.
 *
 *   fuzz_replay <harness> [--list] <input>...
 *   fuzz_replay <harness> --corpus <dir>...     every file under <dir>
 *   fuzz_replay <harness> --mutate N [--seed S] [--seconds T]
 *
 * The harnesses are standalone mains that take a single input path, so replay
 * needs no coverage instrumentation and no libFuzzer: it re-executes the harness
 * binary once per input and collects the result. That is slower than an in-process
 * loop but it is exactly what a CI regression run wants -- the exit code, the
 * corpus file name and the harness name are all that is needed to reproduce a
 * finding, and the process boundary means a crash cannot take the driver with it.
 * In --mutate mode there is one child, not one per input: the harness does its own
 * in-process mutation loop, so the driver only has to hand it a seed and a budget.
 *
 * Determinism: no clock and no entropy are consulted. --mutate runs the harness's
 * own --selfmutate with a fixed --seed, so two runs of the same command produce
 * the same inputs in the same order.
 *
 * Exit codes: 0 every input ran clean, 3 at least one input produced a finding
 * (the first failing input is printed), 4 the driver could not run the harness.
 */

#include "hml_fuzz.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_INPUTS 20000

static char *g_harness_path;
static char *g_inputs[MAX_INPUTS];
static unsigned g_ninputs;

static void add_input(const char *path) {
  if (g_ninputs >= MAX_INPUTS) {
    fprintf(stderr, "fuzz_replay: more than %u inputs, refusing to grow the list\n",
            (unsigned)MAX_INPUTS);
    exit(HML_FUZZ_EXIT_ERROR);
  }
  g_inputs[g_ninputs] = strdup(path);
  if (g_inputs[g_ninputs] == NULL) exit(HML_FUZZ_EXIT_ERROR);
  g_ninputs++;
}

static int cmp_str(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Depth-first, sorted, so the replay order is a property of the tree, not of the
 * order the filesystem happens to hand back. */
static void collect(const char *dir) {
  DIR *d = opendir(dir);
  if (d == NULL) {
    fprintf(stderr, "fuzz_replay: cannot open %s: %s\n", dir, strerror(errno));
    exit(HML_FUZZ_EXIT_ERROR);
  }
  char *subdirs[256];
  char *files[1024];
  unsigned nsub = 0u, nfile = 0u;

  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)) {
      fprintf(stderr, "fuzz_replay: path too long under %s\n", dir);
      exit(HML_FUZZ_EXIT_ERROR);
    }
    struct stat sb;
    if (stat(path, &sb) != 0) {
      fprintf(stderr, "fuzz_replay: cannot stat %s: %s\n", path, strerror(errno));
      exit(HML_FUZZ_EXIT_ERROR);
    }
    if (S_ISDIR(sb.st_mode)) {
      if (nsub >= sizeof(subdirs) / sizeof(subdirs[0])) {
        fprintf(stderr, "fuzz_replay: more than 256 subdirectories under %s\n", dir);
        exit(HML_FUZZ_EXIT_ERROR);
      }
      subdirs[nsub] = strdup(path);
      if (subdirs[nsub] == NULL) exit(HML_FUZZ_EXIT_ERROR);
      nsub++;
    } else if (S_ISREG(sb.st_mode)) {
      if (nfile >= sizeof(files) / sizeof(files[0])) {
        fprintf(stderr, "fuzz_replay: more than 1024 files in %s\n", dir);
        exit(HML_FUZZ_EXIT_ERROR);
      }
      files[nfile] = strdup(path);
      if (files[nfile] == NULL) exit(HML_FUZZ_EXIT_ERROR);
      nfile++;
    }
  }
  (void)closedir(d);

  qsort(files, nfile, sizeof(files[0]), cmp_str);
  for (unsigned i = 0; i < nfile; i++) {
    add_input(files[i]);
    free(files[i]);
  }
  qsort(subdirs, nsub, sizeof(subdirs[0]), cmp_str);
  for (unsigned i = 0; i < nsub; i++) {
    collect(subdirs[i]);
    free(subdirs[i]);
  }
}

static int run_one(const char *arg) {
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "fuzz_replay: fork failed: %s\n", strerror(errno));
    exit(HML_FUZZ_EXIT_ERROR);
  }
  if (pid == 0) {
    (void)execv(g_harness_path, (char *const[]){(char *)g_harness_path, (char *)arg, NULL});
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) exit(HML_FUZZ_EXIT_ERROR);
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return HML_FUZZ_EXIT_ERROR;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr,
            "usage: %s <harness> <input>...\n"
            "       %s <harness> --corpus <dir>...\n"
            "       %s <harness> --mutate N [--seed S] [--seconds S]\n",
            argv[0], argv[0], argv[0]);
    return HML_FUZZ_EXIT_USAGE;
  }
  g_harness_path = argv[1];

  long mutate = 0;
  uint32_t seed = 0u;
  double seconds = 0.0;
  bool list = false;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--corpus") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "fuzz_replay: --corpus needs a directory\n");
        return HML_FUZZ_EXIT_USAGE;
      }
      collect(argv[++i]);
    } else if (strcmp(argv[i], "--mutate") == 0 && i + 1 < argc) {
      mutate = strtol(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = (uint32_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      seconds = (double)strtod(argv[++i], NULL);
    } else if (strcmp(argv[i], "--list") == 0) {
      list = true;
    } else {
      add_input(argv[i]);
    }
  }
  if (g_ninputs == 0 && mutate <= 0) {
    fprintf(stderr, "fuzz_replay: no inputs\n");
    return HML_FUZZ_EXIT_USAGE;
  }

  if (list) {
    for (unsigned i = 0; i < g_ninputs; i++) printf("%s\n", g_inputs[i]);
    return HML_FUZZ_EXIT_OK;
  }

  if (mutate > 0) {
    char sbuf[32], ibuf[32], tbuf[64];
    /* harness, --selfmutate, --iters, N, --seed, S, --seconds, T, NULL */
    const char *args[9];
    unsigned na = 0;
    args[na++] = g_harness_path;
    args[na++] = (const char *)"--selfmutate";
    (void)snprintf(ibuf, sizeof(ibuf), "%ld", mutate);
    args[na++] = (const char *)"--iters";
    args[na++] = ibuf;
    (void)snprintf(sbuf, sizeof(sbuf), "%lu", (unsigned long)seed);
    args[na++] = (const char *)"--seed";
    args[na++] = sbuf;
    (void)snprintf(tbuf, sizeof(tbuf), "%.1f", seconds);
    args[na++] = (const char *)"--seconds";
    args[na++] = tbuf;
    args[na] = NULL;

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0 || (pid == 0 && execv(g_harness_path, (char *const *)args) != 0)) {
      fprintf(stderr, "fuzz_replay: cannot exec %s\n", g_harness_path);
      return HML_FUZZ_EXIT_ERROR;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
      if (errno != EINTR) return HML_FUZZ_EXIT_ERROR;
    }
    if (WIFSIGNALED(status)) {
      fprintf(stderr, "fuzz_replay: mutation run died on signal %d\n", WTERMSIG(status));
      return 128 + WTERMSIG(status);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : HML_FUZZ_EXIT_ERROR;
  }

  unsigned clean = 0u, findings = 0u, errors = 0u;
  const char *first_bad = NULL;
  int first_rc = 0;
  for (unsigned i = 0; i < g_ninputs; i++) {
    int rc = run_one(g_inputs[i]);
    if (rc == HML_FUZZ_EXIT_OK) {
      clean++;
      continue;
    }
    if (rc == HML_FUZZ_EXIT_FINDING) {
      findings++;
    } else if (rc >= 128 && rc < 128 + 64) {
      /* The harness installs handlers for every fatal signal, so a signal exit
       * means the handler did not run: still a finding, just a louder one. */
      fprintf(stderr, "fuzz_replay: %s exited on signal %d (harness guard did not catch it)\n",
              g_inputs[i], rc - 128);
      findings++;
    } else {
      fprintf(stderr, "fuzz_replay: %s exited %d\n", g_inputs[i], rc);
      errors++;
    }
    if (first_bad == NULL) {
      first_bad = g_inputs[i];
      first_rc = rc;
    }
  }

  printf("fuzz_replay: harness=%s inputs=%u clean=%u findings=%u errors=%u\n", argv[1],
         g_ninputs, clean, findings, errors);
  if (first_bad != NULL) {
    fprintf(stderr, "fuzz_replay: FIRST FAILING INPUT %s (exit %d)\n", first_bad, first_rc);
    return HML_FUZZ_EXIT_FINDING;
  }
  return errors > 0u ? HML_FUZZ_EXIT_ERROR : HML_FUZZ_EXIT_OK;
}
