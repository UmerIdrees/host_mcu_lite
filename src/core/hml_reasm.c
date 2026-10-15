#include "hml/hml_reasm.h"

#include "hml/hml_crc.h"
#include "hml/hml_latency.h"

#include <stdlib.h>
#include <string.h>

/*
 * v1 reassembly: one in-flight frame window per stream_id, HML_REASM_SLOTS slots.
 *
 * Fragments are staged in the frame buffer in ARRIVAL order; the fragment index of
 * every staged fragment is remembered so the frame can be laid out in index order
 * when the closing fragment arrives. Layout rule (v1, "MSS style" fragmentation):
 * all fragments but the last one carry the same payload length `chunk`, so
 * fragment i lives at i*chunk and the frame is (n-1)*chunk + len(last) bytes long.
 * A frame that does not satisfy that rule is dropped (counted, never delivered).
 *
 * Frame identity: hml_stream hands out one sequence number per fragment, so every
 * fragment of a frame satisfies seq == base + frag_idx, where base is the sequence of
 * fragment 0. base is derived from the first fragment that arrives (base = seq -
 * frag_idx, so it works for reversed delivery and for a lost first fragment) and every
 * later fragment is checked against it. A fragment that violates the check belongs to a
 * different frame and is never merged: it starts a new frame window and the old one is
 * dropped. This is what keeps two frames from silently mixing when both of their
 * boundary markers are lost, and it wraps cleanly at 2^32.
 *
 * Because a datagram that lost its END flag may still be in flight, a frame that
 * reaches HML_FLAG_END with missing indices is not dropped straight away: it becomes
 * "pending" and waits for its stragglers (this is what makes reversed delivery order
 * work). The loss is confirmed - gaps++ and frames_dropped++ - as soon as a datagram
 * proves the stragglers will not arrive, i.e. a fragment of the next frame. A frame
 * whose END never arrives is dropped the same way (frames_dropped++, plus gaps++ if it
 * had already been closed).
 *
 * Counters:
 *   frags_recv     : datagrams that parsed cleanly
 *   crc_fail       : hdr_crc16 or payload_crc16 mismatch
 *   proto_fail     : anything else rejected by hml_proto_frame_parse, plus
 *                    out-of-range frag_idx / frag_idx >= frag_cnt
 *   frames_ok      : frames delivered to the EOF callback with payload
 *   frames_dropped : frames discarded (gap, restart while in flight, overflow, layout)
 *   gaps           : frames abandoned because a fragment index never arrived
 *
 * Delivery: exactly one callback per assembled frame. A zero-payload HML_FLAG_END
 * datagram with no frame in flight is an "EOF-only flush": the callback fires with
 * frame_bytes == 0 and frames_ok is NOT incremented (so a host can count EOF
 * without a data frame).
 */
#define HML_REASM_SLOTS 2
#define HML_REASM_BITMAP_BYTES ((HML_MAX_FRAGS + 7u) / 8u)

typedef struct hml_reasm_slot {
  bool used;     /* slot bound to a stream_id */
  uint16_t stream_id;
  bool in_flight;
  bool poisoned; /* overflowed max_frame_bytes: swallow the rest of the frame */
  bool pending;  /* END seen, still waiting for missing indices */
  uint32_t seq_base; /* sequence of fragment 0 of this frame */
  hml_frame_hdr final_hdr;
  size_t bytes; /* staged payload bytes (arrival order) */
  uint16_t n_have;
  uint16_t max_idx;
  uint16_t frag_cnt; /* 0 = not signalled */
  uint8_t have[HML_REASM_BITMAP_BYTES];
  uint32_t arrive_off[HML_MAX_FRAGS];
  uint16_t frag_len[HML_MAX_FRAGS];
} hml_reasm_slot;

struct hml_reasm_ctx {
  hml_reasm_stats st;
  hml_reasm_eof_cb cb;
  void *cb_ctx;
  size_t cap;
  uint8_t *buf; /* staging area, fragments land here in arrival order */
  uint8_t *gather_buf; /* index-order gather buffer; frames are delivered from here */
  hml_reasm_slot slots[HML_REASM_SLOTS];
  hml_latency lat; /* EOF latency: hml_clock_us() - final_hdr->ts_us */
  bool owns_bufs;
  bool owns_ctx;
};

static void slot_clear_frame(hml_reasm_slot *s) {
  s->in_flight = false;
  s->poisoned = false;
  s->pending = false;
  s->seq_base = 0;
  s->bytes = 0;
  s->n_have = 0;
  s->max_idx = 0;
  s->frag_cnt = 0;
  memset(s->have, 0, sizeof(s->have));
}

static void slot_begin(hml_reasm_slot *s, const hml_frame_hdr *h) {
  slot_clear_frame(s);
  s->in_flight = true;
  s->used = true;
  s->stream_id = h->stream_id;
  s->seq_base = h->seq - (uint32_t)h->frag_idx; /* wraps like the sequence itself */
}

static bool bit_get(const hml_reasm_slot *s, uint16_t i) {
  return (s->have[i >> 3] & (uint8_t)(1u << (i & 7u))) != 0u;
}
static void bit_set(hml_reasm_slot *s, uint16_t i) {
  s->have[i >> 3] |= (uint8_t)(1u << (i & 7u));
}

static void deliver(hml_reasm_ctx_t *r, hml_reasm_slot *s, const uint8_t *frame, size_t n,
                    const hml_frame_hdr *final_hdr) {
  if (r->cb) r->cb(r->cb_ctx, s->stream_id, s->seq_base, frame, n, final_hdr);
}

/* Called on HML_FLAG_END and whenever a straggler lands on a pending frame. */
static void slot_finish(hml_reasm_ctx_t *r, hml_reasm_slot *s) {
  if (s->poisoned) {
    r->st.frames_dropped++;
    slot_clear_frame(s);
    return;
  }
  if (s->n_have == 0) { /* nothing staged: EOF-only flush */
    deliver(r, s, r->buf, 0, &s->final_hdr);
    slot_clear_frame(s);
    return;
  }

  uint16_t last = (s->frag_cnt != 0) ? (uint16_t)(s->frag_cnt - 1u) : s->max_idx;
  if (last >= HML_MAX_FRAGS) {
    r->st.gaps++;
    r->st.frames_dropped++;
    slot_clear_frame(s);
    return;
  }
  for (uint16_t i = 0; i <= last; i++) {
    if (!bit_get(s, i)) { /* fragment i has not arrived (yet): wait for the straggler */
      s->pending = true;
      return;
    }
  }

  /* complete: lay the staged fragments out in index order */
  size_t chunk = 0;
  for (uint16_t i = 0; i <= last; i++)
    if (s->frag_len[i] > chunk) chunk = s->frag_len[i];
  for (uint16_t i = 0; i < last; i++) {
    if (s->frag_len[i] != chunk) { /* not uniformly fragmentable in v1 */
      r->st.gaps++;
      r->st.frames_dropped++;
      slot_clear_frame(s);
      return;
    }
  }

  size_t total = (size_t)last * chunk + (size_t)s->frag_len[last];
  if (total > r->cap) {
    r->st.frames_dropped++;
    slot_clear_frame(s);
    return;
  }
  /* Lay the staged fragments out in index order. This is a gather, not an in-place
     permutation: fragments may have arrived in any order, so the two buffers must
     not be aliased. A single-fragment frame is already in index order. */
  const uint8_t *out = r->buf;
  if (last > 0) {
    for (uint16_t i = 0; i <= last; i++) {
      uint16_t l = s->frag_len[i];
      if (l == 0) continue;
      memcpy(r->gather_buf + (size_t)i * chunk, r->buf + s->arrive_off[i], l);
    }
    out = r->gather_buf;
  }

  r->st.frames_ok++;
  /* EOF latency tap: delta = receive clock - final fragment's ts_us. A final
     fragment with ts_us == 0 means a sender that never stamps (skip, count as
     skew); a negative delta means the two ends do not share a clock domain
     (unsigned wrap — count as skew, never record a fake value). */
  uint64_t now = hml_clock_us();
  if (s->final_hdr.ts_us == 0u || now < s->final_hdr.ts_us) {
    r->lat.skew_samples++;
  } else {
    (void)hml_latency_record(&r->lat, now - s->final_hdr.ts_us);
  }
  deliver(r, s, out, total, &s->final_hdr);
  slot_clear_frame(s);
}

size_t hml_reasm_ctx_size(void) {
  return sizeof(hml_reasm_ctx_t);
}

hml_status hml_reasm_open_buffers(hml_reasm_ctx_t **out, void *ctx_mem, size_t ctx_mem_size,
                                 uint8_t *staging_buf, uint8_t *gather_buf,
                                 size_t capacity) {
  if (!out || !staging_buf || !gather_buf || capacity == 0) return HML_ERR_ARG;
  *out = NULL;

  size_t cap = capacity;
  if (cap > (size_t)HML_MAX_FRAME_SIZE) cap = (size_t)HML_MAX_FRAME_SIZE;

  hml_reasm_ctx_t *r = NULL;
  bool owns_ctx = false;

  if (ctx_mem != NULL) {
    if (ctx_mem_size < sizeof(hml_reasm_ctx_t)) return HML_ERR_ARG;
    r = (hml_reasm_ctx_t *)ctx_mem;
    memset(r, 0, sizeof(*r));
  } else {
    r = (hml_reasm_ctx_t *)calloc(1, sizeof(*r));
    if (!r) return HML_ERR_NOMEM;
    owns_ctx = true;
  }

  r->buf = staging_buf;
  r->gather_buf = gather_buf;
  r->cap = cap;
  r->owns_bufs = false;
  r->owns_ctx = owns_ctx;
  (void)hml_latency_init(&r->lat, 0u);
  *out = r;
  return HML_OK;
}

hml_status hml_reasm_open(hml_reasm_ctx_t **out, size_t max_frame_bytes) {
  if (!out) return HML_ERR_ARG;
  *out = NULL;

  size_t cap = max_frame_bytes;
  if (cap == 0) cap = (size_t)HML_MAX_FRAME_SIZE;
  if (cap > (size_t)HML_MAX_FRAME_SIZE) cap = (size_t)HML_MAX_FRAME_SIZE;

  hml_reasm_ctx_t *r = (hml_reasm_ctx_t *)calloc(1, sizeof(*r));
  if (!r) return HML_ERR_NOMEM;
  /* note: 2 * max_frame_bytes of heap (staging + index-order gather) */
  r->buf = (uint8_t *)malloc(cap);
  r->gather_buf = (uint8_t *)malloc(cap);
  if (!r->buf || !r->gather_buf) {
    free(r->buf);
    free(r->gather_buf);
    free(r);
    return HML_ERR_NOMEM;
  }
  r->cap = cap;
  r->owns_bufs = true;
  r->owns_ctx = true;
  (void)hml_latency_init(&r->lat, 0u); /* default bucket width */
  *out = r;
  return HML_OK;
}

void hml_reasm_close(hml_reasm_ctx_t *r) {
  if (!r) return;
  if (r->owns_bufs) {
    free(r->buf);
    free(r->gather_buf);
  }
  if (r->owns_ctx) {
    free(r);
  }
}

void hml_reasm_set_eof_cb(hml_reasm_ctx_t *r, hml_reasm_eof_cb cb, void *ctx) {
  if (!r) return;
  r->cb = cb;
  r->cb_ctx = ctx;
}

void hml_reasm_reset(hml_reasm_ctx_t *r) {
  if (!r) return;
  for (unsigned i = 0; i < HML_REASM_SLOTS; i++) slot_clear_frame(&r->slots[i]);
}

void hml_reasm_get_stats(const hml_reasm_ctx_t *r, hml_reasm_stats *st) {
  if (!st) return;
  if (!r) {
    memset(st, 0, sizeof(*st));
    return;
  }
  *st = r->st;
}

void hml_reasm_get_latency(const hml_reasm_ctx_t *r, hml_latency *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!r) return;
  *out = r->lat;
}

hml_status hml_reasm_feed_datagram(hml_reasm_ctx_t *r, const uint8_t *dg, size_t len) {
  if (!r || (!dg && len > 0)) return HML_ERR_ARG;

  hml_frame_hdr h;
  const uint8_t *pay = NULL;
  hml_status st = hml_proto_frame_parse(dg, len, &h, &pay);
  if (st == HML_ERR_CRC) {
    r->st.crc_fail++;
    return st;
  }
  if (st != HML_OK) {
    r->st.proto_fail++;
    return st;
  }
  r->st.frags_recv++;

  if (h.frag_idx >= HML_MAX_FRAGS || (h.frag_cnt != 0 && h.frag_idx >= h.frag_cnt)) {
    r->st.proto_fail++;
    return HML_ERR_PROTO;
  }

  const bool start = (h.flags & HML_FLAG_START) != 0u;
  const bool end = (h.flags & HML_FLAG_END) != 0u;

  hml_reasm_slot *s = NULL;
  for (unsigned i = 0; i < HML_REASM_SLOTS; i++) {
    if (r->slots[i].used && r->slots[i].stream_id == h.stream_id) {
      s = &r->slots[i];
      break;
    }
  }
  if (!s) {
    unsigned victim = HML_REASM_SLOTS;
    for (unsigned i = 0; i < HML_REASM_SLOTS; i++) {
      if (!r->slots[i].used) {
        victim = i;
        break;
      }
    }
    if (victim == HML_REASM_SLOTS) { /* every slot is busy: drop the oldest frame */
      victim = 0;
      for (unsigned i = 1; i < HML_REASM_SLOTS; i++)
        if (r->slots[i].seq_base < r->slots[victim].seq_base) victim = i;
    }
    s = &r->slots[victim];
    if (s->in_flight) r->st.frames_dropped++;
    s->used = true;
    s->stream_id = h.stream_id;
    slot_clear_frame(s);
  }

  if (h.payload_len == 0 && end && !start && !s->in_flight) {
    /* EOF-only flush: wake the consumer without a data frame. */
    s->seq_base = h.seq - (uint32_t)h.frag_idx;
    s->final_hdr = h;
    deliver(r, s, r->buf, 0, &h);
    return HML_OK;
  }

  if (s->in_flight) {
    if (h.seq != s->seq_base + (uint32_t)h.frag_idx) {
      /* the fragment belongs to another frame: drop-oldest, and confirm a pending gap */
      if (s->pending) r->st.gaps++;
      r->st.frames_dropped++;
      slot_clear_frame(s);
    }
  }
  if (!s->in_flight) slot_begin(s, &h);
  if (h.frag_cnt != 0) s->frag_cnt = h.frag_cnt;

  bool added = false;
  if (h.payload_len > 0 && !bit_get(s, h.frag_idx)) { /* duplicates are idempotent */
    if ((size_t)h.payload_len > r->cap - s->bytes) {
      s->poisoned = true;
    } else {
      memcpy(r->buf + s->bytes, pay, h.payload_len);
      s->arrive_off[h.frag_idx] = (uint32_t)s->bytes;
      s->frag_len[h.frag_idx] = h.payload_len;
      s->bytes += h.payload_len;
      s->n_have++;
      if (h.frag_idx > s->max_idx) s->max_idx = h.frag_idx;
      bit_set(s, h.frag_idx);
      added = true;
    }
  }
  if (end) {
    s->final_hdr = h;
    slot_finish(r, s);
  } else if (added && s->pending) {
    slot_finish(r, s); /* a straggler completed the pending frame */
  }
  return HML_OK;
}
