/*
 * test_soak_roce -- bounded soak of the socket-crafted RoCEv2 data plane: the
 * real transmitter (hml_rocev2) against the real receiver/demultiplexer
 * (hml_roce_demux), over loopback, with faults injected in between.
 *
 * The reassembly soak covers the CPU-side fragment logic and the control soak
 * covers the request/response plane. Neither touches this one, and this is the
 * part of the tree where a leak is most likely to be invisible: the receiver
 * owns a simulated remote memory window, the transmitter owns a reassembly
 * context and a send buffer, and both are driven by long streams. A per-frame
 * leak in either would only show up as a slow RSS climb, which is exactly what
 * this test is shaped to detect.
 *
 * The invariant this test is really about is accounting, not throughput. Every
 * datagram the transmitter emits must end up in exactly one of the receiver's
 * buckets, and that must remain true at every sample:
 *
 *   pkts == writes + icrc_err + proto_err
 *
 * with nothing else able to move pkts. Held continuously, this pins down
 * "a dropped or corrupted datagram is accounted for and cannot silently inflate
 * the frame count", and it is a much stronger statement than a final frame
 * count. The frame-level companion is:
 *
 *   writes + meta writes accounted == bytes written into the window
 *   eof_frames == frames whose meta record survived the CRC check
 *   delivered == eof_frames - frames lost to injected loss
 *
 * Faults are injected deterministically, on a fixed pattern, so two runs of the
 * same duration do the same work:
 *
 *   - every 37th datagram is dropped outright;
 *   - every 53rd has a single flipped payload byte, which must surface as an
 *     icrc_err because ICRC covers the payload;
 *   - every 71st has its RETH rkey rewritten, which must surface as a proto_err
 *     and not as an icrc_err, because the hook refreshes the ICRC so the
 *     receiver has to catch it as a protocol error;
 *   - every 89th has its VA moved outside the window, same expectation.
 *
 * The two "refresh the ICRC" corruptions are the interesting ones: a receiver
 * that only verified the CRC would accept them, so asserting they land in
 * proto_err and not icrc_err is testing the demux's parsing rather than its
 * checksum. The payload flip is the opposite: it must NOT be caught as a
 * protocol error, because the datagram is structurally perfect.
 *
 * The ICRC-verified case is the one that matters for correctness of the data
 * path, so the receiver runs with verify_icrc on; the meta CRC is checked by the
 * receiver and the payload is checked here in on_eof against the deterministic
 * content pattern, which catches a receiver that assembles the wrong bytes even
 * when every counter agrees.
 *
 * Phases, each with its own arithmetic, because a single blended budget cannot
 * distinguish "loss ate the frames" from "an accounting bug ate the frames":
 *
 *   clean  -- no faults. Every frame must be delivered.
 *   lossy  -- the four fault patterns above. Some frames are lost, and the loss
 *             must be fully explained by the datagrams that were taken away.
 *   churn  -- open/close of the receiver and the transmitter, which is where a
 *             context that is not fully released shows up.
 *
 *   test_soak_roce [--seconds N] [--port-base P]    default 5 seconds
 */

#include "hml_soak_util.h"

#include "hml/hml_crc.h"
#include "hml/hml_port.h"
#include "hml/hml_protocol.h"
#include "hml/hml_transport.h"
#include "hml_roce_demux.h"
#include "hml_roce_priv.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

#define PORT_LO 45300u
#define PORT_HI 45499u

#define WIN_BASE 0x100000ull
#define WIN_LEN 65536u
#define FRAME_SIZE 9000u
#define RKEY 0xdeadbeefu
#define PIECE 3000u
#define PIECES_PER_FRAME 3u
#define FRAGS_PER_FRAME 9u /* 3 pieces x 3 fragments */
#define DG_PER_FRAME 10u   /* +1 EOF metadata write */
#define INFLIGHT 48u       /* keeps the kernel queue from overflowing */
#define STREAM_ROUNDS 8u   /* frames per stream before the stream id changes */

#define DROP_EVERY 37u
#define FLIP_EVERY 53u
#define RKEY_EVERY 71u
#define VA_EVERY 89u

/* Frames per second the sender aims for. Not a target but a cap: the point is to
 * keep the fault pattern's density stable across machines, so a faster or slower
 * host runs the same numbers of faults rather than a different mixture. */
#define FRAMES_PER_ROUND 4u

/* ---- fault injection ---------------------------------------------------- */

typedef struct fault {
  bool armed;        /* false for the clean phase: the hook is installed but inert */
  uint64_t offered;  /* datagrams the transmitter handed to the hook */
  uint64_t dropped;  /* withheld by this hook */
  uint64_t flipped;  /* payload byte flipped (expect icrc_err) */
  uint64_t badkey;   /* rkey rewritten, ICRC refreshed (expect proto_err) */
  uint64_t badva;    /* VA moved out of window, ICRC refreshed (expect proto_err) */
} fault;

static fault g_fault;

static bool fault_hook(void *ctx, unsigned idx, uint8_t *dg, size_t *len) {
  (void)ctx;
  (void)idx;
  fault *f = &g_fault;
  f->offered++;
  if (!f->armed) return false;
  uint64_t n = f->offered;

  if ((n % DROP_EVERY) == 0u) {
    f->dropped++;
    return true;
  }
  if ((n % FLIP_EVERY) == 0u) {
    /* Payload bit flip. The header is untouched, so the datagram is structurally
     * valid and only the ICRC can catch it. */
    dg[HML_ROCE_OFF_PAYLOAD + 7u] ^= 0x40u;
    f->flipped++;
    return false;
  }
  if ((n % RKEY_EVERY) == 0u) {
    hml_put_be32(dg + HML_ROCE_OFF_RETH + 8u, 0x12345678u);
    f->badkey++;
  } else if ((n % VA_EVERY) == 0u) {
    hml_put_be64(dg + HML_ROCE_OFF_RETH, 0x0000dead00000000ull);
    f->badva++;
  } else {
    return false;
  }
  /* The RETH is inside the ICRC coverage, so refresh it: the receiver has to
   * report a protocol error, not a link error. */
  uint32_t ic = hml_crc32_ieee(dg + HML_ROCE_OFF_BTH, *len - 4u - HML_ROCE_OFF_BTH);
  dg[*len - 4u] = (uint8_t)ic;
  dg[*len - 3u] = (uint8_t)(ic >> 8);
  dg[*len - 2u] = (uint8_t)(ic >> 16);
  dg[*len - 1u] = (uint8_t)(ic >> 24);
  return false;
}

/* ---- receive thread ----------------------------------------------------- */

/*
 * The receiver owns the stats; the sender's thread wants to assert on them every
 * round. hml_roce_rx_stats() is a plain read of counters the receiver thread is
 * concurrently incrementing, so sampling it from the sender can catch pkts one
 * datagram ahead of the buckets that datagram is about to land in, and the
 * partition identity would fail intermittently for no reason.
 *
 * So the receiver thread publishes a whole snapshot through a seqlock: it bumps a
 * sequence counter to odd, writes the struct, bumps it to even. A reader retries
 * while the counter is odd or changed across the copy. That is one atomic load on
 * the hot path, no lock in the poll loop, and every sample is internally
 * consistent -- which is what makes "these four buckets sum to the datagrams read"
 * a meaningful assertion rather than a coin flip.
 */
typedef struct rxsnap {
  hml_roce_stats st;
  uint64_t delivered, bytes, bad_len, bad_crc, bad_content;
} rxsnap;

typedef struct rxrt {
  hml_roce_rx_t *rx;
  pthread_t th;
  atomic_int stop;
  atomic_uint_least64_t delivered;
  atomic_uint_least64_t bytes;
  atomic_uint_least64_t bad_len;
  atomic_uint_least64_t bad_crc;
  atomic_uint_least64_t bad_content;
  /* seqlock: odd = a write is in progress. */
  atomic_uint seq;
  rxsnap snap;
} rxrt;

static rxrt *g_rx;

/* The content of frame `frame_seq` is deterministic, so the assembled bytes can
 * be verified without keeping a shadow copy: frame[i] == (uint8_t)(seq + i). This
 * is what catches a receiver that assembles the wrong bytes while every counter
 * still adds up. */
static void on_eof(void *vctx, uint32_t frame_seq, const uint8_t *frame, size_t n, uint32_t crc32) {
  rxrt *s = (rxrt *)vctx;
  atomic_fetch_add(&s->delivered, 1);
  atomic_fetch_add(&s->bytes, (uint64_t)n);
  if (n != FRAME_SIZE) {
    atomic_fetch_add(&s->bad_len, 1);
    return;
  }
  if (crc32 != hml_roce_crc32_done(hml_roce_crc32_update(HML_ROCE_CRC32_INIT, frame, n))) {
    atomic_fetch_add(&s->bad_crc, 1);
    return;
  }
  for (size_t i = 0; i < n; i++) {
    if (frame[i] != (uint8_t)(frame_seq + i)) {
      atomic_fetch_add(&s->bad_content, 1);
      return;
    }
  }
}

static void rx_pump(rxrt *s, int timeout_ms) {
  hml_roce_stats st;
  (void)hml_roce_rx_poll(s->rx, timeout_ms);
  hml_roce_rx_stats(s->rx, &st); /* this thread is the only stats writer */

  /* Publish under the seqlock. The receiver's counters and the callback's atomics
   * are copied together, so a reader never sees a mix of two points in time --
   * in particular not eof_frames from after a meta write with delivered from
   * before the callback it invoked. */
  rxsnap snap;
  snap.st = st;
  snap.delivered = atomic_load(&s->delivered);
  snap.bytes = atomic_load(&s->bytes);
  snap.bad_len = atomic_load(&s->bad_len);
  snap.bad_crc = atomic_load(&s->bad_crc);
  snap.bad_content = atomic_load(&s->bad_content);

  atomic_fetch_add_explicit(&s->seq, 1u, memory_order_acq_rel); /* odd: write in progress */
  s->snap = snap;
  atomic_thread_fence(memory_order_release);
  atomic_fetch_add_explicit(&s->seq, 1u, memory_order_release); /* even: consistent again */
}

/* Read a consistent snapshot, retrying while the receiver is publishing one. */
static void rx_read(const rxrt *s, rxsnap *out) {
  for (int tries = 0; tries < 50000; tries++) {
    unsigned a = atomic_load_explicit(&s->seq, memory_order_acquire);
    if (a & 1u) {
      if (tries > 50) sched_yield();
      continue;
    }
    *out = s->snap;
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&s->seq, memory_order_acquire) == a) return;
    if (tries > 50) sched_yield();
  }
  printf("FAIL the receiver never published a consistent snapshot\n");
  g_soak_fails++;
  memset(out, 0, sizeof(*out));
}

static void *rx_main(void *arg) {
  rxrt *s = (rxrt *)arg;
  while (!atomic_load(&s->stop)) rx_pump(s, 20);
  for (int i = 0; i < 40; i++) rx_pump(s, 10); /* final drain */
  return NULL;
}

static void rx_start(rxrt *s) {
  atomic_store(&s->stop, 0);
  if (pthread_create(&s->th, NULL, rx_main, s) != 0) {
    printf("FAIL pthread_create for the RoCE receiver\n");
  }
}

static void rx_stop(rxrt *s) {
  atomic_store(&s->stop, 1);
  (void)pthread_join(s->th, NULL);
}

/* ---- transmit ----------------------------------------------------------- */

/* Flow control. The kernel receive queue holds far fewer datagrams than a burst
 * needs, so the sender is capped at INFLIGHT datagrams beyond what the receiver
 * has drained. The count comes from the receiver thread's seqlock snapshot, so
 * the sender reads a consistent pkts rather than a value mid-update.
 *
 * The comparison is against the datagrams actually put on the wire, not against
 * the ones offered to the fault hook: a dropped datagram is never received, so
 * counting it here would let the gap grow without bound and the sender would wait
 * forever. */
static uint64_t on_wire(void) { return g_fault.offered - g_fault.dropped; }

static void flow(void) {
  rxsnap snap;
  rx_read(g_rx, &snap);
  while (snap.st.pkts + INFLIGHT < on_wire()) {
    struct timespec ts = {0, 50000L}; /* 50 us */
    (void)nanosleep(&ts, NULL);
    rx_read(g_rx, &snap);
  }
}

static hml_status tx_one(const hml_xport_ops *ops, hml_xport_t *x, const hml_frame_hdr *h,
                         const uint8_t *p, size_t n) {
  for (int i = 0; i < 2000; i++) {
    hml_status st = ops->send_frag(x, h, p, n);
    if (st != HML_ERR_AGAIN) return st;
    struct timespec ts = {0, 200000L}; /* 200 us */
    (void)nanosleep(&ts, NULL);
  }
  return HML_ERR_TIMEOUT;
}

static hml_status tx_frame(const hml_xport_ops *ops, hml_xport_t *x, uint32_t seq, uint16_t stream,
                           uint8_t *frame_buf) {
  for (size_t i = 0; i < FRAME_SIZE; i++) frame_buf[i] = (uint8_t)(seq + i);
  size_t off = 0;
  uint16_t idx = 0;
  for (unsigned piece = 0; piece < PIECES_PER_FRAME; piece++) {
    size_t left = PIECE;
    while (left) {
      size_t take = left > HML_ROCE_MAX_PAYLOAD ? HML_ROCE_MAX_PAYLOAD : left;
      hml_frame_hdr h;
      memset(&h, 0, sizeof(h));
      h.stream_id = stream;
      h.seq = seq;
      h.frag_idx = idx;
      h.frag_cnt = FRAGS_PER_FRAME;
      h.ts_us = hml_clock_us();
      h.payload_len = (uint16_t)take;
      if (idx == 0) h.flags |= HML_FLAG_START;
      if (idx == FRAGS_PER_FRAME - 1) h.flags |= HML_FLAG_END;
      hml_status st = tx_one(ops, x, &h, frame_buf + off, take);
      if (st != HML_OK) return st;
      off += take;
      left -= take;
      idx++;
      flow();
    }
  }
  return HML_OK;
}

/* ---- phases ------------------------------------------------------------- */

/* Every datagram the receiver read must land in exactly one bucket, and every
 * frame it declared must have been verified byte for byte.
 *
 * The partition has four terms, not three: a datagram is either a window write, a
 * meta-slot write (which is what completes a frame and is counted by eof_frames
 * or meta_drops rather than by writes), an ICRC error, or a protocol error. The
 * meta write is a real RDMA WRITE and is counted in pkts, so leaving it out of the
 * identity makes it fail by one per frame.
 *
 * The one thing pkts can hold that is not in any bucket is a stale or duplicated
 * PSN, which the demux deliberately drops silently (hml_roce_demux.c:289) because
 * a reordered datagram is not an error. The transmitter never reorders, so this
 * identity holds exactly here; a non-zero skew would mean the two sides disagree
 * about the stream.
 *
 * pkts must also equal the datagrams actually put on the wire. That holds only
 * after the receiver has drained, so it is checked at drain points; mid-flight the
 * receiver is legitimately behind, and the check would be a race rather than a
 * finding. */
static void check_accounting(const rxsnap *a, const fault *f, uint64_t frames_sent,
                             const char *phase) {
  (void)f;
  SOAK_CHECK_EQ(a->st.pkts,
                a->st.writes + a->st.eof_frames + a->st.meta_drops + a->st.icrc_err +
                    a->st.proto_err,
                "%s: every datagram read must be a window write, a meta write, an ICRC error or "
                "a protocol error",
                phase);
  /* The callback runs synchronously inside rx_meta(), so once the receiver has
   * been polled a declared frame has already been delivered. The seqlock makes
   * the snapshot consistent across both, so this is an equality, not a bound. */
  SOAK_CHECK_EQ(a->st.eof_frames, a->delivered,
                "%s: every declared frame must have reached the callback", phase);
  SOAK_CHECK_EQ(a->bad_len + a->bad_crc + a->bad_content, 0u,
                "%s: no delivered frame may be truncated, CRC-wrong or misassembled", phase);
  SOAK_CHECK(a->delivered <= frames_sent, "%s: more frames delivered than were sent (%llu > %llu)",
             phase, (unsigned long long)a->delivered, (unsigned long long)frames_sent);
  /* Each surviving frame contributes its full data writes to the window, so the
   * window's byte count cannot be below the delivered frames' worth. A frame is
   * declared only when the meta CRC over the whole window matches, so a short
   * count here would mean a frame was declared from an incomplete window. */
  SOAK_CHECK(a->st.bytes >= a->delivered * FRAME_SIZE,
             "%s: the window absorbed %llu bytes for %llu delivered frames", phase,
             (unsigned long long)a->st.bytes, (unsigned long long)a->delivered);
  /* And the delivered byte count the callback accumulated must match, which is
   * what rules out a frame being declared with a truncated length. */
  SOAK_CHECK_EQ(a->bytes, a->delivered * FRAME_SIZE, "%s: the callback's byte count", phase);
}

/* Everything the receiver read must have been sent, and every fault must be
 * attributed to exactly the right check. Only valid once the receiver has
 * drained; see check_accounting. */
static void check_drained(const rxsnap *a, const fault *f, const char *phase) {
  SOAK_CHECK_EQ(a->st.pkts, f->offered - f->dropped,
                "%s: datagrams read must equal datagrams put on the wire", phase);
  /* A flipped payload byte must be an ICRC error, not a protocol error: the
   * datagram is structurally perfect and only the checksum can catch it. */
  SOAK_CHECK_EQ(a->st.icrc_err, f->flipped,
                "%s: payload flips must be ICRC errors and nothing else may be", phase);
  /* A rewritten rkey or an out-of-window VA, with the ICRC refreshed so the
   * checksum is valid, must be a protocol error: a receiver that only checked
   * the CRC would have accepted it. */
  SOAK_CHECK_EQ(a->st.proto_err, f->badkey + f->badva,
                "%s: re-addressed datagrams must be protocol errors", phase);
}

/* Wait for the receiver to have drained everything offered, so the totals are
 * final rather than mid-flight. Bounded, so a lost datagram cannot hang the run. */
static void drain(rxrt *s, const fault *f, int max_ms) {
  uint64_t want = f->offered - f->dropped;
  uint64_t deadline = hml_clock_us() + (uint64_t)max_ms * 1000ull;
  rxsnap snap;
  for (;;) {
    rx_read(s, &snap);
    if (snap.st.pkts >= want) return;
    if (hml_clock_us() >= deadline) return;
    struct timespec ts = {0, 1000000L};
    (void)nanosleep(&ts, NULL);
  }
}

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

  uint8_t frame_buf[FRAME_SIZE];
  rxrt s;
  memset(&s, 0, sizeof(s));

  /* The receiver binds the port, so pick one that is free before opening. */
  uint16_t port = soak_port_pick(PORT_LO, PORT_HI, &cursor);
  if (!port) {
    printf("FAIL no free UDP port in %u..%u for the RoCE soak\n", PORT_LO, PORT_HI);
    return 1;
  }

  hml_roce_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.udp_port = port;
  cfg.window_base = WIN_BASE;
  cfg.window_len_bytes = WIN_LEN;
  cfg.frame_size = FRAME_SIZE;
  cfg.rkey = RKEY;
  cfg.verify_icrc = true;
  SOAK_CHECK_STATUS(hml_roce_rx_open(&s.rx, &cfg, on_eof, &s), HML_OK);
  if (s.rx == NULL) {
    printf("FAIL hml_roce_rx_open returned HML_OK but no receiver\n");
    return 1;
  }

  hml_dest_cfg dest;
  memset(&dest, 0, sizeof(dest));
  dest.kind = HML_XPORT_ROCEV2;
  dest.ipv4 = HML_ROCE_LOOPBACK_IP;
  dest.udp_port = port;
  dest.qp = 1;
  dest.psn = 0x00200001u;
  dest.rkey = RKEY;
  dest.remote_addr = WIN_BASE;
  dest.frame_size = FRAME_SIZE;
  dest.window_len = 16u;
  dest.hop_limit = 64u;

  const hml_xport_ops *ops = hml_rocev2_ops();
  hml_xport_t x;
  x.ops = ops;
  x.impl = NULL;
  SOAK_CHECK_STATUS(ops->open(&x, &dest, 1u), HML_OK);
  if (x.impl == NULL) {
    printf("FAIL the rocev2 open returned HML_OK but no context\n");
    hml_roce_rx_close(s.rx);
    return 1;
  }

  memset(&g_fault, 0, sizeof(g_fault));
  hml_roce_set_fault_hook(fault_hook, &g_fault); /* installed for the whole run */
  g_rx = &s;
  rx_start(&s);

  soak_budget budget;
  soak_budget_init(&budget, seconds);
  soak_rss rss;
  soak_rss_init(&rss, 256L * 1024L);

  uint32_t seq = 1u;
  uint64_t clean_frames = 0, lossy_frames = 0, rounds = 0;

  /* ---- phase 1: clean. No faults, so every frame must arrive. ----
   * The hook stays installed but drops nothing in this phase, because removing
   * and reinstalling it would add a data race between the two threads to a test
   * whose job is to find races. */
  {
    /* `armed` is the only thing that changes: the hook stays installed and keeps
     * counting offered datagrams, so the fault indices continue into the lossy
     * phase and the fault density is the specified one. Toggling the hook itself
     * would race the transmitter thread against the install for no benefit. */
    g_fault.armed = false;
    uint64_t deadline = hml_clock_us() + (uint64_t)seconds * 500000ull; /* half the budget */
    while (hml_clock_us() < deadline && !soak_budget_expired(&budget)) {
      for (unsigned k = 0; k < FRAMES_PER_ROUND; k++) {
        if (tx_frame(ops, &x, seq, 1u, frame_buf) != HML_OK) {
          SOAK_FAILF("the clean phase failed to send frame %u", seq);
          break;
        }
        seq++;
        clean_frames++;
      }
      rounds++;
      if ((rounds % 4u) == 0u) soak_rss_sample(&rss, 2u);
    }
    drain(&s, &g_fault, 1000);
    rxsnap a;
    rx_read(&s, &a);
    /* With nothing dropped and nothing corrupted, the only explanation for a
     * missing frame would be a bug, so require all of them. */
    SOAK_CHECK_EQ(g_fault.offered, clean_frames * DG_PER_FRAME,
                  "clean phase: datagrams emitted per frame");
    SOAK_CHECK_EQ(a.delivered, clean_frames, "clean phase: every frame must be delivered");
    SOAK_CHECK_EQ(a.st.icrc_err, 0u, "clean phase: ICRC errors");
    SOAK_CHECK_EQ(a.st.proto_err, 0u, "clean phase: protocol errors");
    SOAK_CHECK_EQ(a.st.meta_drops, 0u, "clean phase: meta drops");
    /* No fault was armed, so the transmitter's PSNs must arrive unbroken. A gap
     * here would mean datagrams went missing on a lossless loopback, which is a
     * kernel-drop or receiver bug rather than anything the demux can account for. */
    SOAK_CHECK_EQ(a.st.psn_gaps, 0u, "clean phase: PSN gaps");
    check_accounting(&a, &g_fault, clean_frames, "clean");
    check_drained(&a, &g_fault, "clean");
    printf("  clean: frames=%llu datagrams=%llu psn_gaps=%llu\n", (unsigned long long)clean_frames,
           (unsigned long long)g_fault.offered, (unsigned long long)a.st.psn_gaps);
  }

  /* ---- phase 2: lossy. Every frame is sent; some are not delivered, and the
   * loss must be fully explained. ---- */
  {
    g_fault.armed = true;
    while (!soak_budget_expired(&budget)) {
      for (unsigned k = 0; k < FRAMES_PER_ROUND; k++) {
        if (tx_frame(ops, &x, seq, (uint16_t)(1u + (seq / STREAM_ROUNDS) % 4u), frame_buf) !=
            HML_OK) {
          SOAK_FAILF("the lossy phase failed to send frame %u", seq);
          break;
        }
        seq++;
        lossy_frames++;
      }
      rounds++;
      if ((rounds % 4u) == 0u) soak_rss_sample(&rss, 2u);

      /* The partition and the content checks run every round; the attribution
       * does not, because at four frames a round the injected counts are too
       * small for an exact match to mean anything. Attribution is checked after
       * the drain, where the totals are final. */
      rxsnap a;
      rx_read(&s, &a);
      check_accounting(&a, &g_fault, clean_frames + lossy_frames, "lossy");
    }
    drain(&s, &g_fault, 1000);
    rxsnap a;
    rx_read(&s, &a);
    check_accounting(&a, &g_fault, clean_frames + lossy_frames, "lossy");
    check_drained(&a, &g_fault, "lossy");

    /* Loss accounting. A frame is declared only when its meta write arrives, and
     * the meta write's CRC covers the whole window, so a frame is lost if any of
     * its data writes or its meta write was dropped, flipped, re-addressed or
     * truncated. That makes "one damaged datagram can cost at most one frame" the
     * bound: frames lost must not exceed datagrams damaged. The exact count
     * depends on which fragment of which frame the pattern hit and is not
     * asserted, but exceeding the bound would mean a damaged datagram cost more
     * than its own frame, which would be a real accounting bug. */
    uint64_t damaged = g_fault.dropped + g_fault.flipped + g_fault.badkey + g_fault.badva;
    uint64_t sent_total = clean_frames + lossy_frames;
    uint64_t short_by = sent_total - a.delivered;
    SOAK_CHECK(short_by <= damaged,
               "lossy: %llu frames are missing, which is more than the %llu datagrams that were "
               "damaged or dropped",
               (unsigned long long)short_by, (unsigned long long)damaged);
    printf("  lossy: frames=%llu delivered=%llu datagrams=%llu dropped=%llu flipped=%llu "
           "badkey=%llu badva=%llu icrc_err=%llu proto_err=%llu meta_drops=%llu\n",
           (unsigned long long)lossy_frames, (unsigned long long)a.delivered,
           (unsigned long long)g_fault.offered, (unsigned long long)g_fault.dropped,
           (unsigned long long)g_fault.flipped, (unsigned long long)g_fault.badkey,
           (unsigned long long)g_fault.badva, (unsigned long long)a.st.icrc_err,
           (unsigned long long)a.st.proto_err, (unsigned long long)a.st.meta_drops);
    /* The fault patterns must all have fired often enough to mean something. */
    SOAK_CHECK(g_fault.dropped > 100u, "lossy: too few datagrams were dropped to be meaningful "
                                       "(%llu)",
               (unsigned long long)g_fault.dropped);
    SOAK_CHECK(g_fault.flipped > 50u, "lossy: too few payload flips (%llu)",
               (unsigned long long)g_fault.flipped);
    SOAK_CHECK(g_fault.badkey > 20u, "lossy: too few bad-rkey datagrams (%llu)",
               (unsigned long long)g_fault.badkey);
    SOAK_CHECK(g_fault.badva > 20u, "lossy: too few out-of-window VAs (%llu)",
               (unsigned long long)g_fault.badva);
  }

  rx_stop(&s);
  ops->close(&x);
  hml_roce_set_fault_hook(NULL, NULL);
  hml_roce_rx_close(s.rx);
  g_rx = NULL;

  /* ---- phase 3: churn. The receiver owns a window and a socket, the transmitter
   * owns a context and a send buffer, so an open/close leak only shows up here.
   * Every iteration also moves one frame end to end, so the buffers are dirtied
   * before they are freed -- a leak of a buffer that was never touched would
   * otherwise be invisible. ---- */
  soak_rss churn;
  soak_rss_init(&churn, 128L * 1024L);
  unsigned churn_n = 0;
  for (; churn_n < 200u && !soak_budget_expired(&budget); churn_n++) {
    uint16_t p = soak_port_pick(PORT_LO, PORT_HI, &cursor);
    if (!p) break;
    hml_roce_cfg c2 = cfg;
    c2.udp_port = p;
    rxrt r2;
    memset(&r2, 0, sizeof(r2));
    hml_roce_rx_t *rx = NULL;
    if (hml_roce_rx_open(&rx, &c2, on_eof, &r2) != HML_OK || rx == NULL) {
      SOAK_FAILF("hml_roce_rx_open failed on iteration %u of the churn loop", churn_n);
      break;
    }
    hml_dest_cfg d2 = dest;
    d2.udp_port = p;
    hml_xport_t x2;
    x2.ops = ops;
    x2.impl = NULL;
    if (ops->open(&x2, &d2, 1u) != HML_OK || x2.impl == NULL) {
      SOAK_FAILF("the rocev2 open failed on iteration %u of the churn loop", churn_n);
      hml_roce_rx_close(rx);
      break;
    }
    if (tx_frame(ops, &x2, 7u, 1u, frame_buf) != HML_OK) {
      SOAK_FAILF("iteration %u: the churn frame did not go out", churn_n);
    }
    ops->close(&x2);
    /* Drain without a receiver thread, so the datagrams are actually parsed and
     * the window really is written before it is released. */
    for (int i = 0; i < 200; i++) {
      hml_roce_stats st;
      if (hml_roce_rx_poll(rx, 5) == HML_OK) {
        hml_roce_rx_stats(rx, &st);
        if (st.pkts >= DG_PER_FRAME) break;
      }
    }
    hml_roce_stats st2;
    hml_roce_rx_stats(rx, &st2);
    if (st2.pkts > 0u) {
      SOAK_CHECK_EQ(st2.eof_frames, 1u, "iteration %u: the churn frame should have been delivered",
                    churn_n);
      SOAK_CHECK_EQ(r2.bad_len + r2.bad_crc + r2.bad_content, 0u,
                    "iteration %u: the churn frame did not verify", churn_n);
    }
    hml_roce_rx_close(rx);
    soak_rss_sample(&churn, 4u);
  }
  if (churn.available) {
    long growth = churn.peak_kib - churn.baseline_kib;
    if (growth > 128L * 1024L) {
      SOAK_FAILF("%u receiver/transmitter open/close cycles grew the resident set by %ld KiB "
                 "(baseline %ld, peak %ld)",
                 churn_n, growth, churn.baseline_kib, churn.peak_kib);
    }
  }

  printf("  total: rounds=%llu frames_sent=%llu (clean %llu + lossy %llu)\n",
         (unsigned long long)rounds, (unsigned long long)(clean_frames + lossy_frames),
         (unsigned long long)clean_frames, (unsigned long long)lossy_frames);
  return soak_finish("test_soak_roce", rounds, soak_budget_elapsed_us(&budget), &rss);
}
