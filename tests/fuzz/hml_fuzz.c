/*
 * Implementation of the shared fuzz plumbing declared in hml_fuzz.h.
 *
 * Robustness rules this file exists to enforce, per the reliability checklist:
 *   - a fatal signal inside the code under test becomes a FINDING on stdout,
 *     not a raw signal death that loses the input that caused it;
 *   - one input can never hang the harness (SIGALRM watchdog);
 *   - one input can never exhaust the address space unnoticed (RLIMIT_AS plus an
 *     allocation guard that flags a single request above HML_FUZZ_ALLOC_MAX);
 *   - one input is size-capped before anything walks it.
 *
 * The seal_* helpers rebuild a structurally valid record with the library's own
 * serialiser, so the accept paths stay reachable without coverage feedback.
 */

#include "hml_fuzz.h"

#include "hml/hml_crc.h"
#include "hml/hml_ctrl.h"
#include "hml/hml_discovery.h"
#include "hml/hml_port.h"
#include "hml/hml_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__GLIBC__)
#include <malloc.h>
#define HML_FUZZ_HAVE_ALLOC_GUARD 1
/* The guard below interposes on malloc/calloc/realloc/free, so it needs a way to
 * reach the real allocator. glibc exports the __libc_* entry points but does not
 * declare them in any public header, so they are declared here. dlsym(RTLD_NEXT)
 * is the portable-looking alternative and does not work here: dlsym itself
 * allocates, so resolving malloc through it recurses before the guard is armed. */
extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t nmemb, size_t size);
extern void *__libc_realloc(void *p, size_t size);
extern void __libc_free(void *p);
#else
#define HML_FUZZ_HAVE_ALLOC_GUARD 0
#endif

#if defined(__SANITIZE_ADDRESS__)
#define HML_FUZZ_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
#define HML_FUZZ_SANITIZED 1
#else
#define HML_FUZZ_SANITIZED 0
#endif
#else
#define HML_FUZZ_SANITIZED 0
#endif

/* A single allocation above this is treated as a defect: every caller in the
 * library is expected to clamp a caller-supplied size_t to its own maximum
 * before allocating, so nothing legitimate comes close. */
#define HML_FUZZ_ALLOC_MAX (32u * 1024u * 1024u)
/* Address-space ceiling. Comfortably above every legitimate allocation the
 * library makes (2 x HML_MAX_FRAME_SIZE for a reassembler) and far below the
 * absurd sizes the corpus asks for. */
#define HML_FUZZ_AS_LIMIT (256u * 1024u * 1024u)
/* Watchdog for a single input. Nothing legitimate is close to this. */
#define HML_FUZZ_INPUT_ALARM 20u

static const char *g_harness = "harness";
static const char *g_input_path = "<memory>";
static const uint8_t *g_live_data;
static size_t g_live_size;

/* ---- allocation guard --------------------------------------------------- */

#if HML_FUZZ_HAVE_ALLOC_GUARD
void *malloc(size_t size) {
  if (size > HML_FUZZ_ALLOC_MAX) {
    hml_fuzz_finding("malloc(%zu) exceeds the %u byte guard: a caller-supplied size "
                     "reached the allocator unclamped",
                     size, (unsigned)HML_FUZZ_ALLOC_MAX);
  }
  return __libc_malloc(size);
}

void *calloc(size_t nmemb, size_t size) {
  if (nmemb != 0u && size > HML_FUZZ_ALLOC_MAX / nmemb) {
    hml_fuzz_finding("calloc(%zu, %zu) exceeds the %u byte guard: a caller-supplied size "
                     "reached the allocator unclamped",
                     nmemb, size, (unsigned)HML_FUZZ_ALLOC_MAX);
  }
  return __libc_calloc(nmemb, size);
}

void *realloc(void *p, size_t size) {
  if (size > HML_FUZZ_ALLOC_MAX) {
    hml_fuzz_finding("realloc(_, %zu) exceeds the %u byte guard: a caller-supplied size "
                     "reached the allocator unclamped",
                     size, (unsigned)HML_FUZZ_ALLOC_MAX);
  }
  return __libc_realloc(p, size);
}

void free(void *p) { __libc_free(p); }
#endif

/* ---- findings ----------------------------------------------------------- */

static void raw(const char *s) {
  size_t n = strlen(s);
  ssize_t w = write(2, s, n);
  (void)w;
}

static void report_finding(const char *msg) {
  char line[512];
  int k = snprintf(line, sizeof(line), "FINDING harness=%s input=%s bytes=%zu: %s\n",
                   g_harness, g_input_path, g_live_size, msg);
  if (k < 0) k = 0;
  if ((size_t)k >= sizeof(line)) k = (int)sizeof(line) - 1;
  ssize_t w = write(1, line, (size_t)k);
  (void)w;
  raw(line);
  /* Also dump the leading bytes so a finding is usable even when the input has
   * no path on disk (self-mutation mode). */
  if (g_live_data != NULL) {
    char hex[256];
    size_t show = g_live_size < 24u ? g_live_size : 24u;
    int off = snprintf(hex, sizeof(hex), "FINDING bytes:");
    for (size_t i = 0; i < show && off > 0 && (size_t)off + 3u < sizeof(hex); i++)
      off += snprintf(hex + off, sizeof(hex) - (size_t)off, " %02x", g_live_data[i]);
    if (off > 0 && (size_t)off + 1u < sizeof(hex)) {
      snprintf(hex + off, sizeof(hex) - (size_t)off, "\n");
      raw(hex);
    }
  }
}

void hml_fuzz_finding(const char *fmt, ...) {
  /* The allocation guard calls this from inside malloc, and the reporting path
   * itself uses stdio, so a second report must not recurse. */
  static volatile int reporting = 0;
  if (reporting) _exit(HML_FUZZ_EXIT_FINDING);
  reporting = 1;

  char msg[320];
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  report_finding(msg);
  _exit(HML_FUZZ_EXIT_FINDING);
}

static void on_fatal_signal(int sig) {
  const char *name = "fatal signal";
  switch (sig) {
    case SIGSEGV: name = "SIGSEGV (out-of-bounds access or null deref)"; break;
    case SIGBUS: name = "SIGBUS (misaligned or out-of-bounds access)"; break;
    case SIGILL: name = "SIGILL (bad opcode)"; break;
    case SIGFPE: name = "SIGFPE (arithmetic)"; break;
    case SIGABRT: name = "SIGABRT (abort/assert)"; break;
    case SIGSYS: name = "SIGSYS (bad syscall)"; break;
    case SIGTRAP: name = "SIGTRAP"; break;
    case SIGALRM: name = "SIGALRM (per-input hang: the watchdog fired)"; break;
    default: break;
  }
  report_finding(name);
  _exit(HML_FUZZ_EXIT_FINDING);
}

static void install_handler(int sig) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_fatal_signal;
  sigemptyset(&sa.sa_mask);
  (void)sigaction(sig, &sa, NULL);
}

static void install_guards(void) {
  static const int fatal[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS, SIGTRAP,
                              SIGALRM};
  for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++) install_handler(fatal[i]);

  const char *off = getenv("HML_FUZZ_NO_RLIMIT");
  if (off != NULL && off[0] == '1') return;
#if !HML_FUZZ_SANITIZED
  /* A sanitizer reserves an enormous shadow mapping, so an address-space cap
   * would kill the process before it reported anything useful. */
  struct rlimit rl;
  if (getrlimit(RLIMIT_AS, &rl) == 0) {
    if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > (rlim_t)HML_FUZZ_AS_LIMIT) {
      rl.rlim_cur = (rlim_t)HML_FUZZ_AS_LIMIT;
      if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < rl.rlim_cur) rl.rlim_cur = rl.rlim_max;
      (void)setrlimit(RLIMIT_AS, &rl);
    }
  }
#endif
}

static unsigned alarm_seconds(void) {
  const char *e = getenv("HML_FUZZ_INPUT_TIMEOUT");
  if (e != NULL && e[0] != '\0') {
    long v = strtol(e, NULL, 10);
    if (v > 0 && v < 3600) return (unsigned)v;
  }
  return HML_FUZZ_INPUT_ALARM;
}

/* ---- misc --------------------------------------------------------------- */

uint32_t hml_fuzz_rng(uint32_t *state) {
  uint32_t x = *state;
  if (x == 0u) x = 0x9e3779b9u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

size_t hml_fuzz_mutate(const uint8_t *seed, size_t n, uint32_t *rng, uint8_t *out,
                       size_t cap) {
  if (n > cap) n = cap;
  memcpy(out, seed, n);
  size_t m = n;
  unsigned rounds = 1u + (hml_fuzz_rng(rng) % 6u);
  for (unsigned r = 0; r < rounds; r++) {
    unsigned op = hml_fuzz_rng(rng) % 8u;
    switch (op) {
      case 0: /* bit flip */
        if (m > 0u) out[hml_fuzz_rng(rng) % m] ^= (uint8_t)(1u << (hml_fuzz_rng(rng) % 8u));
        break;
      case 1: /* byte set to an extremal value */
        if (m > 0u) {
          static const uint8_t kExtremes[5] = {0x00u, 0xffu, 0x7fu, 0x80u, 0x01u};
          out[hml_fuzz_rng(rng) % m] = kExtremes[hml_fuzz_rng(rng) % 5u];
        }
        break;
      case 2: /* insert */
        if (m + 1u < cap) {
          size_t at = (size_t)(hml_fuzz_rng(rng) % (unsigned)(m + 1u));
          memmove(out + at + 1, out + at, m - at);
          out[at] = (uint8_t)hml_fuzz_rng(rng);
          m++;
        }
        break;
      case 3: /* delete */
        if (m > 1u) {
          size_t at = (size_t)(hml_fuzz_rng(rng) % m);
          memmove(out + at, out + at + 1, m - at - 1u);
          m--;
        }
        break;
      case 4: /* truncate to a prefix */
        if (m > 0u) m = (size_t)(hml_fuzz_rng(rng) % m);
        break;
      case 5: /* duplicate a run */
        if (m > 0u && m * 2u < cap) {
          size_t at = (size_t)(hml_fuzz_rng(rng) % m);
          size_t run = (size_t)(hml_fuzz_rng(rng) % (m - at)) + 1u;
          memmove(out + at + run, out + at, run);
          m += run;
        }
        break;
      case 6: /* write a 32-bit extremal value over a window */
        if (m >= 4u) {
          static const uint32_t kHuge[4] = {0xffffffffu, 0x7fffffffu, 0x80000000u, 0u};
          size_t at = (size_t)(hml_fuzz_rng(rng) % (m - 3u));
          hml_put_be32(out + at, kHuge[hml_fuzz_rng(rng) % 4u]);
        }
        break;
      default: /* overwrite a run with random bytes */
        if (m > 0u && m < cap) {
          size_t at = (size_t)(hml_fuzz_rng(rng) % m);
          size_t run = (size_t)(hml_fuzz_rng(rng) % 8u) + 1u;
          for (size_t i = 0; i < run && at + i < cap; i++)
            out[at + i] = (uint8_t)hml_fuzz_rng(rng);
        }
        break;
    }
    if (m == 0u) { /* never hand an empty buffer to the mutation loop */
      out[0] = (uint8_t)hml_fuzz_rng(rng);
      m = 1u;
    }
  }
  return m;
}

size_t hml_fuzz_reasm_cap(unsigned index) {
  static const size_t kCaps[HML_FUZZ_REASM_NCAPS] = {
      0u,
      1u,
      (size_t)HML_FRAME_HDR_SIZE,
      (size_t)HML_MAX_FRAG_SIZE,
      4096u,
      65536u,
      (size_t)HML_MAX_FRAME_SIZE,
      (size_t)HML_MAX_FRAME_SIZE + 1u,
      0x7fffffffu,
      0x80000000u,
      0xffffffffu,
      SIZE_MAX};
  return index < HML_FUZZ_REASM_NCAPS ? kCaps[index] : 0u;
}

bool hml_fuzz_read_file(const char *path, uint8_t *out, size_t cap, size_t *n) {
  *n = 0;
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  size_t total = 0;
  for (;;) {
    if (total >= cap) break; /* size-capped on purpose */
    ssize_t r = read(fd, out + total, cap - total);
    if (r < 0) {
      if (errno == EINTR) continue;
      (void)close(fd);
      return false;
    }
    if (r == 0) break;
    total += (size_t)r;
  }
  (void)close(fd);
  *n = total;
  return true;
}

int hml_fuzz_mkdir_p(const char *path) {
  char tmp[512];
  size_t n = strlen(path);
  if (n == 0u || n >= sizeof(tmp)) return -1;
  memcpy(tmp, path, n + 1u);
  for (size_t i = 1; i < n; i++) {
    if (tmp[i] != '/') continue;
    tmp[i] = '\0';
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
    tmp[i] = '/';
  }
  if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
  return 0;
}

bool hml_fuzz_port_free(uint16_t port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(0x7f000001u);
  sa.sin_port = htons(port);
  bool ok = bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  (void)close(fd);
  return ok;
}

uint16_t hml_fuzz_port_pick(uint16_t base, uint16_t lo, uint16_t hi, unsigned tries) {
  if (hi < lo) return 0;
  uint32_t span = (uint32_t)(hi - lo) + 1u;
  for (unsigned i = 0; i < tries; i++) {
    uint16_t p = (uint16_t)(((uint32_t)base + i) % span) + lo;
    if (hml_fuzz_port_free(p)) return p;
  }
  return 0;
}

/* ---- sealing ------------------------------------------------------------ */

static void body_from(const uint8_t *data, size_t n, size_t from, uint8_t *out, size_t len) {
  size_t avail = n > from ? n - from : 0u;
  for (size_t i = 0; i < len; i++)
    out[i] = avail > 0u ? data[from + (i % avail)] : (uint8_t)i;
}

static uint16_t be16_at(const uint8_t *d, size_t n, size_t at) {
  if (n < at + 2u) return 0u;
  return (uint16_t)(((uint16_t)d[at] << 8) | d[at + 1]);
}

static uint32_t be32_at(const uint8_t *d, size_t n, size_t at) {
  if (n < at + 4u) return 0u;
  return hml_get_be32(d + at);
}

size_t hml_fuzz_seal_frame(const uint8_t *data, size_t n, uint8_t *out, size_t cap) {
  hml_frame_hdr h;
  memset(&h, 0, sizeof(h));
  uint32_t plen = be16_at(data, n, 0u);
  if (plen > (uint32_t)HML_MAX_FRAG_PAYLOAD) plen = (uint32_t)HML_MAX_FRAG_PAYLOAD;

  h.stream_id = be16_at(data, n, 6u);
  h.seq = be32_at(data, n, 8u);
  h.frag_idx = be16_at(data, n, 12u);
  h.frag_cnt = be16_at(data, n, 14u);
  h.ts_us = (uint64_t)be32_at(data, n, 16u) | ((uint64_t)be32_at(data, n, 20u) << 32);
  h.flags = (n > 5u) ? data[5] : (uint8_t)(HML_FLAG_START | HML_FLAG_END);
  h.payload_len = (uint16_t)plen;

  size_t total = (size_t)HML_FRAME_HDR_SIZE + plen + HML_TRAIL_CRC_SIZE;
  if (total > cap) return 0;

  hml_proto_frame_fill(&h, out);
  body_from(data, n, 28u, out + HML_OFF_PAYLOAD, plen);
  {
    uint16_t pc = hml_crc16(out + HML_OFF_PAYLOAD, plen);
    out[HML_OFF_PAYLOAD + plen] = (uint8_t)(pc >> 8);
    out[HML_OFF_PAYLOAD + plen + 1u] = (uint8_t)pc;
  }
  return total;
}

size_t hml_fuzz_seal_ctrl(const uint8_t *data, size_t n, uint8_t *out, size_t cap) {
  uint8_t body[HML_CTRL_MAX_PAYLOAD];
  uint32_t plen = be16_at(data, n, 0u);
  if (plen > (uint32_t)HML_CTRL_MAX_PAYLOAD) plen = (uint32_t)HML_CTRL_MAX_PAYLOAD;
  body_from(data, n, 5u, body, plen);

  hml_ctrl_msg m;
  memset(&m, 0, sizeof(m));
  m.op = (n > 2u) ? data[2] : (uint8_t)HML_OP_NOP;
  m.flags = (n > 4u) ? be16_at(data, n, 3u) : (uint16_t)HML_CTRL_FLAG_REQ;
  m.payload = plen > 0u ? body : NULL;
  m.payload_len = (uint16_t)plen;

  uint16_t out_n = 0;
  if (cap > (size_t)UINT16_MAX) cap = (size_t)UINT16_MAX;
  hml_ctrl_fill(&m, out, (uint16_t)cap, &out_n);
  return out_n;
}

size_t hml_fuzz_seal_cfg(const uint8_t *data, size_t n, uint8_t *out, size_t cap) {
  if (cap < (size_t)HML_CFG_SIZE) return 0;
  hml_dest_cfg c;
  memset(&c, 0, sizeof(c));
  c.kind = (n > 0u) ? (hml_xport_kind)(data[0] % 2u) : HML_XPORT_UDP;
  c.udp_port = be16_at(data, n, 2u);
  c.ipv4 = be32_at(data, n, 4u);
  c.qp = be32_at(data, n, 8u);
  c.psn = be32_at(data, n, 12u);
  c.rkey = be32_at(data, n, 16u);
  c.remote_addr = (uint64_t)be32_at(data, n, 20u) | ((uint64_t)be32_at(data, n, 24u) << 32);
  c.frame_size = be32_at(data, n, 28u);
  c.window_len = be16_at(data, n, 32u);
  c.hop_limit = (n > 34u) ? data[34] : 0u;
  hml_proto_cfg_fill(&c, out);
  return (size_t)HML_CFG_SIZE;
}

size_t hml_fuzz_seal_beacon(const uint8_t *data, size_t n, uint8_t *out, size_t cap) {
  if (cap < (size_t)HML_DISC_SIZE) return 0;
  hml_beacon b;
  memset(&b, 0, sizeof(b));
  b.version = (n > 0u) ? data[0] : (uint8_t)HML_DISC_VERSION;
  b.flags = (n > 1u) ? data[1] : 0u;
  b.ttl = (n > 2u) ? data[2] : 1u;
  b.uuid = be32_at(data, n, 3u);
  b.caps = be32_at(data, n, 7u);
  b.fw_ver = be32_at(data, n, 11u);
  /* ts_us is 48 bits on the wire (HML_DISC_SIZE is 28), so the value has to be
   * masked here or fill(parse(x)) would not round trip. */
  b.ts_us = ((uint64_t)be32_at(data, n, 15u) | ((uint64_t)be32_at(data, n, 19u) << 32) |
             ((uint64_t)(n > 23u ? data[23] : 0u) << 40)) &
            0x0000ffffffffffffull;
  hml_beacon_fill(&b, out);
  return (size_t)HML_DISC_SIZE;
}

/* ---- driver ------------------------------------------------------------- */

static void usage(const char *harness) {
  printf("usage: %s <input-file>\n"
         "       %s --selfmutate [--iters N] [--seed S] [--seconds S]\n"
         "       %s --help\n",
         harness, harness, harness);
}

static double now_s(void) {
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int run_selfmutate(const char *harness, hml_fuzz_input_fn run, long iters,
                          uint32_t seed, double seconds) {
  static uint8_t seedbuf[512];
  static uint8_t work[HML_FUZZ_MAX_INPUT];

  /* Seeds for the mutation loop: the harness name plus a fixed constant stream,
   * so the loop is reproducible with no corpus on disk. */
  size_t m = strlen(harness);
  if (m > sizeof(seedbuf)) m = sizeof(seedbuf);
  memcpy(seedbuf, harness, m);
  for (size_t i = m; i < sizeof(seedbuf); i++) seedbuf[i] = (uint8_t)(i * 31u + 7u);
  m = sizeof(seedbuf);

  uint32_t rng = seed != 0u ? seed : 0x1234567u;
  long done = 0;
  double t0 = now_s();
  while (done < iters) {
    /* Checked every 64 inputs: the socket-backed harnesses spend milliseconds
     * per input, so a 1024-input batch would overshoot --seconds badly. */
    if ((done & 0x3f) == 0 && seconds > 0.0 && now_s() - t0 >= seconds) break;
    size_t n = hml_fuzz_mutate(seedbuf, m, &rng, work, sizeof(work));
    /* Bias a quarter of the iterations towards an extremal 32-bit value written
     * at a random offset: that is the corruption class that reaches an
     * allocator or an array index in these parsers. */
    if ((hml_fuzz_rng(&rng) % 4u) == 0u && n >= 4u) {
      static const uint32_t kHuge[4] = {0xffffffffu, 0x7fffffffu, 0x80000000u, 0x00ffffffu};
      size_t at = (size_t)(hml_fuzz_rng(&rng) % (unsigned)(n - 3u));
      hml_put_be32(work + at, kHuge[hml_fuzz_rng(&rng) % 4u]);
    }

    g_live_data = work;
    g_live_size = n;
    g_input_path = "<selfmutate>";
    (void)alarm(alarm_seconds());
    run(work, n);
    (void)alarm(0);
    done++;
  }
  printf("harness %s: selfmutate iters=%ld seed=0x%08x elapsed=%.2fs findings=0\n", harness,
         done, seed != 0u ? seed : 0x1234567u, now_s() - t0);
  return HML_FUZZ_EXIT_OK;
}

int hml_fuzz_main(int argc, char **argv, const char *harness, hml_fuzz_input_fn run,
                  hml_fuzz_once_fn once) {
  g_harness = harness;

  if (argc < 2) {
    usage(harness);
    return HML_FUZZ_EXIT_USAGE;
  }
  if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
    usage(harness);
    return HML_FUZZ_EXIT_OK;
  }

  long iters = 20000;
  uint32_t seed = 0u;
  double seconds = 0.0;
  bool self = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--selfmutate") == 0) self = true;
    else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) iters = strtol(argv[++i], NULL, 0);
    else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
      seed = (uint32_t)strtoul(argv[++i], NULL, 0);
    else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
      seconds = (double)strtod(argv[++i], NULL);
  }
  if (iters < 1) iters = 1;

  install_guards();
  if (once != NULL) once();

  if (self) return run_selfmutate(harness, run, iters, seed, seconds);

  static uint8_t data[HML_FUZZ_MAX_INPUT];
  size_t n = 0;
  g_input_path = argv[1];
  if (!hml_fuzz_read_file(argv[1], data, sizeof(data), &n)) {
    fprintf(stderr, "%s: cannot read %s: %s\n", harness, argv[1], strerror(errno));
    return HML_FUZZ_EXIT_ERROR;
  }
  g_live_data = data;
  g_live_size = n;
  (void)alarm(alarm_seconds());
  run(data, n);
  (void)alarm(0);
  return HML_FUZZ_EXIT_OK;
}
