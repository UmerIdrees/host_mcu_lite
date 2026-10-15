/*
 * M3 / T1a -- drives the two real hml_gen_roce / hml_host_roce binaries (and,
 * in fallback mode, the plain T0 hml_host) as separate processes, exactly the
 * way a person would from two terminals.
 *
 *   test_roce_cli <gen_binary> <host_binary> [udp_port] [mode]
 *
 * mode = "roce"      (default) hml_gen_roce -> hml_host_roce, socket-crafted T1a
 * mode = "fallback"            hml_gen_roce --fallback-t0 -> hml_host, T0
 *
 * Both children's stdout (with stderr merged in) is read through a pipe, so the
 * assertions are made on what the tools really printed. Every wait is bounded by
 * a deadline and any surviving child is killed, so ctest cannot hang here.
 *
 * The fallback run needs the RoCEv2 open to fail for a reason the tool cannot
 * avoid, so the parent holds a plain UDP socket on the source port: an hml_sock
 * bind (which sets SO_REUSEADDR) onto an already-claimed port fails with
 * EADDRINUSE. The held source port and the T0 data port are deliberately
 * different, so the receiver can still bind.
 */

#include "hml_t_m3.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>

#define CLI_DEFAULT_PORT 14793u
#define CLI_FALLBACK_PORT 14893u
#define CLI_FRAMES 3u
#define CLI_SIZE 9000u
#define CLI_CHUNKS 7u /* ceil(9000 / HML_MAX_FRAG_PAYLOAD) */
#define CLI_WINDOW_BASE 0x100000ull
#define CLI_RKEY 0xDEADBEEFu
#define CLI_DEADLINE_MS 30000
#define CLI_READY_MS 3000
#define CLI_FALLBACK_READY_MS 400
#define CLI_MAX_CAPTURE 65536u

typedef struct capture {
  pid_t pid;
  int fd;
  char buf[CLI_MAX_CAPTURE];
  size_t len;
} capture;

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* fork + execv with stdout and stderr both on the pipe. */
static void spawn(capture *c, const char *path, char *const argv[]) {
  c->fd = -1;
  c->len = 0;
  int pfd[2];
  if (pipe(pfd) != 0) {
    fprintf(stderr, "pipe: %s\n", strerror(errno));
    exit(1);
  }
  c->pid = fork();
  if (c->pid < 0) {
    fprintf(stderr, "fork: %s\n", strerror(errno));
    exit(1);
  }
  if (c->pid == 0) {
    (void)dup2(pfd[1], STDOUT_FILENO);
    (void)dup2(pfd[1], STDERR_FILENO);
    (void)close(pfd[0]);
    (void)close(pfd[1]);
    execv(path, argv);
    fprintf(stderr, "execv %s: %s\n", path, strerror(errno));
    _exit(127);
  }
  (void)close(pfd[1]);
  (void)fcntl(pfd[0], F_SETFL, O_NONBLOCK);
  c->fd = pfd[0];
}

/* Non-blocking read of whatever is queued; 1 if bytes were added, 0 otherwise. */
static int drain(capture *c) {
  if (c->fd < 0) return 0;
  char tmp[4096];
  ssize_t r = read(c->fd, tmp, sizeof(tmp));
  if (r > 0) {
    size_t room = sizeof(c->buf) - 1 - c->len;
    size_t cp = ((size_t)r < room) ? (size_t)r : room;
    memcpy(c->buf + c->len, tmp, cp);
    c->len += cp;
    return 1;
  }
  if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
    (void)close(c->fd);
    c->fd = -1;
  }
  return 0;
}

/* Pump both pipes until both children exit or the deadline passes. */
static void pump(capture *a, capture *b, int64_t deadline) {
  while ((a->pid > 0 || b->pid > 0) && now_ms() < deadline) {
    if (a->fd < 0 && b->fd < 0) break;
    struct pollfd pfd[2];
    int n = 0;
    if (a->fd >= 0) pfd[n++] = (struct pollfd){a->fd, POLLIN, 0};
    if (b->fd >= 0) pfd[n++] = (struct pollfd){b->fd, POLLIN, 0};
    (void)poll(pfd, (nfds_t)n, 50);
    (void)drain(a);
    (void)drain(b);
  }
}

static void finish(capture *c, int *status) {
  c->buf[c->len] = '\0';
  if (c->pid > 0) {
    (void)kill(c->pid, SIGKILL); /* a pipe that is still open means it hung */
    (void)waitpid(c->pid, status, 0);
    c->pid = -1;
  }
  if (c->fd >= 0) {
    (void)close(c->fd);
    c->fd = -1;
  }
}

static int exit_code(int status) { return WIFEXITED(status) ? WEXITSTATUS(status) : -1; }

/* ---- output parsing ------------------------------------------------------ */

/*
 * Every lookup is line-scoped on purpose: the tools print several lines that
 * carry the same field name (per-frame hash= and crc=, the stats line and the
 * host summary line), and a plain strstr() would happily match a key that only
 * exists several lines further down, then parse a value out of that other line.
 */
static const char *line_strstr(const char *s, const char *key) {
  const size_t klen = strlen(key);
  for (const char *p = s; *p && *p != '\n'; p++) {
    if (strncmp(p, key, klen) == 0) return p;
  }
  return NULL;
}

/* Value position of `key` within the line at `line`, or NULL. */
static const char *field_after(const char *line, const char *key) {
  const char *p = line_strstr(line, key);
  if (!p) return NULL;
  p += strlen(key);
  while (*p == ' ') p++;
  return p;
}

static bool field_hex(const char *line, const char *key, uint32_t *out) {
  const char *p = field_after(line, key);
  if (!p) return false;
  if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) return false;
  char *end = NULL;
  unsigned long v = strtoul(p + 2, &end, 16); /* stops at the first non-hex byte */
  if (end == p + 2) return false;
  *out = (uint32_t)v;
  return true;
}

static bool field_u64(const char *line, const char *key, uint64_t *out) {
  const char *p = field_after(line, key);
  if (!p) return false;
  char *end = NULL;
  unsigned long long v = strtoull(p, &end, 0);
  if (end == p) return false;
  *out = (uint64_t)v;
  return true;
}

/* First line containing `line_key`, or NULL. */
static const char *find_line(const char *hay, const char *line_key) {
  const char *line = hay;
  while (line && *line) {
    if (line_strstr(line, line_key)) return line;
    const char *nl = strchr(line, '\n');
    line = nl ? nl + 1 : NULL;
  }
  return NULL;
}

/* One field from the first line containing `line_key`. */
static bool line_u64(const char *hay, const char *line_key, const char *key, uint64_t *out) {
  const char *line = find_line(hay, line_key);
  return line && field_u64(line, key, out);
}

static bool line_hex(const char *hay, const char *line_key, const char *key, uint32_t *out) {
  const char *line = find_line(hay, line_key);
  return line && field_hex(line, key, out);
}

/*
 * Collect `key`'s 0x-prefixed 32-bit value from every line containing
 * `line_key`, in emission order.
 */
static int collect_line_hex(const char *hay, const char *line_key, const char *key,
                           uint32_t *out, int cap) {
  int n = 0;
  const char *line = hay;
  while (line && *line && n < cap) {
    if (line_strstr(line, line_key) && field_hex(line, key, &out[n])) n++;
    const char *nl = strchr(line, '\n');
    line = nl ? nl + 1 : NULL;
  }
  return n;
}

/* Print whatever the children produced, so a failure is diagnosable. */
static void dump(const char *tag, const capture *c, int status) {
  fprintf(stderr, "---- %s (exit %d) ----\n%s", tag, exit_code(status), c->buf);
  if (c->len == 0) fprintf(stderr, "(no output)\n");
  fprintf(stderr, "---- end %s ----\n", tag);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <hml_gen_roce> <host_binary> [udp_port] [roce|fallback]\n",
            argv[0]);
    return 2;
  }
  const char *gen_path = argv[1];
  const char *host_path = argv[2];
  const bool fallback = (argc >= 5 && strcmp(argv[4], "fallback") == 0);

  char portbuf[16];
  if (argc >= 4) snprintf(portbuf, sizeof(portbuf), "%s", argv[3]);
  else snprintf(portbuf, sizeof(portbuf), "%u", fallback ? CLI_FALLBACK_PORT : CLI_DEFAULT_PORT);
  const uint16_t data_port = (uint16_t)strtoul(portbuf, NULL, 10);
  const uint16_t hold_port = (uint16_t)(data_port + 1u);

  char winbuf[32], rkeybuf[16], framesbuf[16], sizebuf[16], srcbuf[16];
  snprintf(winbuf, sizeof(winbuf), "0x%" PRIx64, (uint64_t)CLI_WINDOW_BASE);
  snprintf(rkeybuf, sizeof(rkeybuf), "0x%" PRIx32, (uint32_t)CLI_RKEY);
  snprintf(framesbuf, sizeof(framesbuf), "%u", (unsigned)CLI_FRAMES);
  snprintf(sizebuf, sizeof(sizebuf), "%u", (unsigned)CLI_SIZE);
  snprintf(srcbuf, sizeof(srcbuf), "%u", (unsigned)hold_port);

  /* Hold the source port so the RoCEv2 open cannot succeed in fallback mode. */
  int holder = -1;
  if (fallback) {
    holder = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(0);
    sa.sin_port = htons(hold_port);
    /* Deliberately no SO_REUSEADDR: that is what makes the hml_sock bind fail. */
    if (holder < 0 || bind(holder, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
      fprintf(stderr, "could not hold udp/%u: %s\n", (unsigned)hold_port, strerror(errno));
      return 1;
    }
  }

  char *host_argv_fallback[] = {(char *)host_path, (char *)"--port",    portbuf,
                                (char *)"--frames", framesbuf, (char *)"--verbose", NULL};
  char *host_argv_roce[] = {(char *)host_path, (char *)"--port",         portbuf,
                            (char *)"--window-base", winbuf,           (char *)"--window-len",
                            (char *)"65536",       (char *)"--frame-size", sizebuf,
                            (char *)"--rkey",      rkeybuf,            (char *)"--frames",
                            framesbuf,             (char *)"--verbose", NULL};
  char *gen_argv_fallback[] = {(char *)gen_path,    (char *)"--fallback-t0",
                               (char *)"--port",    portbuf,
                               (char *)"--src-port", srcbuf, /* the held port */
                               (char *)"--frames",  framesbuf,
                               (char *)"--size",    sizebuf,
                               (char *)"--window-base", winbuf,
                               (char *)"--rkey",    rkeybuf,
                               (char *)"--seed",    (char *)"1",
                               (char *)"--verbose", NULL};
  char *gen_argv_roce[] = {(char *)gen_path,         (char *)"--port",
                           (char *)portbuf,          (char *)"--frames",
                           (char *)framesbuf,        (char *)"--size",
                           (char *)sizebuf,          (char *)"--frame-size",
                           (char *)sizebuf,          (char *)"--window-base",
                           (char *)winbuf,           (char *)"--rkey",
                           (char *)rkeybuf,          (char *)"--psn",
                           (char *)"1",              (char *)"--seed",
                           (char *)"1",              (char *)"--verbose",
                           NULL};

  capture host;
  capture gen;
  memset(&host, 0, sizeof(host));
  memset(&gen, 0, sizeof(gen));
  host.pid = -1;
  gen.pid = -1;

  const int64_t deadline = now_ms() + CLI_DEADLINE_MS;
  spawn(&host, host_path, fallback ? host_argv_fallback : host_argv_roce);

  /* Wait for the receiver to be listening: an hml_host_roce that has not bound
   * yet would silently swallow frame 0. hml_host prints no such line, so the T0
   * run gets a fixed pause instead -- a UDP datagram to a port nobody has bound
   * is dropped, so being early costs frames just as much as being late. Either
   * way the wait is bounded and a late bind fails the assertions below loudly. */
  const int64_t started = now_ms();
  const int64_t ready_deadline = fallback ? started + CLI_FALLBACK_READY_MS : started + CLI_READY_MS;
  while (host.fd >= 0 && now_ms() < ready_deadline) {
    if (!fallback && strstr(host.buf, "listening") != NULL) break;
    struct pollfd pfd = {host.fd, POLLIN, 0};
    (void)poll(&pfd, 1, 50);
    (void)drain(&host);
  }

  spawn(&gen, gen_path, fallback ? gen_argv_fallback : gen_argv_roce);
  pump(&host, &gen, deadline);

  int host_status = 0, gen_status = 0;
  finish(&host, &host_status);
  finish(&gen, &gen_status);
  if (holder >= 0) (void)close(holder);

  /* ---- the generator must succeed ---------------------------------------- */
  T_CHECK(exit_code(gen_status) == 0, "hml_gen_roce exit %d (signal %d)", exit_code(gen_status),
          WIFEXITED(gen_status) ? 0 : WTERMSIG(gen_status));
  uint64_t gen_frames = 0;
  T_CHECK(line_u64(gen.buf, "sent frames=", "sent frames=", &gen_frames),
          "no generator summary line");
  T_CHECK(gen_frames == CLI_FRAMES, "generator sent frames=%" PRIu64 ", want %u", gen_frames,
          (unsigned)CLI_FRAMES);
  T_CHECK(strstr(gen.buf, "chunks=") != NULL, "per-frame line is missing chunks=");

  uint32_t gen_crc[CLI_FRAMES];
  uint32_t gen_hash[CLI_FRAMES];
  const int n_gen_crc =
      collect_line_hex(gen.buf, "sent frame=", "crc=", gen_crc, (int)CLI_FRAMES);
  const int n_gen_hash =
      collect_line_hex(gen.buf, "sent frame=", "hash=", gen_hash, (int)CLI_FRAMES);
  T_CHECK(n_gen_crc == (int)CLI_FRAMES, "generator printed %d per-frame crcs, want %u", n_gen_crc,
          (unsigned)CLI_FRAMES);
  T_CHECK(n_gen_hash == (int)CLI_FRAMES, "generator printed %d per-frame hashes, want %u",
          n_gen_hash, (unsigned)CLI_FRAMES);
  T_CHECK(exit_code(host_status) == 0, "receiver exit %d", exit_code(host_status));

  if (fallback) {
    T_CHECK(strstr(gen.buf, "fallback: roce open failed, using T0 (udp)") != NULL,
            "generator did not report the T0 fallback");
    uint64_t host_frames = 0, crc_fail = 1, gaps = 1, dropped = 1;
    T_CHECK(line_u64(host.buf, "summary ", "frames=", &host_frames), "no hml_host summary line");
    T_CHECK(line_u64(host.buf, "reasm ", "crc_fail=", &crc_fail), "no hml_host reasm line");
    T_CHECK(line_u64(host.buf, "reasm ", "gaps=", &gaps), "no hml_host reasm line");
    T_CHECK(line_u64(host.buf, "reasm ", "frames_dropped=", &dropped), "no hml_host reasm line");
    T_CHECK(host_frames == CLI_FRAMES, "hml_host delivered %" PRIu64 " frames, want %u",
            host_frames, (unsigned)CLI_FRAMES);
    T_CHECK(crc_fail == 0 && gaps == 0 && dropped == 0,
            "reasm crc_fail=%" PRIu64 " gaps=%" PRIu64 " frames_dropped=%" PRIu64, crc_fail, gaps,
            dropped);

    uint32_t host_hash[CLI_FRAMES];
    const int n_host =
        collect_line_hex(host.buf, "frame seq=", "hash=", host_hash, (int)CLI_FRAMES);
    T_CHECK(n_host == (int)CLI_FRAMES, "hml_host printed %d frame lines, want %u", n_host,
            (unsigned)CLI_FRAMES);
    int match = 0;
    for (int i = 0; i < n_host && i < n_gen_hash; i++) {
      if (host_hash[i] == gen_hash[i]) match++;
    }
    T_CHECK(match == (int)CLI_FRAMES, "only %d of %d T0 hashes match sender to receiver", match,
            (int)CLI_FRAMES);
    printf("  fallback: explicit fallback line printed, hml_host delivered %" PRIu64
           " frames, %d/%d hashes match\n",
           host_frames, match, (int)CLI_FRAMES);
  } else {
    uint64_t eof = 0, icrc_err = 1, meta_drops = 1, proto_err = 1, bad_crc = 1, bad_len = 1;
    T_CHECK(line_u64(host.buf, "stats ", "eof_frames=", &eof), "no hml_host_roce stats line");
    T_CHECK(line_u64(host.buf, "stats ", "icrc_err=", &icrc_err), "no icrc_err=");
    T_CHECK(line_u64(host.buf, "stats ", "meta_drops=", &meta_drops), "no meta_drops=");
    T_CHECK(line_u64(host.buf, "stats ", "proto_err=", &proto_err), "no proto_err=");
    T_CHECK(line_u64(host.buf, "host ", "bad_crc=", &bad_crc), "no bad_crc=");
    T_CHECK(line_u64(host.buf, "host ", "bad_len=", &bad_len), "no bad_len=");
    T_CHECK(eof == gen_frames, "hml_host_roce eof_frames=%" PRIu64 ", generator sent %" PRIu64,
            eof, gen_frames);
    T_CHECK(icrc_err == 0, "icrc_err=%" PRIu64 ", want 0", icrc_err);
    T_CHECK(proto_err == 0, "proto_err=%" PRIu64 ", want 0", proto_err);
    T_CHECK(meta_drops == 0, "meta_drops=%" PRIu64 ", want 0", meta_drops);
    T_CHECK(bad_crc == 0, "bad_crc=%" PRIu64 ", want 0", bad_crc);
    T_CHECK(bad_len == 0, "bad_len=%" PRIu64 ", want 0", bad_len);

    uint32_t host_crc[CLI_FRAMES];
    const int n_host =
        collect_line_hex(host.buf, "frame seq=", "crc=", host_crc, (int)CLI_FRAMES);
    T_CHECK(n_host == (int)CLI_FRAMES, "hml_host_roce printed %d frame lines, want %u", n_host,
            (unsigned)CLI_FRAMES);
    int match = 0;
    for (int i = 0; i < n_host; i++) {
      for (int j = 0; j < n_gen_crc; j++) {
        if (host_crc[i] == gen_crc[j]) match++;
      }
    }
    T_CHECK(match > 0, "no hml_host_roce crc=0x line matches a generator frame crc");

    /* The verbose tap has to agree with the packet the demux actually saw. */
    uint32_t tap_psn = 0, tap_rkey = 0;
    T_CHECK(line_hex(gen.buf, "pkt0", "psn=", &tap_psn), "no pkt0 psn in the generator output");
    T_CHECK(line_hex(gen.buf, "pkt0", "rkey=", &tap_rkey), "no pkt0 rkey in the generator output");
    T_CHECK(tap_psn == 1u, "pkt0 psn=0x%06" PRIx32 ", want 0x000001", tap_psn);
    T_CHECK(tap_rkey == CLI_RKEY, "pkt0 rkey=0x%08" PRIx32 ", want 0x%08" PRIx32, tap_rkey,
            CLI_RKEY);
    uint64_t psn_first = 0, psn_last = 0;
    T_CHECK(line_u64(gen.buf, "sent frames=", "psn_first=", &psn_first), "no psn_first=");
    T_CHECK(line_u64(gen.buf, "sent frames=", "psn_last=", &psn_last), "no psn_last=");
    T_CHECK(psn_first == 1u, "psn_first=%" PRIu64 ", want 1", psn_first);
    T_CHECK(psn_last == psn_first + gen_frames * (CLI_CHUNKS + 1u) - 1u,
            "psn_last=%" PRIu64 " is not the last packet of %" PRIu64 " frames", psn_last,
            gen_frames);

    printf("  roce: eof_frames=%" PRIu64 " icrc_err=%" PRIu64 " %d/%d frame crcs match, "
           "psn %" PRIu64 "..%" PRIu64 "\n",
           eof, icrc_err, n_host, (int)CLI_FRAMES, psn_first, psn_last);
  }

  if (g_fails) {
    dump(fallback ? "hml_gen_roce (fallback)" : "hml_gen_roce", &gen, gen_status);
    dump(fallback ? "hml_host" : "hml_host_roce", &host, host_status);
  }

  T_DONE(fallback ? "test_roce_cli[fallback]" : "test_roce_cli");
}
