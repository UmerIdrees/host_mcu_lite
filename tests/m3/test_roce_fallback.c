/*
 * M3 / T1a -- the T1a -> T0 fallback decision, as a unit test.
 *
 * hml_gen_roce --fallback-t0 has to degrade to the plain T0 UDP transport when,
 * and only when, the RoCEv2 transport cannot be opened, and it must never let a
 * degraded transport pass for the real one. This drives the same helper
 * (hml_roce_gen_xport_open) that the CLI drives, in-process, and checks:
 *
 *   A  a destination the RoCEv2 transport rejects (frame_size == 0, which would
 *      alias the metadata slot onto the first data write) -> udp ops
 *   B  a source port already held by a socket this test keeps open
 *      -> hml_sock bind fails with EADDRINUSE -> udp ops
 *   C  a valid destination -> rocev2 ops, and the fallback does NOT fire
 *   D  a rejected destination with fallback disabled -> the failure propagates
 *
 * A, B and D are deterministic and need no privileges, no NIC and no network.
 * A and B then send real T0 frames through the fallback transport into a
 * host/hml_host receiver and require every frame to arrive with a matching hash.
 */

#include "hml_t_m3.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>

#include "hml/hml_port.h"
#include "hml/hml_protocol.h"
#include "hml/hml_sock.h"
#include "hml/hml_stream.h"
#include "hml_roce_gen.h"
#include "hml_udp_priv.h"
#include "hml_roce_priv.h"
#include "hml_host.h"
#include "hml_synth_priv.h"

#define T0_PORT_A 14811u
#define T0_PORT_B 14812u
#define BUSY_PORT 14813u
#define WIN_BASE 0x100000ull
#define FRAME_SIZE 9000u
#define RKEY 0xDEADBEEFu
#define LOOPBACK 0x7F000001u
#define N_FRAMES 3u

static uint8_t g_frame[FRAME_SIZE];

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

typedef struct rx_state {
  uint32_t frames;
  uint32_t bad_hash;
  uint32_t bad_len;
  uint32_t last_hash;
  /* Expected hash per delivered frame_seq: the send buffer is reused for the
   * next frame long before the receiver may be polled, so the hash has to be
   * captured at send time, not recomputed in the callback. */
  uint32_t expect[32];
  bool expect_set[32];
} rx_state;

static void on_frame(void *vctx, uint16_t stream_id, uint32_t frame_seq, size_t frame_bytes,
                     uint32_t hash) {
  rx_state *s = (rx_state *)vctx;
  (void)stream_id;
  s->frames++;
  s->last_hash = hash;
  if (frame_bytes != (size_t)FRAME_SIZE) s->bad_len++;
  const size_t cap = sizeof(s->expect) / sizeof(s->expect[0]);
  if (frame_seq >= cap || !s->expect_set[frame_seq] || hash != s->expect[frame_seq]) {
    s->bad_hash++;
  }
}

/* Poll a hml_host-style receiver until it has `target` frames or the deadline. */
static int wait_frames(hml_host_t *h, rx_state *s, uint32_t target, int ms) {
  const int64_t deadline = now_ms() + ms;
  while (s->frames < target && now_ms() < deadline) {
    (void)hml_host_poll(h, 50);
  }
  return s->frames >= target ? 0 : -1;
}

/*
 * Open `dest` through the fallback helper and push N_FRAMES real T0 frames
 * through whatever transport came back, into a receiver on `rx_port`. Returns
 * the open status so the caller can assert on the decision as well.
 */
static hml_status drive_t0(const hml_dest_cfg *dest, bool allow_fallback, uint16_t src_port,
                           uint16_t rx_port) {
  hml_roce_gen_xport gx;
  hml_status st = hml_roce_gen_xport_open(&gx, dest, 1, src_port, allow_fallback);
  if (st != HML_OK) return st;

  T_CHECK(gx.ops != NULL, "open succeeded but ops is NULL");
  T_CHECK(strcmp(gx.ops->name, "udp") == 0, "expected the udp fallback ops, got '%s'",
          gx.ops->name);
  T_CHECK(gx.fallback, "fallback flag is not set although the RoCEv2 open failed");
  T_CHECK(gx.roce_status != HML_OK, "roce_status claims success");
  T_CHECK(gx.xport.ops == gx.ops, "xport.ops and ops disagree");
  T_CHECK(gx.xport.impl != NULL, "fallback transport has no private state");

  rx_state rs;
  memset(&rs, 0, sizeof(rs));
  hml_host_t *h = NULL;
  T_CHECK(hml_host_open(&h, rx_port, (size_t)HML_MAX_FRAME_SIZE) == HML_OK,
          "hml_host_open(udp/%u) failed", (unsigned)rx_port);
  if (!h) {
    hml_roce_gen_xport_close(&gx);
    return HML_ERR_STATE;
  }
  (void)hml_host_set_frame_cb(h, on_frame, &rs);

  hml_stream_cfg scfg;
  memset(&scfg, 0, sizeof(scfg));
  scfg.stream_id = 1;
  scfg.max_frame_bytes = FRAME_SIZE;
  scfg.seq_start = 0;
  scfg.mtu_effective = 1500;
  hml_stream_t *s = NULL;
  st = hml_stream_open(&s, &scfg, &gx.xport);
  T_CHECK(st == HML_OK, "hml_stream_open failed (%d)", (int)st);
  if (st == HML_OK) {
    for (uint32_t f = 0; f < N_FRAMES; f++) {
      hml_roce_gen_frame_fill(g_frame, FRAME_SIZE, f, 1);
      const uint32_t first_seq = hml_stream_next_seq(s);
      T_CHECK(first_seq < sizeof(rs.expect) / sizeof(rs.expect[0]),
              "frame %u would start at seq %u, past the expectation table", f, first_seq);
      size_t chunks = 0;
      st = hml_roce_gen_frame_send(s, g_frame, FRAME_SIZE, &chunks);
      if (st != HML_OK) {
        T_CHECK(0, "T0 send failed on frame %u (%d)", f, (int)st);
        break;
      }
      T_CHECK(chunks == 7u, "frame %u took %zu fragments, want 7", f, chunks);
      if (first_seq < sizeof(rs.expect) / sizeof(rs.expect[0])) {
        rs.expect[first_seq] = hml_fnv1a32(g_frame, FRAME_SIZE);
        rs.expect_set[first_seq] = true;
      }
    }
    T_CHECK(wait_frames(h, &rs, N_FRAMES, 5000) == 0, "receiver saw %u of %u frames", rs.frames,
            (unsigned)N_FRAMES);
    T_CHECK(rs.frames == N_FRAMES, "receiver saw %u frames, want %u", rs.frames,
            (unsigned)N_FRAMES);
    T_CHECK(rs.bad_len == 0, "%u frames had the wrong length", rs.bad_len);
    T_CHECK(rs.bad_hash == 0, "%u frames had the wrong hash", rs.bad_hash);
    printf("  fallback delivered %u frames, last hash 0x%08x\n", rs.frames, rs.last_hash);
    hml_stream_close(s); /* closes the transport */
  }

  hml_host_close(h);
  hml_roce_gen_xport_close(&gx); /* no-op after the stream closed it */
  return st;
}

static void base_dest(hml_dest_cfg *d, uint16_t port) {
  memset(d, 0, sizeof(*d));
  d->kind = HML_XPORT_ROCEV2;
  d->ipv4 = LOOPBACK;
  d->udp_port = port;
  d->qp = 1;
  d->psn = 1;
  d->rkey = RKEY;
  d->remote_addr = WIN_BASE;
  d->frame_size = FRAME_SIZE;
  d->window_len = 16;
  d->hop_limit = 64;
}

int main(void) {
  hml_dest_cfg dest;

  /* ---- A: a destination the RoCEv2 transport refuses --------------------- */
  base_dest(&dest, T0_PORT_A);
  dest.frame_size = 0; /* metadata slot would alias the first data write */
  T_CHECK(hml_roce_open_ex(NULL, &dest, 1, 0) == HML_ERR_ARG, "roce open ignored a NULL xport");
  T_CHECK(drive_t0(&dest, true, 0, T0_PORT_A) == HML_OK, "case A fallback did not complete");

  /* ---- B: a source port this process already holds ----------------------- */
  int holder = socket(AF_INET, SOCK_DGRAM, 0);
  T_CHECK(holder >= 0, "socket() failed: %s", strerror(errno));
  if (holder >= 0) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(0);
    sa.sin_port = htons(BUSY_PORT);
    /* No SO_REUSEADDR on purpose: hml_sock sets it, and a bind over a port that
     * is held without it is refused with EADDRINUSE. */
    T_CHECK(bind(holder, (struct sockaddr *)&sa, sizeof(sa)) == 0, "bind(udp/%u): %s",
            (unsigned)BUSY_PORT, strerror(errno));
    base_dest(&dest, T0_PORT_B);
    T_CHECK(drive_t0(&dest, true, BUSY_PORT, T0_PORT_B) == HML_OK,
            "case B fallback did not complete");
    (void)close(holder);
  }

  /* ---- C: a valid destination keeps the real T1a transport --------------- */
  base_dest(&dest, T0_PORT_A);
  {
    hml_roce_gen_xport gx;
    hml_status st = hml_roce_gen_xport_open(&gx, &dest, 1, 0, true);
    T_CHECK(st == HML_OK, "valid destination: open failed (%d)", (int)st);
    T_CHECK(gx.ops == hml_rocev2_ops(), "valid destination did not select the rocev2 ops");
    T_CHECK(strcmp(gx.ops->name, "rocev2") == 0, "ops name is '%s', want rocev2", gx.ops->name);
    T_CHECK(!gx.fallback, "fallback fired on a destination that opened cleanly");
    T_CHECK(gx.roce_status == HML_OK, "roce_status is %d, want HML_OK", (int)gx.roce_status);
    T_CHECK(gx.xport.impl != NULL, "rocev2 transport has no private state");
    hml_roce_gen_xport_close(&gx);
    T_CHECK(gx.xport.impl == NULL && gx.ops == NULL, "close left the context populated");
  }

  /* ---- D: fallback disabled propagates the failure ----------------------- */
  base_dest(&dest, T0_PORT_A);
  dest.frame_size = 0;
  {
    hml_roce_gen_xport gx;
    hml_status st = hml_roce_gen_xport_open(&gx, &dest, 1, 0, false);
    T_CHECK(st == HML_ERR_ARG, "fallback disabled: status %d, want HML_ERR_ARG", (int)st);
    T_CHECK(gx.ops == NULL, "fallback disabled: a transport was opened anyway");
    T_CHECK(gx.xport.impl == NULL, "fallback disabled: private state was left behind");
    T_CHECK(gx.roce_status == HML_ERR_ARG, "fallback disabled: roce_status is %d",
            (int)gx.roce_status);
  }
  T_CHECK(hml_roce_gen_xport_open(NULL, &dest, 1, 0, true) == HML_ERR_ARG,
          "helper ignored a NULL context");
  T_CHECK(hml_roce_gen_frame_send(NULL, g_frame, sizeof(g_frame), NULL) == HML_ERR_ARG,
          "frame send ignored a NULL stream");

  T_DONE("test_roce_fallback");
}
