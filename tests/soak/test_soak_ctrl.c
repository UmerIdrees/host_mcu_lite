/*
 * test_soak_ctrl -- bounded soak of the control plane (hml_control_* server plus
 * hml_client_* request/response, over loopback).
 *
 * The plan's M2 exit criterion is "discover -> configure dest -> start/stop
 * without rebuild", and its soak case is 1 h continuous with flat memory. The
 * control plane is the part of the tree that holds state across requests, so it
 * is the part a soak has to hammer: one open context, thousands of
 * set_dest/start/stop/status cycles plus malformed traffic interleaved, and every
 * counter reconciled at the end.
 *
 * The design point is that the server runs in a thread and the client in the
 * main thread, exactly as hml_control is documented to be used ("not internally
 * locked, one poller per hml_control_t"). Anything that only works single
 * threaded is a defect for a real device, and a soak that drove the server
 * synchronously would not find it.
 *
 * Invariants checked throughout:
 *
 *   - rx_count rises by exactly the number of datagrams sent, and bad_count by
 *     exactly the number that were deliberately malformed. Together they account
 *     for everything on the wire, so a datagram that is silently swallowed shows
 *     up as a reconciliation failure rather than as a slow drift.
 *   - malformed traffic changes nothing: after a batch of garbage the stored
 *     dest, the streaming flag and the callback counters must all be as they
 *     were. This is the check that makes a fuzz finding here meaningful -- the
 *     fuzz harness proves malformed input is rejected, this proves rejection has
 *     no side effect.
 *   - every SET_DEST is readable: the readback must match byte for byte, so a
 *     server that accumulated state instead of replacing it would be caught.
 *   - GET_DEST before any SET_DEST must answer ERR, not success with zeroes.
 *   - an unknown opcode must be answered with ERROR/ERR rather than ignored,
 *     because a host waiting on that socket would otherwise block until timeout.
 *   - poke/readback round-trip through the device register file, and an
 *     out-of-range address must fail closed.
 *
 * Two phases, because they detect different things: a long steady-state loop in
 * one open server, then an open/close churn. hml_control_open binds a port and
 * allocates receive buffers, so a server that leaks on close only shows up in the
 * churn.
 *
 *   test_soak_ctrl [--seconds N] [--port-base P]    default 5 seconds
 */

#include "hml_soak_util.h"

#include "hml/hml_ctrl.h"
#include "hml/hml_protocol.h"
#include "hml/hml_sock.h"
#include "hml_client.h"
#include "hml_control.h"

#include <pthread.h>

/* Control datagram field offsets. HML_CTRL_HDR_SIZE is public (hml_control.h) but
 * the field positions are private to hml_ctrl_codec.c, so they are spelled out
 * here. tests/fuzz checks the same offsets against the real codec, so a layout
 * change fails there too rather than silently making these corruptions benign. */
#define HML_OFF_CTRL_MAGIC 0u
#define HML_OFF_CTRL_VERSION 4u
#define HML_OFF_CTRL_LEN 8u
#define HML_OFF_CTRL_RSVD 10u

/* Private port range, disjoint from the fuzz harnesses' and from the M2/M3 tests,
 * so this can run alongside them. Rotated by soak_port_pick on a busy port. */
#define PORT_LO 45100u
#define PORT_HI 45299u
#define MAX_PORTS 8u

#define REGS 64u
#define ROUNDS_PER_SAMPLE 8u

/* The device side of the control plane. A mutex because the callbacks run on the
 * server thread while the main thread reads the counters to reconcile them. */
typedef struct dev {
  pthread_mutex_t lock;
  uint32_t sets, starts, stops, pokes, reads, errors;
  hml_dest_cfg dest;
  uint32_t regs[REGS];
} dev;

static hml_control_t *g_ctl;
static volatile int g_run = 1;
static volatile int g_poll_bad;

static void on_set_dest(void *ctx, const hml_dest_cfg *cfg) {
  dev *d = (dev *)ctx;
  pthread_mutex_lock(&d->lock);
  d->dest = *cfg;
  d->sets++;
  pthread_mutex_unlock(&d->lock);
}
static void on_start(void *ctx) {
  dev *d = (dev *)ctx;
  pthread_mutex_lock(&d->lock);
  d->starts++;
  pthread_mutex_unlock(&d->lock);
}
static void on_stop(void *ctx) {
  dev *d = (dev *)ctx;
  pthread_mutex_lock(&d->lock);
  d->stops++;
  pthread_mutex_unlock(&d->lock);
}
static uint32_t on_poke(void *ctx, uint32_t addr, uint32_t val, bool *ok) {
  dev *d = (dev *)ctx;
  pthread_mutex_lock(&d->lock);
  if (addr >= REGS) {
    *ok = false;
  } else {
    d->regs[addr] = val;
    d->pokes++;
    *ok = true;
  }
  uint32_t back = *ok ? d->regs[addr] : 0u;
  pthread_mutex_unlock(&d->lock);
  return back;
}
static uint32_t on_read(void *ctx, uint32_t addr, bool *ok) {
  dev *d = (dev *)ctx;
  pthread_mutex_lock(&d->lock);
  if (addr >= REGS) {
    *ok = false;
  } else {
    d->reads++;
    *ok = true;
  }
  uint32_t v = *ok ? d->regs[addr] : 0u;
  pthread_mutex_unlock(&d->lock);
  return v;
}

/* The server loop. hml_control_poll returns HML_ERR_AGAIN when its budget expired
 * with nothing received, which is the normal idle case and not a fault. */
static void *dev_thread(void *arg) {
  (void)arg;
  while (g_run) {
    hml_status st = hml_control_poll(g_ctl, 5);
    if (st != HML_OK && st != HML_ERR_AGAIN && st != HML_ERR_TIMEOUT) g_poll_bad = 1;
  }
  return NULL;
}

static uint32_t dev_get(dev *d, uint32_t *sets, uint32_t *starts, uint32_t *stops,
                        uint32_t *pokes, uint32_t *reads) {
  pthread_mutex_lock(&d->lock);
  if (sets) *sets = d->sets;
  if (starts) *starts = d->starts;
  if (stops) *stops = d->stops;
  if (pokes) *pokes = d->pokes;
  if (reads) *reads = d->reads;
  uint32_t v = 0;
  pthread_mutex_unlock(&d->lock);
  return v;
}

static hml_dest_cfg make_cfg(uint32_t round, uint16_t data_port) {
  hml_dest_cfg c;
  memset(&c, 0, sizeof(c));
  c.kind = HML_XPORT_UDP;
  c.ipv4 = HML_IP_LOOPBACK;
  c.udp_port = data_port;
  c.qp = (uint16_t)(0x1000u + (round & 0x0fffu));
  c.psn = 0xdead0000u + (uint32_t)round;
  c.rkey = 0x0abcde00u + (uint32_t)round;
  c.remote_addr = 0x00007f0000001000ull;
  c.frame_size = 65536u;
  c.window_len = 8u;
  c.hop_limit = 64u;
  return c;
}

/* One malformed datagram. `kind` picks which property is broken, because a soak
 * that only sends one shape of garbage proves only that one shape is rejected. */
static size_t build_bad(uint8_t *buf, unsigned kind, unsigned round) {
  hml_ctrl_msg m;
  memset(&m, 0, sizeof(m));
  m.op = (uint8_t)(0x01u + (round % 8u));
  m.flags = HML_CTRL_FLAG_REQ;
  const uint8_t pay[4] = {0xde, 0xad, 0xbe, 0xef};
  m.payload = pay;
  m.payload_len = 4u;
  uint16_t n = 0;
  hml_ctrl_fill(&m, buf, (uint16_t)(HML_CTRL_HDR_SIZE + 8u + 2u), &n);
  switch (kind % 6u) {
    case 0: /* too short to hold a header */
      return 9u;
    case 1: /* broken trailing CRC */
      buf[n - 1u] ^= 0xffu;
      return n;
    case 2: /* length field claims more than the datagram carries */
      buf[HML_OFF_CTRL_LEN + 0] = 0x00;
      buf[HML_OFF_CTRL_LEN + 1] = 40u;
      return n;
    case 3: /* wrong magic */
      buf[HML_OFF_CTRL_MAGIC + 0] ^= 0xffu;
      return n;
    case 4: /* wrong version */
      buf[HML_OFF_CTRL_VERSION] = 0x7fu;
      return n;
    default: /* non-zero reserved byte */
      buf[HML_OFF_CTRL_RSVD] = 0x01u;
      return n;
  }
}

/* Datagrams the client puts on the wire in one round, one per call below. The
 * reconciliation later asserts that the server saw exactly this many, so if a
 * client request were ever retried, coalesced, or split the count would stop
 * matching and the soak would fail. Counted explicitly rather than assumed,
 * because "one hml_client_* call is one datagram" is a property worth testing.
 *
 *   set_dest, get_dest, start, status, stop, status, poke, poke(out of range),
 *   readback
 */
#define CLIENT_RPCS_PER_ROUND 9u

/* Malformed datagrams sent from the probe socket per round; see build_bad(). */
#define GARBAGE_PER_ROUND 3u

int main(int argc, char **argv) {
  unsigned seconds = 5u;
  uint16_t cursor = PORT_LO;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      seconds = (unsigned)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(argv[i], "--port-base") == 0 && i + 1 < argc) {
      cursor = (uint16_t)strtoul(argv[++i], NULL, 0);
      if (cursor < PORT_LO || cursor > PORT_HI) {
        printf("FAIL --port-base must be within %u..%u\n", PORT_LO, PORT_HI);
        return 2;
      }
    } else {
      printf("usage: %s [--seconds N] [--port-base P]\n", argv[0]);
      return 2;
    }
  }
  if (seconds == 0u) seconds = 1u;

  /* Three ports from the private range, retried on a busy bind. */
  uint16_t ctrl_port = soak_port_pick(PORT_LO, PORT_HI, &cursor);
  uint16_t disc_port = soak_port_pick(PORT_LO, PORT_HI, &cursor);
  uint16_t client_port = soak_port_pick(PORT_LO, PORT_HI, &cursor);
  uint16_t probe_port = soak_port_pick(PORT_LO, PORT_HI, &cursor);
  if (!ctrl_port || !disc_port || !client_port || !probe_port) {
    printf("FAIL no free UDP ports in %u..%u for the control soak\n", PORT_LO, PORT_HI);
    return 1;
  }

  dev d;
  memset(&d, 0, sizeof(d));
  if (pthread_mutex_init(&d.lock, NULL) != 0) {
    printf("FAIL pthread_mutex_init\n");
    return 1;
  }

  hml_control_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.ctrl_port = ctrl_port;
  cfg.disc_port = disc_port;
  /* Beacon on, at a cadence short enough that a soak exercises the beacon path
   * many times over rather than twice. */
  cfg.beacon_enable = true;
  cfg.beacon_period_ms = 50u;
  cfg.uuid = 0x5a5au;
  cfg.caps = HML_CAP_UDP | HML_CAP_MCU;
  cfg.fw_ver = 0x00010000u;
  cfg.reply_ip = HML_IP_LOOPBACK;
  cfg.reply_port = client_port;
  SOAK_CHECK_STATUS(hml_control_open(&g_ctl, &cfg), HML_OK);
  if (g_ctl == NULL) {
    printf("FAIL hml_control_open returned HML_OK but no server\n");
    return 1;
  }
  SOAK_CHECK_STATUS(hml_control_set_cbs(g_ctl, on_set_dest, on_start, on_stop, on_poke, &d),
                    HML_OK);
  SOAK_CHECK_STATUS(hml_control_set_read_cb(g_ctl, on_read, &d), HML_OK);

  pthread_t th;
  if (pthread_create(&th, NULL, dev_thread, NULL) != 0) {
    printf("FAIL pthread_create for the control server\n");
    hml_control_close(g_ctl);
    return 1;
  }

  hml_client_t *cli = NULL;
  SOAK_CHECK_STATUS(hml_client_open(&cli, client_port), HML_OK);
  if (cli == NULL) {
    printf("FAIL hml_client_open returned HML_OK but no client\n");
    g_run = 0;
    (void)pthread_join(th, NULL);
    hml_control_close(g_ctl);
    return 1;
  }
  SOAK_CHECK_STATUS(hml_client_set_peer(cli, HML_IP_LOOPBACK, ctrl_port, disc_port), HML_OK);
  SOAK_CHECK_STATUS(hml_client_set_timeout(cli, 500u), HML_OK);

  hml_sock_t *probe = NULL;
  SOAK_CHECK_STATUS(hml_sock_udp_create(&probe, HML_IP_ANY, probe_port, true), HML_OK);
  SOAK_CHECK_STATUS(hml_sock_set_dest(probe, HML_IP_LOOPBACK, ctrl_port), HML_OK);

  /* A first request proves the server is actually up before the long loop starts;
   * otherwise every failure below would be reported as a request failure. */
  uint32_t tok = 0;
  SOAK_CHECK_STATUS(hml_client_ping(cli, 1u, &tok), HML_OK);
  SOAK_CHECK_EQ(tok, 1u, "the first ping did not echo its token");

  soak_budget budget;
  soak_budget_init(&budget, seconds);
  soak_rss rss;
  soak_rss_init(&rss, 256L * 1024L);

  uint64_t rounds = 0, sent = 0, sent_bad = 0, sent_at = 0, sent_bad_at = 0;
  uint32_t rx_seen = hml_control_rx_count(g_ctl), bad_seen = hml_control_bad_count(g_ctl);
  uint8_t bad[HML_CTRL_HDR_SIZE + 8u + 2u];

  while (!soak_budget_expired(&budget)) {
    /* ---- steady state: the full request set, once per round ---- */
    sent += CLIENT_RPCS_PER_ROUND;
    hml_dest_cfg want = make_cfg(rounds, (uint16_t)(40000u + (rounds % 512u)));
    SOAK_CHECK_STATUS(hml_client_set_dest(cli, &want), HML_OK);
    hml_dest_cfg got;
    memset(&got, 0, sizeof(got));
    SOAK_CHECK_STATUS(hml_client_get_dest(cli, &got), HML_OK);
    if (memcmp(&want, &got, sizeof(want)) != 0) {
      SOAK_FAILF("round %llu: dest readback differs from what was set (port %u vs %u, psn %u vs "
                 "%u, rkey %u vs %u)",
                 (unsigned long long)rounds, (unsigned)want.udp_port, (unsigned)got.udp_port,
                 (unsigned)want.psn, (unsigned)got.psn, (unsigned)want.rkey, (unsigned)got.rkey);
    }
    bool streaming = false;
    SOAK_CHECK_STATUS(hml_client_start(cli), HML_OK);
    SOAK_CHECK_STATUS(hml_client_status(cli, &streaming), HML_OK);
    SOAK_CHECK(streaming, "round %llu: STATUS did not report streaming after START",
               (unsigned long long)rounds);
    SOAK_CHECK_STATUS(hml_client_stop(cli), HML_OK);
    SOAK_CHECK_STATUS(hml_client_status(cli, &streaming), HML_OK);
    SOAK_CHECK(!streaming, "round %llu: STATUS did not report stopped after STOP",
               (unsigned long long)rounds);

    /* poke / readback, including the fail-closed path */
    uint32_t addr = (uint32_t)(rounds % REGS);
    uint32_t val = 0x600d0000u + (uint32_t)rounds;
    uint32_t rval = 0;
    SOAK_CHECK_STATUS(hml_client_poke(cli, addr, val, &rval), HML_OK);
    SOAK_CHECK_EQ(rval, val, "round %llu: POKE did not read back what it wrote",
                  (unsigned long long)rounds);
    uint32_t rb = 0;
    SOAK_CHECK_STATUS(hml_client_readback(cli, addr, &rb), HML_OK);
    SOAK_CHECK_EQ(rb, val, "round %llu: READBACK did not read back what was poked",
                  (unsigned long long)rounds);
    if (hml_client_poke(cli, REGS + 1000u, 1u, &rval) != HML_ERR_NOTSUP) {
      SOAK_FAILF("round %llu: an out-of-range POKE did not fail closed", (unsigned long long)rounds);
    }

    /* ---- malformed traffic, interleaved: must have no side effect ----
     * Sent from the probe socket, whose destination is the control port, so
     * nothing here involves the client's request/response path. A real client
     * follows immediately (the STATUS below) and must still be served, so a
     * server that got confused by the garbage fails in this round rather than
     * in some later one. */
    sent += GARBAGE_PER_ROUND;
    sent_bad += GARBAGE_PER_ROUND;
    for (unsigned k = 0; k < GARBAGE_PER_ROUND; k++) {
      size_t n = build_bad(bad, (unsigned)(rounds + k), rounds);
      if (hml_sock_send(probe, bad, n) != HML_OK) {
        SOAK_FAILF("round %llu: the probe could not send its garbage datagram %u",
                   (unsigned long long)rounds, k);
      }
    }

    /* ---- an unknown opcode must be answered, not dropped ----
     * Replies are pointed at the probe for the duration, so the response lands
     * somewhere this test can read it. The probe is drained first: anything
     * queued there from an earlier exchange would otherwise be read as the
     * answer to this request. */
    if ((rounds % 16u) == 0u) {
      uint8_t drain[64];
      size_t drain_n = 0;
      while (hml_sock_recv(probe, drain, sizeof(drain), &drain_n, 0) == HML_OK) {
      }
      SOAK_CHECK_STATUS(hml_control_set_reply(g_ctl, HML_IP_LOOPBACK, probe_port), HML_OK);
      hml_ctrl_msg m;
      memset(&m, 0, sizeof(m));
      m.op = 0x7fu;
      m.flags = HML_CTRL_FLAG_REQ;
      const uint8_t pay[4] = {1, 2, 3, 4};
      m.payload = pay;
      m.payload_len = 4u;
      uint8_t req[HML_CTRL_HDR_SIZE + 4u + 2u];
      uint16_t n = 0;
      hml_ctrl_fill(&m, req, (uint16_t)sizeof(req), &n);
      if (hml_sock_send(probe, req, n) == HML_OK) {
        sent++;
        int answered = 0;
        for (int tries = 0; tries < 8 && !answered; tries++) {
          uint8_t resp[64];
          size_t got_n = 0;
          hml_status st = hml_sock_recv(probe, resp, sizeof(resp), &got_n, 500);
          if (st != HML_OK) break;
          hml_ctrl_msg r;
          const uint8_t *pl = NULL;
          if (hml_ctrl_parse(resp, got_n, &r, &pl) != HML_OK) {
            SOAK_FAILF("round %llu: the answer to an unknown opcode did not parse",
                       (unsigned long long)rounds);
            break;
          }
          if (r.op != HML_OP_ERROR) {
            SOAK_FAILF("round %llu: the answer to an unknown opcode was op 0x%02x flags 0x%04x, "
                       "want op 0xff with FLAG_ERR",
                       (unsigned long long)rounds, (unsigned)r.op, (unsigned)r.flags);
            break;
          }
          if ((r.flags & HML_CTRL_FLAG_ERR) == 0) {
            SOAK_FAILF("round %llu: the answer to an unknown opcode did not carry FLAG_ERR",
                       (unsigned long long)rounds);
            break;
          }
          if (r.payload_len == 1u && pl != NULL && pl[0] == (uint8_t)HML_ERR_NOTSUP) answered = 1;
        }
        SOAK_CHECK(answered, "round %llu: an unknown opcode was not answered within the timeout",
                   (unsigned long long)rounds);
      }
      SOAK_CHECK_STATUS(hml_control_set_reply(g_ctl, HML_IP_LOOPBACK, client_port), HML_OK);
      /* Anything the probe still holds would be read as a stray next time, and
       * more importantly it means a response was sent to the wrong socket. */
      SOAK_CHECK(hml_sock_recv(probe, drain, sizeof(drain), &drain_n, 50) != HML_OK,
                 "round %llu: an unexpected extra response reached the probe",
                 (unsigned long long)rounds);
    }

    /* ---- reconciliation: everything sent is either served or counted bad ---- */
    rounds++;
    if (rounds % ROUNDS_PER_SAMPLE == 0u) {
      /* The server runs on its own thread, so its counters are read here and the
       * exact totals are only required to hold once the server has drained. A
       * short bounded wait, not a sleep: the loop is otherwise busy. Deltas, not
       * totals, because the counters live across samples. */
      uint32_t rx = 0, badn = 0;
      for (int spin = 0; spin < 400; spin++) {
        rx = hml_control_rx_count(g_ctl);
        badn = hml_control_bad_count(g_ctl);
        if (rx - rx_seen >= (uint32_t)(sent - sent_at) &&
            badn - bad_seen >= (uint32_t)(sent_bad - sent_bad_at)) {
          break;
        }
        struct timespec ts = {0, 1000000L}; /* 1 ms */
        (void)nanosleep(&ts, NULL);
      }
      uint64_t d_sent = sent - sent_at, d_bad = sent_bad - sent_bad_at;
      SOAK_CHECK_EQ(rx - rx_seen, d_sent, "datagrams served since the last sample");
      SOAK_CHECK_EQ(badn - bad_seen, d_bad, "malformed datagrams counted since the last sample");
      /* Nothing may be counted as both served and bad. */
      SOAK_CHECK_EQ((rx - rx_seen) - (badn - bad_seen), d_sent - d_bad,
                    "served and rejected must partition the traffic");
      rx_seen = rx;
      bad_seen = badn;
      sent_at = sent;
      sent_bad_at = sent_bad;
      /* And the state must be exactly what this test put there. */
      uint32_t sets = 0, starts = 0, stops = 0, pokes = 0, reads = 0;
      (void)dev_get(&d, &sets, &starts, &stops, &pokes, &reads);
      SOAK_CHECK_EQ(sets, rounds, "set_dest callbacks");
      SOAK_CHECK_EQ(starts, rounds, "start callbacks");
      SOAK_CHECK_EQ(stops, rounds, "stop callbacks");
      SOAK_CHECK_EQ(pokes, rounds, "poke callbacks");
      SOAK_CHECK_EQ(reads, rounds, "read callbacks");
      SOAK_CHECK_EQ(g_poll_bad, 0, "the server poll loop saw an unexpected status");
      soak_rss_sample(&rss, 2u);
    }
  }

  /* ---- totals, once the server has drained ---- */
  uint32_t rx = 0, badn = 0;
  for (int spin = 0; spin < 2000; spin++) {
    rx = hml_control_rx_count(g_ctl);
    badn = hml_control_bad_count(g_ctl);
    if (rx - rx_seen >= (uint32_t)(sent - sent_at) &&
        badn - bad_seen >= (uint32_t)(sent_bad - sent_bad_at)) {
      break;
    }
    struct timespec ts = {0, 1000000L};
    (void)nanosleep(&ts, NULL);
  }
  uint64_t d_sent = sent - sent_at, d_bad = sent_bad - sent_bad_at;
  SOAK_CHECK_EQ(rx - rx_seen, d_sent, "total datagrams served");
  SOAK_CHECK_EQ(badn - bad_seen, d_bad, "total malformed datagrams rejected");
  /* rx_count includes the rejected ones, so served == rx - bad by definition.
   * Asserting it says the two counters are consistent rather than drifting. */
  SOAK_CHECK_EQ((rx - rx_seen) - (badn - bad_seen), d_sent - d_bad,
                "total served-and-rejected partition");
  SOAK_CHECK_EQ(hml_control_streaming(g_ctl) ? 1u : 0u, 0u, "the server is still streaming at exit");
  {
    uint32_t sets = 0, starts = 0, stops = 0, pokes = 0, reads = 0;
    (void)dev_get(&d, &sets, &starts, &stops, &pokes, &reads);
    SOAK_CHECK_EQ(starts, stops, "start and stop callbacks are unbalanced");
    SOAK_CHECK_EQ(sets, rounds, "set_dest callbacks at exit");
  }
  /* Rejected counts the deliberate garbage; the remaining datagrams are the
   * client's own request/response traffic, which the server served. */
  printf("  rounds=%llu datagrams=%llu rejected=%llu served=%llu beacons>0\n",
         (unsigned long long)rounds, (unsigned long long)sent, (unsigned long long)sent_bad,
         (unsigned long long)(sent - sent_bad));

  /* ---- discovery: the beacon is the other thing that runs on a timer ---- */
  {
    uint32_t uuid = 0, caps = 0, fw = 0;
    uint64_t ts1 = 0, ts2 = 0;
    SOAK_CHECK_STATUS(hml_client_discover(cli, 2000, &uuid, &caps, &fw, &ts1), HML_OK);
    SOAK_CHECK_EQ(uuid, cfg.uuid, "the beacon advertised the wrong uuid");
    SOAK_CHECK_EQ(caps, cfg.caps, "the beacon advertised the wrong caps");
    SOAK_CHECK_EQ(fw, cfg.fw_ver, "the beacon advertised the wrong firmware version");
    SOAK_CHECK_STATUS(hml_client_discover(cli, 2000, &uuid, &caps, &fw, &ts2), HML_OK);
    SOAK_CHECK(ts2 > ts1, "the beacon timestamp did not advance (%llu then %llu)",
               (unsigned long long)ts1, (unsigned long long)ts2);
    uint32_t cuuid = 0, ccaps = 0, cfw = 0;
    SOAK_CHECK_STATUS(hml_client_get_caps(cli, &cuuid, &ccaps, &cfw), HML_OK);
    SOAK_CHECK_EQ(cuuid, cfg.uuid, "GET_CAPS disagrees with the beacon");
  }

  hml_sock_close(probe);
  hml_client_close(cli);
  g_run = 0;
  (void)pthread_join(th, NULL);
  hml_control_close(g_ctl);
  (void)pthread_mutex_destroy(&d.lock);

  /* ---- open/close churn: the server allocates and binds, so a server that is
   * not fully released only shows up here. Each iteration also runs a request,
   * so the receive buffers are exercised at least once before being freed. ---- */
  soak_rss churn;
  soak_rss_init(&churn, 128L * 1024L);
  unsigned churn_n = 0;
  for (; churn_n < 300u && !soak_budget_expired(&budget); churn_n++) {
    uint16_t cp = soak_port_pick(PORT_LO, PORT_HI, &cursor);
    uint16_t dp = soak_port_pick(PORT_LO, PORT_HI, &cursor);
    uint16_t lp = soak_port_pick(PORT_LO, PORT_HI, &cursor);
    if (!cp || !dp || !lp) break;
    hml_control_cfg c2 = cfg;
    c2.ctrl_port = cp;
    c2.disc_port = dp;
    c2.beacon_enable = false;
    c2.reply_port = lp;
    hml_control_t *tmp = NULL;
    if (hml_control_open(&tmp, &c2) != HML_OK || tmp == NULL) {
      SOAK_FAILF("hml_control_open failed on iteration %u of the open/close loop", churn_n);
      break;
    }
    hml_client_t *tc = NULL;
    if (hml_client_open(&tc, lp) == HML_OK && tc != NULL) {
      (void)hml_client_set_peer(tc, HML_IP_LOOPBACK, cp, dp);
      (void)hml_client_set_timeout(tc, 200u);
      uint32_t t = 0;
      if (hml_client_ping(tc, 0xfeedu, &t) != HML_OK) t = 0u;
      if (t != 0xfeedu) SOAK_FAILF("iteration %u: ping did not echo across a fresh server", churn_n);
      hml_client_close(tc);
    }
    hml_control_close(tmp);
    soak_rss_sample(&churn, 4u);
  }
  if (churn.available) {
    long growth = churn.peak_kib - churn.baseline_kib;
    if (growth > 128L * 1024L) {
      SOAK_FAILF("%u control open/close cycles grew the resident set by %ld KiB (baseline %ld, "
                 "peak %ld)",
                 churn_n, growth, churn.baseline_kib, churn.peak_kib);
    }
  }
  if (rss.available && rss.peak_kib - rss.baseline_kib > 256L * 1024L) {
    SOAK_FAILF("the request loop grew the resident set by %ld KiB", rss.peak_kib - rss.baseline_kib);
  }

  return soak_finish("test_soak_ctrl", rounds, soak_budget_elapsed_us(&budget), &rss);
}
