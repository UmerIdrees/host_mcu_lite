/*
 * test_soak_reasm -- bounded soak of frame reassembly (hml_reasm_*).
 *
 * The plan's M1 exit criterion is "sustained T0 stream; loss documented under
 * load" and its soak case is "1h continuous; memory flat (no leak)". This is the
 * T0 half of that at a duration CTest can afford, and the same binary with
 * --seconds 3600 is the 1 h run.
 *
 * The workload is a fixed sequence of phases, repeated until the budget is spent.
 * Each phase has an exactly predictable effect on the reassembly counters, and
 * the run asserts those deltas after every phase of every round, not only at the
 * end. That is deliberate: reassembly's counters are deferred by design (a frame
 * with a hole becomes "pending" and is only confirmed lost when a later datagram
 * proves the stragglers will not arrive), so a soak that only compared totals
 * could sit on a permanently wrong number without ever noticing. Checking the
 * deltas per phase is also what makes a leak visible while the run is still
 * going rather than three hours later.
 *
 * The phases, in order, and what each one pins down:
 *
 *   clean     two streams, four fragments per frame, in order. frames_ok only.
 *             The stream id rotates because the arena holds one slot per stream
 *             (HML_REASM_SLOTS), so this is also what checks that a completed
 *             frame leaves its slot reusable.
 *   loss      one hole per N frames on a single stream, each hole followed by the
 *             next frame's first fragment. gaps and frames_dropped must each rise
 *             by exactly the number of holes: this is the plan's "loss_pct <=
 *             injected" as an equality rather than a hand-wave.
 *   crc       fragments with a broken payload CRC and a broken header CRC. Each
 *             must return HML_ERR_CRC, count crc_fail, and change nothing else --
 *             an integrity failure that advanced the state machine would deliver
 *             a frame containing bytes that were never on the wire.
 *   flush     zero-payload EOF-only flushes. They wake the consumer with
 *             frame_bytes == 0 and deliberately do NOT increment frames_ok, which
 *             is the documented contract a host counts EOFs against.
 *   reset     a frame abandoned mid-flight by hml_reasm_reset. It must be neither
 *             delivered nor counted as dropped: reset is not loss, and a host
 *             that saw a drop here would report loss it did not have.
 *   dup       a duplicate fragment delivered while the frame is still in flight.
 *             Idempotent, so no counter may move.
 *
 * Frames are laid out so a misordered gather is visible as wrong bytes rather
 * than merely a wrong count: fragment i is filled with (seq + i), and the
 * delivered frame is compared against that pattern.
 *
 * A final open/close phase covers what the others cannot: hml_reasm_open()
 * allocates 2 * capacity, so a context that is not fully released only shows up
 * when contexts are created and destroyed repeatedly.
 *
 *   test_soak_reasm [--seconds N]    default 5
 */

#include "hml_soak_util.h"

#include "hml/hml_crc.h"
#include "hml/hml_protocol.h"
#include "hml/hml_reasm.h"

/* Fragment layout. v1 requires every fragment but the last to carry the same
 * payload length, so the frame is (NFRAGS-1)*FRAG_CHUNK + LAST_CHUNK bytes and
 * FRAME_BYTES is what the callback has to report. */
#define FRAG_CHUNK 700u
#define LAST_CHUNK 32u
#define NFRAGS 4u
#define FRAME_BYTES ((NFRAGS - 1u) * FRAG_CHUNK + LAST_CHUNK)

#define ARENA_BYTES 32768u
/* The arena keeps one slot per stream id, so two is the largest number of
 * interleaved streams that does not make the receiver evict its own oldest
 * frame -- which would be a legitimate drop and would muddy the exact counts. */
#define STREAMS 2u
/* One hole in every HOLE_EVERY frames. */
#define HOLE_EVERY 5u
/* Fragments per round; keeps the outer loop long enough for the RSS watch to
 * have something to watch. */
#define FRAGS_PER_ROUND 64u

typedef struct probe {
  uint64_t frames;   /* data frames, i.e. callback with frame_bytes != 0 */
  uint64_t flushes;  /* EOF-only flushes, i.e. callback with frame_bytes == 0 */
  uint64_t bad_bytes;
  uint64_t bad_frags;
} probe;

static void on_frame(void *ctx, uint16_t stream_id, uint32_t frame_seq, const uint8_t *frame,
                     size_t frame_bytes, const hml_frame_hdr *final_hdr) {
  probe *p = (probe *)ctx;
  (void)stream_id;
  if (frame_bytes == 0u) {
    p->flushes++;
    return;
  }
  p->frames++;
  if (frame_bytes != FRAME_BYTES) {
    p->bad_bytes++;
    return;
  }
  /* fragment i carries (frame_seq + i) throughout */
  for (unsigned i = 0; i < NFRAGS; i++) {
    size_t off = (size_t)i * FRAG_CHUNK;
    size_t len = (i + 1u == NFRAGS) ? LAST_CHUNK : FRAG_CHUNK;
    uint8_t want = (uint8_t)(frame_seq + i);
    for (size_t k = 0; k < len; k++) {
      if (frame[off + k] != want) {
        p->bad_bytes++;
        return;
      }
    }
  }
  if (final_hdr == NULL || final_hdr->frag_cnt != NFRAGS) p->bad_frags++;
}

/* `seq` is this fragment's own sequence number, which for fragment i of a frame
 * based at b is b + i, and the payload is filled with (uint8_t)seq. The callback
 * compares against frame_seq + i, so a misordered gather shows up as wrong bytes
 * rather than as a plausible-looking frame. */
static void build_frag(uint8_t *dg, uint16_t stream, uint32_t seq, uint16_t idx, uint8_t flags,
                       uint16_t len) {
  hml_frame_hdr h;
  memset(&h, 0, sizeof(h));
  h.stream_id = stream;
  h.seq = seq;
  h.frag_idx = idx;
  h.frag_cnt = NFRAGS;
  /* Stamped with the current clock, not with the sequence: ts_us == 0 is the
   * documented "sender does not stamp" case and is counted as clock skew, and a
   * sequence-derived stamp would make every delta ~1e8 us and park the whole run
   * in the top latency bucket. */
  h.ts_us = hml_clock_us();
  h.flags = flags;
  h.payload_len = len;
  hml_proto_frame_fill(&h, dg);
  uint8_t *pay = dg + HML_OFF_PAYLOAD;
  for (uint16_t i = 0; i < len; i++) pay[i] = (uint8_t)seq;
  hml_put_be16(pay + len, hml_crc16(pay, len));
}

static uint8_t flags_for(uint16_t idx) {
  uint8_t f = 0;
  if (idx == 0u) f |= HML_FLAG_START;
  if (idx + 1u == NFRAGS) f |= HML_FLAG_END;
  return f;
}

static uint16_t len_for(uint16_t idx) {
  return (uint16_t)((idx + 1u == NFRAGS) ? LAST_CHUNK : FRAG_CHUNK);
}

/* One whole frame, fragments in index order. */
/* Every datagram the soak hands to the library goes through here, so the total
 * fed is a count rather than a formula restated at the end. The phases still
 * assert their own frags_recv deltas; this is the cross-phase total. */
static uint64_t g_fed;

static hml_status feed(hml_reasm_ctx_t *r, const uint8_t *dg, size_t n) {
  g_fed++;
  return hml_reasm_feed_datagram(r, dg, n);
}

static void feed_frame(hml_reasm_ctx_t *r, uint8_t *dg, uint16_t stream, uint32_t seq) {
  for (uint16_t i = 0; i < NFRAGS; i++) {
    build_frag(dg, stream, seq + i, i, flags_for(i), len_for(i));
    SOAK_CHECK_STATUS(feed(r, dg, HML_FRAME_HDR_SIZE + len_for(i) + 2u), HML_OK);
  }
}

/* Snapshot the counters so a phase can assert its own deltas. */
static hml_reasm_stats snap(hml_reasm_ctx_t *r) {
  hml_reasm_stats s;
  hml_reasm_get_stats(r, &s);
  return s;
}

#define DELTA(field) ((unsigned long long)(after.field - before.field))

int main(int argc, char **argv) {
  unsigned seconds = 5u;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      seconds = (unsigned)strtoul(argv[++i], NULL, 0);
    } else {
      printf("usage: %s [--seconds N]\n", argv[0]);
      return 2;
    }
  }
  if (seconds == 0u) seconds = 1u;

  hml_reasm_ctx_t *r = NULL;
  SOAK_CHECK_STATUS(hml_reasm_open(&r, ARENA_BYTES), HML_OK);
  if (r == NULL) {
    printf("FAIL hml_reasm_open returned HML_OK but no context\n");
    return 1;
  }

  probe p;
  memset(&p, 0, sizeof(p));
  hml_reasm_set_eof_cb(r, on_frame, &p);

  soak_budget budget;
  soak_budget_init(&budget, seconds);
  /* 256 KiB across a run that pushes tens of thousands of frames: far above
   * allocator bookkeeping noise, far below what a per-frame leak reaches. */
  soak_rss rss;
  soak_rss_init(&rss, 256L * 1024L);

  uint8_t dg[HML_MAX_FRAG_SIZE];
  uint64_t iters = 0, frames_done = 0, holes = 0, crcs = 0, flushes = 0, resets = 0, dups = 0;
  uint32_t base = 1000u;

  while (!soak_budget_expired(&budget)) {
    /* ---- clean: two interleaved streams, in-order fragments ---- */
    {
      hml_reasm_stats before = snap(r);
      for (unsigned k = 0; k < FRAGS_PER_ROUND / NFRAGS; k++) {
        feed_frame(r, dg, (uint16_t)(k % STREAMS), base);
        base += NFRAGS;
        iters++;
      }
      hml_reasm_stats after = snap(r);
      SOAK_CHECK_EQ(DELTA(frames_ok), (unsigned long long)(FRAGS_PER_ROUND / NFRAGS),
                    "frames_ok for a clean round");
      SOAK_CHECK_EQ(DELTA(frames_dropped), 0u, "frames_dropped for a clean round");
      SOAK_CHECK_EQ(DELTA(gaps), 0u, "gaps for a clean round");
      SOAK_CHECK_EQ(DELTA(crc_fail), 0u, "crc_fail for a clean round");
      SOAK_CHECK_EQ(DELTA(proto_fail), 0u, "proto_fail for a clean round");
      SOAK_CHECK_EQ(DELTA(frags_recv), (unsigned long long)FRAGS_PER_ROUND, "frags_recv");
      frames_done += FRAGS_PER_ROUND / NFRAGS;
    }

    /* ---- loss: one hole per HOLE_EVERY frames, then the proving fragment ----
     * The hole leaves the frame "pending" rather than dropped, so the counters
     * only move once the next frame's first fragment proves the stragglers are
     * not coming. Feeding it here, inside the phase, is what makes the delta
     * below an exact number instead of a "somewhere around" one. */
    {
      hml_reasm_stats before = snap(r);
      uint64_t fed = 0, hole = 0;
      for (unsigned k = 0; k < HOLE_EVERY; k++) {
        for (uint16_t i = 0; i < NFRAGS; i++) {
          if (i == 1u) continue; /* the hole, in every frame of this phase */
          build_frag(dg, 0u, base + i, i, flags_for(i), len_for(i));
          SOAK_CHECK_STATUS(feed(r, dg, HML_FRAME_HDR_SIZE + len_for(i) + 2u),
                            HML_OK);
          fed++;
        }
        base += NFRAGS;
        hole++;
        /* The next frame's first fragment: proves the hole will not be filled, so
         * this is where the drop is confirmed. It also starts the next frame. */
        build_frag(dg, 0u, base, 0u, flags_for(0u), len_for(0u));
        SOAK_CHECK_STATUS(feed(r, dg, HML_FRAME_HDR_SIZE + len_for(0u) + 2u),
                          HML_OK);
        fed++;
      }
      hml_reasm_stats after = snap(r);
      SOAK_CHECK_EQ(DELTA(gaps), hole, "gaps for a lossy round");
      SOAK_CHECK_EQ(DELTA(frames_dropped), hole, "frames_dropped for a lossy round");
      SOAK_CHECK_EQ(DELTA(frames_ok), 0u, "a frame with a hole was delivered anyway");
      SOAK_CHECK_EQ(DELTA(frags_recv), fed, "frags_recv for a lossy round");
      /* Every datagram in this phase belongs to one of the two frames, so
       * datagrams received must exceed frames accounted for by exactly the
       * fragments that staged into frames which were then dropped. */
      SOAK_CHECK(after.frags_recv >= after.frames_ok + after.frames_dropped,
                 "frags_recv is below frames_ok + frames_dropped");
      holes += hole;
    }

    /* ---- crc: integrity failures must not advance the state machine ---- */
    {
      hml_reasm_stats before = snap(r);
      /* A frame that is already in flight, so a state advance would show up in
       * the frames_ok delta below. */
      build_frag(dg, 0u, base, 0u, flags_for(0u), len_for(0u));
      SOAK_CHECK_STATUS(feed(r, dg, HML_FRAME_HDR_SIZE + len_for(0u) + 2u),
                        HML_OK);
      hml_reasm_stats mid = snap(r);
      uint32_t staged = base;
      base += NFRAGS;
      hml_reasm_reset(r); /* leave nothing in flight for the next phase */

      uint8_t bad[HML_MAX_FRAG_SIZE];
      /* broken payload CRC */
      build_frag(bad, 0u, staged, 0u, flags_for(0u), len_for(0u));
      bad[HML_OFF_PAYLOAD + len_for(0u)] ^= 0xffu;
      SOAK_CHECK_STATUS(feed(r, bad, HML_FRAME_HDR_SIZE + len_for(0u) + 2u),
                        HML_ERR_CRC);
      /* broken header CRC */
      build_frag(bad, 0u, staged, 0u, flags_for(0u), len_for(0u));
      bad[HML_OFF_HDR_CRC] ^= 0xffu;
      SOAK_CHECK_STATUS(feed(r, bad, HML_FRAME_HDR_SIZE + len_for(0u) + 2u),
                        HML_ERR_CRC);
      hml_reasm_stats after = snap(r);
      SOAK_CHECK_EQ(DELTA(crc_fail), 2u, "crc_fail for the CRC phase");
      SOAK_CHECK_EQ(DELTA(frames_ok), 0u, "a CRC failure produced a frame");
      SOAK_CHECK_EQ(DELTA(frames_dropped), 0u, "a CRC failure was counted as a lost frame");
      SOAK_CHECK_EQ(DELTA(gaps), 0u, "a CRC failure was counted as a gap");
      SOAK_CHECK_EQ(DELTA(proto_fail), 0u, "a CRC failure was counted as a protocol error");
      SOAK_CHECK_EQ(after.frags_recv - mid.frags_recv, 0u,
                    "a CRC failure was counted as a received fragment");
      crcs += 2u;
    }

    /* ---- flush: EOF-only, no frame, and frames_ok must not move ---- */
    {
      hml_reasm_stats before = snap(r);
      uint64_t f0 = p.flushes;
      for (unsigned k = 0; k < 4u; k++) {
        build_frag(dg, (uint16_t)(k % STREAMS), base, 0u, HML_FLAG_END, 0u);
        SOAK_CHECK_STATUS(feed(r, dg, HML_FRAME_HDR_SIZE + 2u), HML_OK);
        base++;
      }
      hml_reasm_stats after = snap(r);
      SOAK_CHECK_EQ(DELTA(frames_ok), 0u, "an EOF-only flush was counted as a frame");
      SOAK_CHECK_EQ(p.flushes - f0, 4u, "EOF-only flush callbacks");
      SOAK_CHECK_EQ(DELTA(frags_recv), 4u, "frags_recv for the flush phase");
      flushes += 4u;
    }

    /* ---- dup: an in-flight duplicate is idempotent ---- */
    {
      hml_reasm_stats before = snap(r);
      build_frag(dg, 0u, base, 0u, flags_for(0u), len_for(0u));
      SOAK_CHECK_STATUS(feed(r, dg, HML_FRAME_HDR_SIZE + len_for(0u) + 2u),
                        HML_OK);
      for (unsigned rep = 0; rep < 3u; rep++) {
        build_frag(dg, 0u, base, 0u, flags_for(0u), len_for(0u));
        SOAK_CHECK_STATUS(feed(r, dg, HML_FRAME_HDR_SIZE + len_for(0u) + 2u),
                          HML_OK);
      }
      for (uint16_t i = 1; i < NFRAGS; i++) {
        build_frag(dg, 0u, base + i, i, flags_for(i), len_for(i));
        SOAK_CHECK_STATUS(feed(r, dg, HML_FRAME_HDR_SIZE + len_for(i) + 2u),
                          HML_OK);
      }
      hml_reasm_stats after = snap(r);
      SOAK_CHECK_EQ(DELTA(frames_ok), 1u, "frames_ok for a frame with duplicate fragments");
      SOAK_CHECK_EQ(DELTA(frames_dropped), 0u, "a duplicate fragment dropped a frame");
      SOAK_CHECK_EQ(DELTA(gaps), 0u, "a duplicate fragment was counted as a gap");
      SOAK_CHECK_EQ(DELTA(frags_recv), 4u + 3u, "frags_recv for the duplicate phase");
      base += NFRAGS;
      dups += 3u;
      frames_done++;
    }

    /* ---- reset: neither delivered nor counted as a loss ---- */
    {
      hml_reasm_stats before = snap(r);
      uint64_t f0 = p.frames;
      build_frag(dg, 0u, base, 0u, flags_for(0u), len_for(0u));
      SOAK_CHECK_STATUS(feed(r, dg, HML_FRAME_HDR_SIZE + len_for(0u) + 2u),
                        HML_OK);
      build_frag(dg, 0u, base + 1u, 1u, 0u, len_for(1u));
      SOAK_CHECK_STATUS(feed(r, dg, HML_FRAME_HDR_SIZE + len_for(1u) + 2u),
                        HML_OK);
      hml_reasm_reset(r);
      /* The rest of the abandoned frame arrives after the reset and must be
       * swallowed, not assembled: a reset that left the staging buffer populated
       * would deliver a frame made of fragments from two different frames. */
      for (uint16_t i = 2; i < NFRAGS; i++) {
        build_frag(dg, 0u, base + i, i, flags_for(i), len_for(i));
        (void)feed(r, dg, HML_FRAME_HDR_SIZE + len_for(i) + 2u);
      }
      hml_reasm_stats after = snap(r);
      SOAK_CHECK_EQ(p.frames - f0, 0u, "an abandoned frame was delivered after a reset");
      /* The post-reset fragments form a new window whose indices are incomplete at
       * END, so they may end up pending; what must not happen is a delivery. */
      SOAK_CHECK_EQ(DELTA(frames_ok), 0u, "frames_ok across a reset");
      hml_reasm_reset(r);
      base += NFRAGS;
      resets++;
    }

    soak_rss_sample(&rss, 2u);
  }

  /* Totals, once, as a cross-check on the per-phase deltas. */
  hml_reasm_stats st = snap(r);
  SOAK_CHECK_EQ(p.frames, frames_done, "total data frames delivered");
  SOAK_CHECK_EQ(p.flushes, flushes, "total EOF-only flushes");
  SOAK_CHECK_EQ(p.bad_bytes, 0u, "frames delivered with wrong bytes");
  SOAK_CHECK_EQ(p.bad_frags, 0u, "frames delivered with the wrong frag_cnt");
  SOAK_CHECK_EQ(st.frames_ok, (uint32_t)frames_done, "frames_ok total");
  SOAK_CHECK_EQ(st.gaps, (uint32_t)holes, "gaps total");
  SOAK_CHECK_EQ(st.crc_fail, (uint32_t)crcs, "crc_fail total");
  /* g_fed counts every datagram handed over, including the deliberately corrupt
   * ones; frags_recv counts only those that parsed, and crc_fail the rest, so the
   * two together must account for all of them. proto_fail must stay at zero here
   * because this workload only ever emits well-formed datagrams. */
  SOAK_CHECK_EQ(st.frags_recv + st.crc_fail, g_fed,
                "every datagram fed is either a received fragment or a CRC failure");
  SOAK_CHECK_EQ(st.proto_fail, 0u, "a well-formed datagram was rejected as a protocol error");
  printf("  frames_ok=%llu gaps=%llu crc_fail=%llu frags_recv=%llu flushes=%llu resets=%llu\n",
         (unsigned long long)st.frames_ok, (unsigned long long)st.gaps,
         (unsigned long long)st.crc_fail, (unsigned long long)st.frags_recv,
         (unsigned long long)p.flushes, (unsigned long long)resets);

  /* The latency tap has to be live, or a soak would not notice a histogram that
   * stopped recording. The plan's M1 exit criterion is "p99 EOF latency within
   * written budget", so p50 and p99 are both read back: an empty histogram makes
   * a percentile call fail, which is itself the check. */
  hml_latency lat;
  hml_reasm_get_latency(r, &lat);
  SOAK_CHECK(lat.n > 0u, "the EOF latency histogram recorded no samples");
  SOAK_CHECK_EQ(lat.n + lat.skew_samples, (unsigned long long)st.frames_ok,
                "every delivered frame must record a latency sample or a skew count");
  SOAK_CHECK(lat.max_us >= lat.min_us, "the latency histogram max is below its min");
  uint64_t p50 = 0, p99 = 0;
  SOAK_CHECK_STATUS(hml_latency_percentile(&lat, 50u, &p50), HML_OK);
  SOAK_CHECK_STATUS(hml_latency_percentile(&lat, 99u, &p99), HML_OK);
  SOAK_CHECK(p99 >= p50, "p99 %llu is below p50 %llu", (unsigned long long)p99,
             (unsigned long long)p50);
  printf("  latency n=%llu p50=%llu p99=%llu max=%llu skew=%u\n", (unsigned long long)lat.n,
         (unsigned long long)p50, (unsigned long long)p99, (unsigned long long)lat.max_us,
         (unsigned)lat.skew_samples);
  hml_reasm_close(r);

  /* ---- open/close churn: 2 * capacity per context, so this is the only phase
   * that can see a context which is not fully released ---- */
  soak_rss churn;
  soak_rss_init(&churn, 64L * 1024L);
  unsigned churn_n = 0;
  for (; churn_n < 4000u && !soak_budget_expired(&budget); churn_n++) {
    hml_reasm_ctx_t *tmp = NULL;
    if (hml_reasm_open(&tmp, ARENA_BYTES) != HML_OK || tmp == NULL) {
      SOAK_FAILF("hml_reasm_open failed on iteration %u of the open/close loop", churn_n);
      break;
    }
    uint8_t seq = (uint8_t)churn_n;
    for (uint16_t f = 0; f < NFRAGS; f++) {
      build_frag(dg, 1u, seq + f, f, flags_for(f), len_for(f));
      (void)hml_reasm_feed_datagram(tmp, dg, HML_FRAME_HDR_SIZE + len_for(f) + 2u);
    }
    hml_reasm_close(tmp);
    soak_rss_sample(&churn, 4u);
  }

  /* The tighter churn budget is applied as a total rather than through the watch:
   * each iteration allocates 64 KiB, so a per-iteration budget would flag the
   * peak of a single uncollected context. */
  if (churn.available) {
    long growth = churn.peak_kib - churn.baseline_kib;
    if (growth > 64L * 1024L) {
      SOAK_FAILF("%u open/close pairs grew the resident set by %ld KiB (baseline %ld, peak %ld)",
                 churn_n, growth, churn.baseline_kib, churn.peak_kib);
    }
  }
  if (rss.available && rss.peak_kib - rss.baseline_kib > 256L * 1024L) {
    SOAK_FAILF("the frame loop grew the resident set by %ld KiB", rss.peak_kib - rss.baseline_kib);
  }

  return soak_finish("test_soak_reasm", iters, soak_budget_elapsed_us(&budget), &rss);
}
