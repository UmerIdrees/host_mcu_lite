/*
 * M2 / test_lifecycle - full plan M2 sequence in one process.
 *
 * The device control server runs in a pthread (loopback high ports) and the
 * host client drives it from the main thread:
 *   discover -> set_dest -> get_dest readback -> start,start -> status
 *   -> stop -> status, then 20x (set_dest,start,stop) for determinism.
 * Then fault injection: garbage datagrams must be dropped without corrupting
 * state, an unknown opcode must answer ERROR/ERR instead of hanging.
 *
 * argv[1] overrides the port base (default 45502):
 *   control = base, beacon = base-2, client = base+1, raw probe = base+2.
 */

#include "hml_client.h"
#include "hml_control.h"

#include "hml/hml_sock.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failures = 0;

#define CHECK(cond, ...)                          \
  do {                                            \
    if (!(cond)) {                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
      g_failures++;                               \
    }                                             \
  } while (0)

#define CHECK_OK(expr) CHECK((expr) == HML_OK, "%s -> %d", #expr, (int)(expr))

/* ---- device side ------------------------------------------------------- */

#define DEV_REGS 16u

typedef struct {
  pthread_mutex_t lock;
  uint32_t starts;
  uint32_t stops;
  uint32_t set_dests;
  uint32_t pokes;
  uint32_t reads;
  hml_dest_cfg dest;
  uint32_t regs[DEV_REGS];
} dev_ctx_t;

static hml_control_t *g_ctl = NULL;
static volatile int g_dev_run = 0;

static void dev_on_set_dest(void *ctx, const hml_dest_cfg *cfg) {
  dev_ctx_t *d = (dev_ctx_t *)ctx;
  pthread_mutex_lock(&d->lock);
  d->dest = *cfg;
  d->set_dests++;
  pthread_mutex_unlock(&d->lock);
}

static void dev_on_start(void *ctx) {
  dev_ctx_t *d = (dev_ctx_t *)ctx;
  pthread_mutex_lock(&d->lock);
  d->starts++;
  pthread_mutex_unlock(&d->lock);
}

static void dev_on_stop(void *ctx) {
  dev_ctx_t *d = (dev_ctx_t *)ctx;
  pthread_mutex_lock(&d->lock);
  d->stops++;
  pthread_mutex_unlock(&d->lock);
}

static uint32_t dev_on_poke(void *ctx, uint32_t addr, uint32_t val, bool *ok) {
  dev_ctx_t *d = (dev_ctx_t *)ctx;
  pthread_mutex_lock(&d->lock);
  if (addr >= DEV_REGS) {
    *ok = false;
    pthread_mutex_unlock(&d->lock);
    return 0;
  }
  d->regs[addr] = val;
  d->pokes++;
  *ok = true;
  uint32_t v = d->regs[addr];
  pthread_mutex_unlock(&d->lock);
  return v;
}

static uint32_t dev_on_read(void *ctx, uint32_t addr, bool *ok) {
  dev_ctx_t *d = (dev_ctx_t *)ctx;
  pthread_mutex_lock(&d->lock);
  if (addr >= DEV_REGS) {
    *ok = false;
    pthread_mutex_unlock(&d->lock);
    return 0;
  }
  d->reads++;
  *ok = true;
  uint32_t v = d->regs[addr];
  pthread_mutex_unlock(&d->lock);
  return v;
}

static void *dev_thread(void *arg) {
  (void)arg;
  while (g_dev_run) {
    hml_status st = hml_control_poll(g_ctl, 10);
    if (st != HML_OK && st != HML_ERR_AGAIN && st != HML_ERR_TIMEOUT)
      printf("FAIL dev poll status %d\n", (int)st);
  }
  return NULL;
}

static uint32_t dev_starts(dev_ctx_t *d) {
  pthread_mutex_lock(&d->lock);
  uint32_t v = d->starts;
  pthread_mutex_unlock(&d->lock);
  return v;
}

static uint32_t dev_stops(dev_ctx_t *d) {
  pthread_mutex_lock(&d->lock);
  uint32_t v = d->stops;
  pthread_mutex_unlock(&d->lock);
  return v;
}

/* ---- helpers ----------------------------------------------------------- */

static hml_dest_cfg make_cfg(uint32_t ipv4_host, uint16_t port, uint32_t frame_size) {
  hml_dest_cfg c;
  memset(&c, 0, sizeof(c));
  c.kind = HML_XPORT_UDP;
  c.ipv4 = ipv4_host; /* hml_dest_cfg.ipv4 is network byte order */
  c.udp_port = port;
  c.qp = 0x1234u;
  c.psn = 0xdeadbeefu;
  c.rkey = 0x0abcde01u;
  c.remote_addr = 0x00007f0000001000ull;
  c.frame_size = frame_size;
  c.window_len = 4u;
  c.hop_limit = 64u;
  return c;
}

/* ---- tests ------------------------------------------------------------- */

static void test_lifecycle(hml_client_t *cli, dev_ctx_t *dev, uint16_t ctrl_port,
                           uint16_t disc_port) {
  /* 1. discover: beacon + advertised identity */
  uint32_t uuid = 0, caps = 0, fw = 0;
  uint64_t ts = 0;
  hml_status st = hml_client_discover(cli, 3000, &uuid, &caps, &fw, &ts);
  CHECK(st == HML_OK, "discover: status %d", (int)st);
  CHECK(uuid == 0x1234u, "discover uuid 0x%08x", uuid);
  CHECK(caps == (HML_CAP_UDP | HML_CAP_MCU), "discover caps 0x%08x", caps);
  CHECK(fw == 0x00010000u, "discover fw 0x%08x", fw);
  CHECK(ts != 0, "discover ts_us is zero");

  uint32_t uuid2 = 0, caps2 = 0, fw2 = 0;
  uint64_t ts2 = 0;
  st = hml_client_discover(cli, 3000, &uuid2, &caps2, &fw2, &ts2);
  CHECK(st == HML_OK, "second discover: status %d", (int)st);
  CHECK(uuid2 == uuid && caps2 == caps && fw2 == fw, "beacon identity must be stable");
  CHECK(ts2 > ts, "beacon ts must advance: %llu then %llu", (unsigned long long)ts,
        (unsigned long long)ts2);

  /* GET_CAPS must agree with the beacon */
  uint32_t cuuid = 0, ccaps = 0, cfw = 0;
  CHECK_OK(hml_client_get_caps(cli, &cuuid, &ccaps, &cfw));
  CHECK(cuuid == uuid && ccaps == caps && cfw == fw, "get_caps 0x%08x/0x%08x/0x%08x", cuuid, ccaps,
        cfw);

  /* ping echo */
  uint32_t tok = 0;
  CHECK_OK(hml_client_ping(cli, 0x5a5a1234u, &tok));
  CHECK(tok == 0x5a5a1234u, "ping token 0x%08x", tok);

  /* 2. set_dest + readback */
  hml_dest_cfg want = make_cfg(HML_IP_LOOPBACK, 45511u, 65536u);
  CHECK_OK(hml_client_set_dest(cli, &want));
  hml_dest_cfg got;
  memset(&got, 0xAB, sizeof(got));
  CHECK_OK(hml_client_get_dest(cli, &got));
  CHECK(memcmp(&want, &got, sizeof(want)) == 0, "dest readback mismatch (kind=%u ipv4=%08x port=%u "
                                               "qp=%u psn=%08x rkey=%08x raddr=%llx fsize=%u "
                                               "wlen=%u hop=%u)",
         (unsigned)got.kind, got.ipv4, (unsigned)got.udp_port, got.qp, got.psn, got.rkey,
         (unsigned long long)got.remote_addr, got.frame_size, got.window_len, got.hop_limit);
  hml_dest_cfg dev_dest;
  CHECK(hml_control_get_dest(g_ctl, &dev_dest), "device must report a stored dest");
  CHECK(memcmp(&dev_dest, &want, sizeof(want)) == 0, "device dest mismatch");

  /* 3. start twice (idempotent), status, stop */
  CHECK_OK(hml_client_start(cli));
  CHECK_OK(hml_client_start(cli));
  CHECK(dev_starts(dev) == 2u, "two START commands -> %u starts", dev_starts(dev));
  bool streaming = false;
  CHECK_OK(hml_client_status(cli, &streaming));
  CHECK(streaming, "status must report streaming after start");
  CHECK(hml_control_streaming(g_ctl), "device must be streaming");
  CHECK_OK(hml_client_stop(cli));
  CHECK_OK(hml_client_status(cli, &streaming));
  CHECK(!streaming, "status must report stopped after stop");
  CHECK(!hml_control_streaming(g_ctl), "device must be stopped");
  CHECK(dev_stops(dev) == 1u, "one stop recorded, got %u", dev_stops(dev));

  /* 4. 20x set_dest/start/stop determinism */
  uint32_t starts_before = dev_starts(dev);
  uint32_t stops_before = dev_stops(dev);
  for (int i = 0; i < 20; i++) {
    hml_dest_cfg c = make_cfg(HML_IP_LOOPBACK, (uint16_t)(45520u + (uint32_t)i), (uint32_t)(i * 1024u));
    hml_status s = hml_client_set_dest(cli, &c);
    CHECK(s == HML_OK, "loop %d set_dest -> %d", i, (int)s);
    s = hml_client_start(cli);
    CHECK(s == HML_OK, "loop %d start -> %d", i, (int)s);
    s = hml_client_status(cli, &streaming);
    CHECK(s == HML_OK && streaming, "loop %d status -> %d streaming=%d", i, (int)s, streaming ? 1 : 0);
    s = hml_client_stop(cli);
    CHECK(s == HML_OK, "loop %d stop -> %d", i, (int)s);
    hml_dest_cfg back;
    s = hml_client_get_dest(cli, &back);
    CHECK(s == HML_OK && memcmp(&c, &back, sizeof(c)) == 0, "loop %d dest readback", i);
  }
  CHECK(dev_starts(dev) == starts_before + 20u, "20 STARTs -> starts %u (was %u)",
        dev_starts(dev) - starts_before, starts_before);
  CHECK(dev_stops(dev) == stops_before + 20u, "20 STOPs -> stops %u (was %u)",
        dev_stops(dev) - stops_before, stops_before);

  /* poke / readback through the device register file */
  uint32_t rval = 0;
  CHECK_OK(hml_client_poke(cli, 5u, 0x600du, &rval));
  CHECK(rval == 0x600du, "poke readback 0x%08x", rval);
  uint32_t rb = 0;
  CHECK_OK(hml_client_readback(cli, 5u, &rb));
  CHECK(rb == 0x600du, "readback 0x%08x", rb);
  st = hml_client_poke(cli, 999u, 1u, &rval);
  CHECK(st == HML_ERR_NOTSUP, "out of range poke must fail closed, got %d", (int)st);

  /* reset clears the dest and stops */
  CHECK_OK(hml_client_reset(cli));
  CHECK(!hml_control_get_dest(g_ctl, &dev_dest), "reset must clear the stored dest");
  CHECK(!hml_control_streaming(g_ctl), "reset must leave the device stopped");
  (void)disc_port;
  (void)ctrl_port;
}

/* Sends raw bytes to the control port from a dedicated probe socket. */
static hml_status probe_send(hml_sock_t *s, const uint8_t *b, size_t n, uint16_t ctrl_port) {
  (void)hml_sock_set_dest(s, HML_IP_LOOPBACK, ctrl_port);
  return hml_sock_send(s, b, n);
}

static void sleep_ms(unsigned ms) {
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000u);
  ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
  (void)nanosleep(&ts, NULL);
}

/* Garbage / short / bad-CRC datagrams must be dropped without a response and
 * without any state change. Valid traffic comes from the real client. */
static void test_faults(hml_client_t *cli, dev_ctx_t *dev, uint16_t ctrl_port,
                        uint16_t probe_port) {
  hml_sock_t *probe = NULL;
  CHECK_OK(hml_sock_udp_create(&probe, HML_IP_ANY, probe_port, true));
  if (!probe) return;

  hml_dest_cfg want = make_cfg(HML_IP_LOOPBACK, 45577u, 4096u);
  CHECK_OK(hml_client_set_dest(cli, &want));
  hml_dest_cfg pre;
  CHECK_OK(hml_client_get_dest(cli, &pre));
  CHECK(memcmp(&pre, &want, sizeof(want)) == 0, "dest before fault injection");

  uint32_t bad_before = hml_control_bad_count(g_ctl);
  uint32_t rx_before = hml_control_rx_count(g_ctl);

  /* 1. junk shorter than a header */
  const uint8_t junk[9] = {'g', 'a', 'r', 'b', 'a', 'g', 'e', '!', '!'};
  CHECK_OK(probe_send(probe, junk, sizeof(junk), ctrl_port));

  /* 2. header-shaped datagram with a corrupted CRC */
  uint8_t badcrc[HML_CTRL_HDR_SIZE + 2u];
  hml_ctrl_msg m;
  memset(&m, 0, sizeof(m));
  m.op = HML_OP_START;
  m.flags = HML_CTRL_FLAG_REQ;
  uint16_t n = 0;
  hml_ctrl_fill(&m, badcrc, (uint16_t)sizeof(badcrc), &n);
  CHECK(n == sizeof(badcrc), "probe fill size %u", (unsigned)n);
  badcrc[n - 1] ^= 0xffu;
  CHECK_OK(probe_send(probe, badcrc, n, ctrl_port));

  /* 3. inconsistent length field (claims 40 bytes, carries none) */
  uint8_t badlen[HML_CTRL_HDR_SIZE + 2u];
  memset(&m, 0, sizeof(m));
  m.op = HML_OP_SET_DEST;
  m.flags = HML_CTRL_FLAG_REQ;
  hml_ctrl_fill(&m, badlen, (uint16_t)sizeof(badlen), &n);
  badlen[8] = 0x00;
  badlen[9] = 40; /* len = 40 but the datagram is only 14 bytes */
  CHECK_OK(probe_send(probe, badlen, n, ctrl_port));

  /* 4. zero-length datagram */
  CHECK_OK(probe_send(probe, junk, 0, ctrl_port));

  /* wait for the server to account for all four drops */
  uint32_t settle = hml_control_bad_count(g_ctl);
  for (int i = 0; i < 200 && settle < bad_before + 4u; i++) {
    sleep_ms(5);
    settle = hml_control_bad_count(g_ctl);
  }
  CHECK(settle >= bad_before + 4u, "malformed datagrams must be counted (%u -> %u)", bad_before,
        settle);
  CHECK(settle == bad_before + 4u, "expected 4 bad datagrams, counted %u", settle - bad_before);
  CHECK(hml_control_rx_count(g_ctl) == rx_before + 4u, "expected 4 received datagrams, got %u",
        hml_control_rx_count(g_ctl) - rx_before);

  /* the server must still answer valid requests and keep its state */
  bool streaming = true;
  CHECK_OK(hml_client_status(cli, &streaming));
  CHECK(!streaming, "malformed datagrams must not start the stream");
  hml_dest_cfg back;
  CHECK_OK(hml_client_get_dest(cli, &back));
  CHECK(memcmp(&back, &want, sizeof(want)) == 0, "malformed datagrams must not corrupt dest");
  CHECK(dev_starts(dev) == 22u, "no start callback for malformed input, got %u", dev_starts(dev));
  CHECK(dev_stops(dev) == 22u, "no stop callback for malformed input, got %u", dev_stops(dev));

  hml_sock_close(probe);
}

/* An unknown opcode must answer HML_OP_ERROR with FLAG_ERR, not hang. */
static void test_unknown_op(hml_client_t *cli, dev_ctx_t *dev, uint16_t ctrl_port,
                            uint16_t probe_port, uint16_t client_port) {
  hml_sock_t *probe = NULL;
  CHECK_OK(hml_sock_udp_create(&probe, HML_IP_ANY, probe_port, true));
  if (!probe) return;

  /* point the device replies at the probe socket for the duration of the test */
  CHECK_OK(hml_control_set_reply(g_ctl, HML_IP_LOOPBACK, probe_port));

  uint8_t pay[4] = {0xde, 0xad, 0xbe, 0xef};
  uint8_t req[HML_CTRL_HDR_SIZE + 4u + 2u];
  hml_ctrl_msg m;
  memset(&m, 0, sizeof(m));
  m.op = 0x7fu; /* not a defined HML_OP_* */
  m.flags = HML_CTRL_FLAG_REQ;
  m.payload = pay;
  m.payload_len = 4;
  uint16_t n = 0;
  hml_ctrl_fill(&m, req, (uint16_t)sizeof(req), &n);
  CHECK(n == 18u, "unknown-op fill %u", (unsigned)n);
  CHECK_OK(probe_send(probe, req, n, ctrl_port));

  uint8_t resp[64];
  size_t got = 0;
  hml_status st = hml_sock_recv(probe, resp, sizeof(resp), &got, 1000);
  CHECK(st == HML_OK, "unknown opcode must be answered, recv status %d", (int)st);
  if (st == HML_OK) {
    hml_ctrl_msg r;
    const uint8_t *pl = NULL;
    CHECK(hml_ctrl_parse(resp, got, &r, &pl) == HML_OK, "error response must be a valid message");
    CHECK(r.op == HML_OP_ERROR, "error response op 0x%02x want 0xff", (unsigned)r.op);
    CHECK((r.flags & HML_CTRL_FLAG_RESP) != 0, "error response must carry FLAG_RESP");
    CHECK((r.flags & HML_CTRL_FLAG_ERR) != 0, "error response must carry FLAG_ERR");
    CHECK((r.flags & HML_CTRL_FLAG_ACK) == 0, "error response must not carry FLAG_ACK");
    CHECK(r.payload_len == 1 && pl && pl[0] == (uint8_t)HML_ERR_NOTSUP,
          "error payload len=%u value=%u want 1/%u", (unsigned)r.payload_len, pl ? pl[0] : 0,
          (unsigned)HML_ERR_NOTSUP);
  }

  /* restore replies to the client and prove the client still works */
  CHECK_OK(hml_control_set_reply(g_ctl, HML_IP_LOOPBACK, client_port));
  bool streaming = false;
  CHECK_OK(hml_client_status(cli, &streaming));
  CHECK(!streaming, "unknown opcode must not change state");
  uint32_t tok = 0;
  CHECK_OK(hml_client_ping(cli, 0x99u, &tok));
  CHECK(dev_starts(dev) == 22u, "unknown opcode must not start anything (%u)", dev_starts(dev));

  hml_sock_close(probe);
}

int main(int argc, char **argv) {
  uint16_t base = 45502;
  if (argc > 1) {
    unsigned long v = strtoul(argv[1], NULL, 0);
    if (v > 65533u) {
      printf("FAIL port base out of range\n");
      return 1;
    }
    base = (uint16_t)v;
  }
  const uint16_t ctrl_port = base;
  const uint16_t disc_port = (uint16_t)(base - 2u);
  const uint16_t client_port = (uint16_t)(base + 1u);
  const uint16_t probe_port = (uint16_t)(base + 2u);

  dev_ctx_t dev;
  memset(&dev, 0, sizeof(dev));
  CHECK(pthread_mutex_init(&dev.lock, NULL) == 0, "mutex init");

  hml_control_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.ctrl_port = ctrl_port;
  cfg.disc_port = disc_port;
  cfg.beacon_enable = true;
  cfg.beacon_period_ms = 100; /* fast cadence keeps the test under a second */
  cfg.uuid = 0x1234u;
  cfg.caps = HML_CAP_UDP | HML_CAP_MCU;
  cfg.fw_ver = 0x00010000u;
  cfg.reply_ip = HML_IP_LOOPBACK;
  cfg.reply_port = client_port;

  hml_status st = hml_control_open(&g_ctl, &cfg);
  if (st != HML_OK) {
    printf("FAIL hml_control_open: %d (port %u busy?)\n", (int)st, (unsigned)ctrl_port);
    return 1;
  }
  CHECK_OK(hml_control_set_cbs(g_ctl, dev_on_set_dest, dev_on_start, dev_on_stop, dev_on_poke, &dev));
  CHECK_OK(hml_control_set_read_cb(g_ctl, dev_on_read, &dev));

  g_dev_run = 1;
  pthread_t th;
  if (pthread_create(&th, NULL, dev_thread, NULL) != 0) {
    printf("FAIL pthread_create\n");
    return 1;
  }

  hml_client_t *cli = NULL;
  st = hml_client_open(&cli, client_port);
  if (st != HML_OK) {
    printf("FAIL hml_client_open: %d (port %u busy?)\n", (int)st, (unsigned)client_port);
    g_dev_run = 0;
    pthread_join(th, NULL);
    hml_control_close(g_ctl);
    return 1;
  }
  CHECK_OK(hml_client_set_timeout(cli, 1000u));
  CHECK_OK(hml_client_set_peer(cli, HML_IP_LOOPBACK, ctrl_port, disc_port));

  printf("lifecycle on control=%u beacon=%u client=%u probe=%u\n", (unsigned)ctrl_port,
         (unsigned)disc_port, (unsigned)client_port, (unsigned)probe_port);
  test_lifecycle(cli, &dev, ctrl_port, disc_port);
  test_faults(cli, &dev, ctrl_port, probe_port + 100u);
  test_unknown_op(cli, &dev, ctrl_port, probe_port + 200u, client_port);

  g_dev_run = 0;
  pthread_join(th, NULL);

  pthread_mutex_lock(&dev.lock);
  uint32_t starts = dev.starts, stops = dev.stops, set_dests = dev.set_dests;
  uint32_t pokes = dev.pokes, reads = dev.reads;
  pthread_mutex_unlock(&dev.lock);

  hml_client_close(cli);
  hml_control_close(g_ctl);
  g_ctl = NULL;
  pthread_mutex_destroy(&dev.lock);

  printf("device stats: starts=%u stops=%u set_dests=%u pokes=%u reads=%u\n", starts, stops,
         set_dests, pokes, reads);
  CHECK(starts == 22u, "device recorded %u starts, want 22", starts);
  CHECK(stops == 22u, "device recorded %u stops, want 22", stops);
  CHECK(set_dests == 22u, "device recorded %u set_dest, want 22", set_dests);


  if (g_failures) {
    printf("test_lifecycle: %d check(s) FAILED\n", g_failures);
    return 1;
  }
  printf("test_lifecycle: all checks passed\n");
  return 0;
}
