/*
 * fuzz_reasm -- hml_reasm_feed_datagram (hml_reasm.h).
 *
 * This is the highest-value target in the tree: reassembly is the only place
 * where an untrusted datagram drives a multi-datagram state machine with
 * indices, sequence arithmetic and a fixed-size arena behind it, and it is the
 * code a hostile peer on the data port reaches first.
 *
 * Per input the harness does three things:
 *
 *   1. canary -- feed one known-good single-fragment frame and require exactly
 *      one delivery, the exact expected bytes, frames_ok == 1 and every other
 *      counter still zero. If the context is not in that state, no further
 *      assertion on it means anything.
 *   2. raw -- feed the input bytes, then every truncation prefix of them, to
 *      the parse-reject side.
 *   3. sequence -- build up to HML_FUZZ_REASM_MAX_DG sealed datagrams out of the
 *      input and feed them in a pattern the input chooses (in order, reversed,
 *      duplicated, with a hole, on two stream ids), so multi-fragment layout,
 *      gap confirmation, drop-oldest and the EOF-only flush are all reachable.
 *
 * The open capacity is chosen from the input as well, including 0xFFFFFFFF,
 * 0x7FFFFFFF and SIZE_MAX: hml_reasm_open takes a caller size_t and is required
 * to clamp it, and the allocation guard in hml_fuzz.c turns an unclamped value
 * into a FINDING instead of an OOM kill.
 *
 * Invariants asserted:
 *   - the canary holds on a fresh context whose capacity can hold it;
 *   - a delivered frame never exceeds the *effective* capacity, i.e. the one
 *     hml_reasm_open actually clamped to (a delivery longer than the arena is an
 *     out-of-bounds read waiting to happen);
 *   - a sealed single-fragment START|END frame always parses (HML_OK) and always
 *     delivers exactly the payload it carried;
 *   - counters never move backwards and frames_ok never exceeds frags_recv.
 *
 * The capacity the harness compares against is the clamped one, never the value
 * it asked for: hml_reasm_open is specified to clamp max_frame_bytes into
 * (0, HML_MAX_FRAME_SIZE], so asking for 0xFFFFFFFF and then asserting against
 * 0xFFFFFFFF would turn correct clamping into a finding.
 */

#include "hml_fuzz.h"

#include "hml/hml_crc.h"
#include "hml/hml_protocol.h"
#include "hml/hml_reasm.h"

#include <stdio.h>
#include <string.h>

#define HML_FUZZ_REASM_MAX_DG 8u
/* The canary's payload size. A context whose capacity is smaller than this can
 * legitimately refuse to assemble it, so the canary is skipped there. */
#define CANARY_PAYLOAD 16u

static unsigned long g_canaries, g_canary_skipped;

typedef struct reasm_probe {
  size_t cap;
  unsigned deliveries;
  /* Cumulative delivery count, so run_sequence can bound what one sequence of
   * datagrams produced without having to reset the counter. */
  unsigned long deliveries_total;
  unsigned long deliveries_before;
  size_t last_bytes;
  uint32_t last_seq;
  uint16_t last_stream;
  size_t max_seen;
  uint8_t first[64];
  size_t first_len;
} reasm_probe;

static void on_eof(void *ctx, uint16_t stream_id, uint32_t frame_seq, const uint8_t *frame,
                   size_t frame_bytes, const hml_frame_hdr *final_hdr) {
  reasm_probe *p = (reasm_probe *)ctx;
  if (frame_bytes > p->cap) {
    hml_fuzz_finding("delivered a %zu byte frame from a %zu byte arena", frame_bytes, p->cap);
  }
  if (frame != NULL && frame_bytes > 0u) {
    /* Touch every delivered byte: an out-of-range delivery is a read past the
     * arena, and this is the cheapest way to make the sanitizer see it. */
    volatile uint32_t sink = 0;
    for (size_t i = 0; i < frame_bytes; i++) sink += frame[i];
    (void)sink;
  }
  if (final_hdr == NULL) hml_fuzz_finding("EOF callback delivered a null final header");
  if (frame_bytes > p->max_seen) p->max_seen = frame_bytes;
  p->deliveries_total++;
  if (p->deliveries == 0u) {
    p->first_len = frame_bytes < sizeof(p->first) ? frame_bytes : sizeof(p->first);
    if (p->first_len > 0u && frame != NULL) memcpy(p->first, frame, p->first_len);
  }
  p->deliveries++;
  p->last_bytes = frame_bytes;
  p->last_seq = frame_seq;
  p->last_stream = stream_id;
}

/* ---- capacity selection: the caller-supplied size_t under attack ---------- */

static size_t pick_cap(const uint8_t *data, size_t n) {
  unsigned k = n > 0u ? (unsigned)(data[0] % HML_FUZZ_REASM_NCAPS) : 0u;
  return hml_fuzz_reasm_cap(k);
}

/* What hml_reasm_open will actually allocate for a requested capacity. */
static size_t effective_cap(size_t requested) {
  size_t cap = requested;
  if (cap == 0u) cap = (size_t)HML_MAX_FRAME_SIZE;
  if (cap > (size_t)HML_MAX_FRAME_SIZE) cap = (size_t)HML_MAX_FRAME_SIZE;
  return cap;
}

/* ---- canary: one known-good frame must come out exactly as it went in ----- */

static void run_canary(hml_reasm_ctx_t *r, reasm_probe *p) {
  uint8_t dg[HML_FRAME_HDR_SIZE + CANARY_PAYLOAD + HML_TRAIL_CRC_SIZE];
  hml_frame_hdr h;
  memset(&h, 0, sizeof(h));
  h.stream_id = 0x1234u;
  h.seq = 7u;
  h.frag_idx = 0u;
  h.frag_cnt = 1u;
  h.ts_us = 1000u;
  h.flags = (uint8_t)(HML_FLAG_START | HML_FLAG_END);
  h.payload_len = CANARY_PAYLOAD;
  hml_proto_frame_fill(&h, dg);
  for (unsigned i = 0; i < CANARY_PAYLOAD; i++) dg[HML_OFF_PAYLOAD + i] = (uint8_t)(0xa0u + i);
  uint16_t pc = hml_crc16(dg + HML_OFF_PAYLOAD, CANARY_PAYLOAD);
  dg[HML_OFF_PAYLOAD + CANARY_PAYLOAD] = (uint8_t)(pc >> 8);
  dg[HML_OFF_PAYLOAD + CANARY_PAYLOAD + 1u] = (uint8_t)pc;

  p->deliveries = 0u;
  p->first_len = 0u;
  if (hml_reasm_feed_datagram(r, dg, sizeof(dg)) != HML_OK)
    hml_fuzz_finding("a well-formed single-fragment frame was rejected by reassembly");
  if (p->deliveries != 1u) {
    hml_fuzz_finding("a well-formed single-fragment frame produced %u deliveries, want 1",
                     p->deliveries);
  }
  if (p->last_bytes != CANARY_PAYLOAD) {
    hml_fuzz_finding("canary frame delivered %zu bytes, want %u", p->last_bytes,
                     (unsigned)CANARY_PAYLOAD);
  }
  if (p->last_seq != 7u || p->last_stream != 0x1234u) {
    hml_fuzz_finding("canary frame delivered seq=%u stream=%u, want 7/4660",
                     (unsigned)p->last_seq, (unsigned)p->last_stream);
  }
  for (unsigned i = 0; i < p->first_len; i++) {
    if (p->first[i] != (uint8_t)(0xa0u + i)) {
      hml_fuzz_finding("canary payload byte %u is 0x%02x, want 0x%02x", i, p->first[i],
                       (unsigned)(0xa0u + i));
    }
  }

  hml_reasm_stats st;
  hml_reasm_get_stats(r, &st);
  if (st.frames_ok != 1u || st.frags_recv != 1u || st.gaps != 0u || st.crc_fail != 0u ||
      st.proto_fail != 0u || st.frames_dropped != 0u) {
    hml_fuzz_finding("canary counters: ok=%u recv=%u gaps=%u crc=%u proto=%u drop=%u, want "
                     "1/1/0/0/0/0",
                     (unsigned)st.frames_ok, (unsigned)st.frags_recv, (unsigned)st.gaps,
                     (unsigned)st.crc_fail, (unsigned)st.proto_fail,
                     (unsigned)st.frames_dropped);
  }
  g_canaries++;
  hml_reasm_reset(r);
}

/* ---- a sealed multi-datagram sequence driven by the input ----------------- */

static void run_sequence(hml_reasm_ctx_t *r, reasm_probe *p, const uint8_t *data, size_t n) {
  uint8_t sealed[HML_FUZZ_REASM_MAX_DG][HML_MAX_FRAG_SIZE];
  size_t len[HML_FUZZ_REASM_MAX_DG];
  size_t ndg = 1u + (n > 1u ? (size_t)(data[1] % HML_FUZZ_REASM_MAX_DG) : 0u);
  p->deliveries_before = p->deliveries_total;

  for (size_t i = 0; i < ndg; i++) {
    uint8_t slice[HML_FUZZ_MAX_SEAL];
    size_t sl = n > 2u ? n - 2u : 1u;
    const uint8_t *src = data + 2u;
    if (i < sl) {
      /* give each datagram a different slice of the input */
      size_t off = (i * 37u) % sl;
      size_t take = sl - off;
      if (take > sizeof(slice)) take = sizeof(slice);
      memcpy(slice, src + off, take);
      sl = take;
    } else {
      slice[0] = (uint8_t)i;
      sl = 1u;
    }
    size_t m = hml_fuzz_seal_frame(slice, sl, sealed[i], HML_MAX_FRAG_SIZE);
    if (m == 0u) return;
    len[i] = m;
  }

  /* Stamp the sequence identity the reassembler keys on, so the datagrams form
   * one frame (or deliberately do not) instead of four unrelated ones. */
  uint16_t base = (n > 3u) ? (uint16_t)((data[2] << 8) | data[3]) : 0u;
  uint16_t frag_cnt = (n > 5u) ? (uint16_t)(((uint32_t)data[4] << 8) | data[5]) : (uint16_t)ndg;
  if (frag_cnt == 0u || frag_cnt > (uint16_t)HML_MAX_FRAGS) frag_cnt = (uint16_t)ndg;
  unsigned order = n > 6u ? (unsigned)data[6] % 6u : 0u;

  for (size_t i = 0; i < ndg; i++) {
    size_t k = i;
    switch (order) {
      case 1: k = ndg - 1u - i; break;                     /* reversed */
      case 2: k = (i % 2u) ? ndg - 1u - (i / 2u) : i / 2u; break; /* shuffled */
      case 3: k = i / 2u; break;                           /* duplicates */
      case 4: k = (i + 1u) % ndg; break;                   /* off by one */
      case 5: k = (i == 0u) ? ndg - 1u : i - 1u; break;     /* a hole, then the rest */
      default: break;
    }
    hml_frame_hdr h;
    const uint8_t *pp = NULL;
    if (hml_proto_frame_parse(sealed[k], len[k], &h, &pp) != HML_OK) {
      hml_fuzz_finding("a sealed datagram built by the harness does not parse");
    }
    h.stream_id = base;
    h.seq = (uint32_t)base + (uint32_t)(k % (size_t)frag_cnt);
    h.frag_idx = (uint16_t)(k % (size_t)frag_cnt);
    h.frag_cnt = frag_cnt;
    h.flags = (uint8_t)((h.frag_idx == 0u ? HML_FLAG_START : 0u) |
                        (h.frag_idx + 1u >= frag_cnt ? HML_FLAG_END : 0u));
    hml_proto_frame_fill(&h, sealed[k]);
    {
      uint16_t pc = hml_crc16(sealed[k] + HML_OFF_PAYLOAD, h.payload_len);
      sealed[k][HML_OFF_PAYLOAD + h.payload_len] = (uint8_t)(pc >> 8);
      sealed[k][HML_OFF_PAYLOAD + h.payload_len + 1u] = (uint8_t)pc;
    }
    (void)hml_reasm_feed_datagram(r, sealed[k], len[k]);
  }

  /* A reassembler fed N datagrams cannot deliver more than N frames: each delivery
   * consumes at least one START fragment and fragments are never reused. This holds
   * for every arrival order above, including the duplicated and holed ones, which is
   * what makes it worth asserting rather than assuming. */
  if (p->deliveries_total - p->deliveries_before > ndg) {
    hml_fuzz_finding("fed %zu datagrams and got %u frame deliveries back", ndg,
                     p->deliveries_total - p->deliveries_before);
  }
}

static void run(const uint8_t *data, size_t n) {
  size_t requested = pick_cap(data, n);
  size_t cap = effective_cap(requested);

  hml_reasm_ctx_t *r = NULL;
  hml_status st = hml_reasm_open(&r, requested);
  if (st != HML_OK || r == NULL) {
    /* A capacity the library could not honour must be reported, not crash. The
     * allocation guard above has already caught an unclamped malloc. */
    hml_reasm_ctx_t *nil = NULL;
    hml_reasm_get_stats(nil, &(hml_reasm_stats){0});
    hml_reasm_feed_datagram(nil, data, n);
    hml_reasm_close(nil);
    return;
  }

  reasm_probe p;
  memset(&p, 0, sizeof(p));
  p.cap = cap;
  hml_reasm_set_eof_cb(r, on_eof, &p);
  if (cap >= CANARY_PAYLOAD) run_canary(r, &p); else g_canary_skipped++;

  /* Raw input, then every truncation prefix. */
  p.deliveries = 0u;
  uint32_t prev_frags = 0u;
  (void)hml_reasm_feed_datagram(r, data, n);
  hml_reasm_stats stx;
  hml_reasm_get_stats(r, &stx);
  if (stx.frames_ok > stx.frags_recv) {
    hml_fuzz_finding("frames_ok (%u) exceeds frags_recv (%u)", (unsigned)stx.frames_ok,
                     (unsigned)stx.frags_recv);
  }

  size_t sweep = n < 96u ? n : 96u;
  for (size_t k = 0; k <= sweep; k++) {
    (void)hml_reasm_feed_datagram(r, data, k);
    hml_reasm_stats sk;
    hml_reasm_get_stats(r, &sk);
    if (sk.frags_recv < prev_frags) hml_fuzz_finding("frags_recv went backwards");
    if (sk.frames_ok > sk.frags_recv) {
      hml_fuzz_finding("frames_ok (%u) exceeds frags_recv (%u)", (unsigned)sk.frames_ok,
                       (unsigned)sk.frags_recv);
    }
    prev_frags = sk.frags_recv;
  }

  /* A freshly opened context, so the sequence assertions start from a known
   * state and the canary proves the arena is intact. */
  hml_reasm_close(r);
  r = NULL;
  if (hml_reasm_open(&r, requested) != HML_OK || r == NULL) return;
  memset(&p, 0, sizeof(p));
  p.cap = cap;
  hml_reasm_set_eof_cb(r, on_eof, &p);
  if (cap >= CANARY_PAYLOAD) run_canary(r, &p);
  hml_reasm_reset(r);

  run_sequence(r, &p, data, n);
  if (p.max_seen > cap) {
    hml_fuzz_finding("the largest delivery was %zu bytes, above the %zu byte arena",
                     p.max_seen, cap);
  }

  hml_reasm_get_latency(r, &(hml_latency){0});
  hml_reasm_close(r);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  run(data, size);
  return 0;
}

#ifndef HML_FUZZ_LIBFUZZER
int main(int argc, char **argv) {
  int rc = hml_fuzz_main(argc, argv, "fuzz_reasm", run, NULL);
  printf("fuzz_reasm: canaries=%lu skipped_tiny_caps=%lu\n", g_canaries, g_canary_skipped);
  return rc;
}
#endif
