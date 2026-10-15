/*
 * gen_corpus -- writes the hml seed corpus.
 *
 *   gen_corpus <output-dir>
 *
 * The corpus is generated rather than committed: every case is either derived from
 * the wire layout in the frozen headers or is an exact byte pattern, so it stays
 * readable, stays diffable and cannot rot into a stale binary blob. The output is
 * deterministic -- no clock, no rand, no environment -- so two runs produce
 * identical files and a corpus diff always means a case actually changed.
 *
 * Layout:
 *   <dir>/<harness>/<name>.bin   one fuzz input
 *   <dir>/manifest.txt           "<harness>\t<file>\t<bytes>\t<tags>"
 *
 * The tags are the contract, and tests/fuzz/test_fuzz_corpus.c checks the bytes
 * behind every tag, so a tag that stops describing its case fails the build rather
 * than quietly weakening the corpus. Three families:
 *
 *   shape      what the record is: valid, truncated, trailing_junk, bad_magic,
 *              bad_version, bad_opcode, bad_proto, bad_rkey, bad_reserved,
 *              ihl_options, len_over_max, max_payload, zero_payload, non_utf8,
 *              non_utf8_header, embedded_nul, bad_icrc, oob_va, window_end
 *   value      an exact big-endian value somewhere in the bytes: v_0xffff,
 *              v_0x2000, v_7fffffff, v_ffffff00, v_ffffffff, v_sizemax
 *   capacity   which entry of the reassembly arena table the first byte selects,
 *              so the size_t under attack is a property of the case, not a guess
 *
 * max_len is not a value tag: it means "a declared length is absurd", so it is
 * only meaningful next to the value tag that produced it, and the test enforces
 * that pairing.
 */

#include "hml_fuzz.h"

#include "hml/hml_crc.h"
#include "hml/hml_ctrl.h"
#include "hml/hml_discovery.h"
#include "hml/hml_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every file this run emits, so emit() can reject a duplicate name. Two cases
 * with one name means the second silently overwrote the first while the manifest
 * grew two rows pointing at the same bytes. */
#define MAX_CASES 512

typedef struct manifest {
  FILE *f;
  unsigned count;
  char names[MAX_CASES][192];
} manifest;

/* 480 leaves room for a 470-character root under the 512-byte PATH_MAX budget. */
static char g_root[480];

static int emit(const char *harness, const char *name, const char *tags, const uint8_t *data,
                size_t n, manifest *m) {
  char key[192];
  (void)snprintf(key, sizeof(key), "%s/%s", harness, name);
  for (unsigned i = 0; i < m->count; i++) {
    if (strcmp(m->names[i], key) == 0) {
      fprintf(stderr, "gen_corpus: duplicate case name %s\n", key);
      return -1;
    }
  }
  if (m->count >= MAX_CASES) {
    fprintf(stderr, "gen_corpus: more than %d cases\n", MAX_CASES);
    return -1;
  }

  char path[640];
  int k = snprintf(path, sizeof(path), "%s/%s/%s.bin", g_root, harness, name);
  if (k < 0 || (size_t)k >= sizeof(path)) {
    fprintf(stderr, "gen_corpus: path too long for %s\n", key);
    return -1;
  }
  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    fprintf(stderr, "gen_corpus: cannot write %s\n", path);
    return -1;
  }
  if (n > 0u && fwrite(data, 1u, n, f) != n) {
    (void)fclose(f);
    fprintf(stderr, "gen_corpus: short write to %s\n", path);
    return -1;
  }
  (void)fclose(f);

  const char *base = strrchr(path, '/');
  base = base != NULL ? base + 1 : path;
  fprintf(m->f, "%s\t%s\t%zu\t%s\n", harness, base, n, tags);
  /* Any name that does not fit here would be truncated into a duplicate of some
   * other case, so refuse it instead. */
  int kn = snprintf(m->names[m->count], sizeof(m->names[0]), "%s", key);
  if (kn < 0 || (size_t)kn >= sizeof(m->names[0])) {
    fprintf(stderr, "gen_corpus: case name too long: %s\n", key);
    return -1;
  }
  m->count++;
  return 0;
}

static int emit_dir(const char *harness) {
  char path[600];
  int k = snprintf(path, sizeof(path), "%s/%s", g_root, harness);
  if (k < 0 || (size_t)k >= sizeof(path)) return -1;
  return hml_fuzz_mkdir_p(path);
}

/* Bytes that are not valid UTF-8: lone continuation bytes, an overlong encoding
 * of '/', and a surrogate half. If any layer treats a wire field as a string, one
 * of these has to be truncated at a bad boundary rather than at a NUL. */
static const uint8_t kNotUtf8[12] = {0xffu, 0xfeu, 0xc0u, 0xafu, 0xedu, 0xa0u,
                                     0x80u, 0xf5u, 0x80u, 0x80u, 0x80u, 0x80u};

/* ---- frame corpus -------------------------------------------------------- */

static size_t build_frame(uint8_t *out, uint16_t plen, uint32_t seq, uint16_t frag_idx,
                          uint16_t frag_cnt, uint8_t flags, uint8_t fill) {
  hml_frame_hdr h;
  memset(&h, 0, sizeof(h));
  h.stream_id = 0x0102u;
  h.seq = seq;
  h.frag_idx = frag_idx;
  h.frag_cnt = frag_cnt;
  h.ts_us = 0x0000000105f5e100ull;
  h.flags = flags;
  h.payload_len = plen;
  hml_proto_frame_fill(&h, out);
  for (uint16_t i = 0; i < plen; i++) out[HML_OFF_PAYLOAD + i] = (uint8_t)(fill + i);
  hml_put_be16(out + HML_OFF_PAYLOAD + plen, hml_crc16(out + HML_OFF_PAYLOAD, plen));
  return (size_t)HML_FRAME_HDR_SIZE + plen + HML_TRAIL_CRC_SIZE;
}

static int corpus_frame(manifest *m) {
  uint8_t buf[HML_MAX_FRAG_SIZE + 64u];
  const uint8_t start_end = (uint8_t)(HML_FLAG_START | HML_FLAG_END);
  if (emit_dir("fuzz_frame") != 0) return -1;

  size_t n = build_frame(buf, 0u, 0u, 0u, 1u, start_end, 0u);
  if (emit("fuzz_frame", "valid_eof_only", "valid zero_payload", buf, n, m) != 0) return -1;

  n = build_frame(buf, 16u, 0u, 0u, 1u, start_end, 0x10u);
  if (emit("fuzz_frame", "valid_single", "valid", buf, n, m) != 0) return -1;

  n = build_frame(buf, (uint16_t)HML_MAX_FRAG_PAYLOAD, 0u, 0u, 1u, start_end, 0x80u);
  if (emit("fuzz_frame", "valid_max_payload", "valid max_payload non_utf8", buf, n, m) != 0)
    return -1;

  /* A full-size payload with NULs at non-zero offsets: NUL-terminated handling of
   * a length-delimited field truncates the frame, and the payload CRC catches it. */
  n = build_frame(buf, (uint16_t)HML_MAX_FRAG_PAYLOAD, 0u, 0u, 1u, start_end, 0x20u);
  memcpy(buf + HML_OFF_PAYLOAD + 3u, "a\0b\0c", 5u);
  hml_put_be16(buf + HML_OFF_PAYLOAD + HML_MAX_FRAG_PAYLOAD,
               hml_crc16(buf + HML_OFF_PAYLOAD, HML_MAX_FRAG_PAYLOAD));
  if (emit("fuzz_frame", "valid_embedded_nul", "valid max_payload embedded_nul", buf, n, m) != 0)
    return -1;

  n = build_frame(buf, 16u, 0u, 0u, 1u, start_end, 0u);
  memcpy(buf + HML_OFF_PAYLOAD, kNotUtf8, 12u);
  hml_put_be16(buf + HML_OFF_PAYLOAD + 16u, hml_crc16(buf + HML_OFF_PAYLOAD, 16u));
  if (emit("fuzz_frame", "valid_non_utf8", "valid non_utf8", buf, n, m) != 0) return -1;

  n = build_frame(buf, 64u, 0u, 0u, 1u, start_end, 0u);
  buf[HML_OFF_MAGIC] ^= 0xffu;
  if (emit("fuzz_frame", "bad_magic", "bad_magic", buf, n, m) != 0) return -1;

  n = build_frame(buf, 64u, 0u, 0u, 1u, start_end, 0u);
  buf[HML_OFF_VERSION] = 2u;
  hml_put_be16(buf + HML_OFF_HDR_CRC, hml_crc16(buf, (size_t)HML_OFF_HDR_CRC));
  if (emit("fuzz_frame", "bad_version", "bad_version", buf, n, m) != 0) return -1;

  /* payload_len past the wire maximum, with a correct header CRC, so the range
   * check itself is the thing under test. */
  n = build_frame(buf, 64u, 0u, 0u, 1u, start_end, 0u);
  hml_put_be16(buf + HML_OFF_PAYLOAD_LEN, 0x2000u);
  hml_put_be16(buf + HML_OFF_HDR_CRC, hml_crc16(buf, (size_t)HML_OFF_HDR_CRC));
  if (emit("fuzz_frame", "payload_len_over_max", "len_over_max v_0x2000 max_len", buf, n, m) != 0)
    return -1;

  n = build_frame(buf, 16u, 0u, 0u, 1u, start_end, 0u);
  hml_put_be16(buf + HML_OFF_PAYLOAD_LEN, 0xffffu);
  hml_put_be16(buf + HML_OFF_HDR_CRC, hml_crc16(buf, (size_t)HML_OFF_HDR_CRC));
  if (emit("fuzz_frame", "payload_len_0xffff", "len_over_max v_0xffff max_len", buf, n, m) != 0)
    return -1;

  n = build_frame(buf, 16u, 0u, 0u, 1u, start_end, 0u);
  hml_put_be16(buf + HML_OFF_SEQ, 0xffffu);
  hml_put_be16(buf + HML_OFF_HDR_CRC, hml_crc16(buf, (size_t)HML_OFF_HDR_CRC));
  if (emit("fuzz_frame", "seq_0xffff", "v_0xffff", buf, n, m) != 0) return -1;

  n = build_frame(buf, 16u, 0u, 0xffffu, 0xffffu, start_end, 0u);
  hml_put_be16(buf + HML_OFF_HDR_CRC, hml_crc16(buf, (size_t)HML_OFF_HDR_CRC));
  if (emit("fuzz_frame", "frag_idx_cnt_0xffff", "v_0xffff", buf, n, m) != 0) return -1;

  n = build_frame(buf, 16u, 0u, 0u, 1u, start_end, 0u);
  hml_put_be16(buf + HML_OFF_HDR_CRC, (uint16_t)(hml_crc16(buf, (size_t)HML_OFF_HDR_CRC) ^ 1u));
  if (emit("fuzz_frame", "bad_hdr_crc", "bad_icrc", buf, n, m) != 0) return -1;

  n = build_frame(buf, 16u, 0u, 0u, 1u, start_end, 0u);
  buf[n - 1u] ^= 0xffu;
  if (emit("fuzz_frame", "bad_payload_crc", "bad_icrc", buf, n, m) != 0) return -1;

  n = build_frame(buf, 64u, 0u, 0u, 1u, start_end, 0u);
  if (emit("fuzz_frame", "truncated_header", "truncated", buf, (size_t)HML_FRAME_HDR_SIZE - 1u, m) != 0)
    return -1;
  if (emit("fuzz_frame", "truncated_payload", "truncated", buf, n - 4u, m) != 0) return -1;
  if (emit("fuzz_frame", "truncated_one", "truncated", buf, n - 1u, m) != 0) return -1;
  if (emit("fuzz_frame", "empty", "truncated", buf, 0u, m) != 0) return -1;

  /* A complete record plus one extra byte. hml_proto_frame_parse is length-driven,
   * so the extra byte has to be ignored rather than trusted as a length. */
  {
    uint8_t extra[HML_MAX_FRAG_SIZE + 8u];
    n = build_frame(extra, 16u, 0u, 0u, 1u, start_end, 0u);
    extra[n] = 0x5au;
    if (emit("fuzz_frame", "trailing_junk", "trailing_junk", extra, n + 1u, m) != 0) return -1;
  }

  /* The extremal size values at every multi-byte length-bearing offset the header
   * has, so no unchecked field can hide. 0xFFFFFFFF/0x7FFFFFFF/0x00FFFFFF are the
   * 4-byte shapes; SIZE_MAX is the 8-byte one. */
  static const size_t kOffsets[4] = {HML_OFF_PAYLOAD_LEN, HML_OFF_FRAG_IDX, HML_OFF_SEQ,
                                     HML_OFF_TS};
  static const char *kValueNames[5] = {"ffff", "7fffffff", "ffffff00", "ffffffff", "sizemax"};
  static const char *kValueTags[5] = {"v_0xffff", "v_7fffffff", "v_ffffff00", "v_ffffffff",
                                      "v_sizemax"};
  static const uint64_t kValues[5] = {0xffffull, 0x7fffffffull, 0x00ffffffull, 0xffffffffull,
                                      0xffffffffffffffffull};
  static const unsigned kWidths[5] = {2u, 4u, 4u, 4u, 8u};
  for (unsigned o = 0; o < 4u; o++) {
    /* A write that reached into the header CRC would be recomputed over the value,
     * destroying the run of bytes the tag is about -- and payload_len at offset 24
     * has only 2 bytes before the CRC, so the wide values cannot go there at all. */
    const size_t room = (size_t)HML_OFF_HDR_CRC - kOffsets[o];
    for (unsigned v = 0; v < 5u; v++) {
      if (kWidths[v] > room) continue;
      uint8_t tmp[128];
      char name[64];
      n = build_frame(tmp, 16u, 0u, 0u, 1u, start_end, 0u);
      if (kWidths[v] == 2u) {
        hml_put_be16(tmp + kOffsets[o], (uint16_t)kValues[v]);
      } else if (kWidths[v] == 4u) {
        hml_put_be32(tmp + kOffsets[o], (uint32_t)kValues[v]);
      } else {
        hml_put_be64(tmp + kOffsets[o], kValues[v]);
      }
      hml_put_be16(tmp + HML_OFF_HDR_CRC, hml_crc16(tmp, (size_t)HML_OFF_HDR_CRC));
      (void)snprintf(name, sizeof(name), "field_%02zu_%s", kOffsets[o], kValueNames[v]);
      if (emit("fuzz_frame", name, kValueTags[v], tmp, n, m) != 0) return -1;
    }
  }

  /* High bytes inside the header rather than the payload: catches a layer that
   * prints or copies a header field as a C string. */
  {
    uint8_t tmp[64];
    n = build_frame(tmp, 0u, 0u, 0u, 1u, start_end, 0u);
    memcpy(tmp + HML_OFF_STREAM, kNotUtf8, 6u);
    if (emit("fuzz_frame", "non_utf8_header", "non_utf8_header", tmp, n, m) != 0) return -1;
  }
  return 0;
}

/* ---- reassembly corpus --------------------------------------------------- */

#define FRAG_PAYLOAD 100u
#define FRAG_BYTES (HML_FRAME_HDR_SIZE + FRAG_PAYLOAD + HML_TRAIL_CRC_SIZE)
#define NFRAG 4u

static int corpus_reasm(manifest *m) {
  uint8_t buf[HML_MAX_FRAG_SIZE];
  uint8_t seq_buf[NFRAG * FRAG_BYTES];
  if (emit_dir("fuzz_reasm") != 0) return -1;

  size_t n = build_frame(buf, 0u, 0u, 0u, 0u, HML_FLAG_END, 0x00u);
  if (emit("fuzz_reasm", "eof_only_flush", "valid zero_payload", buf, n, m) != 0) return -1;

  n = build_frame(buf, 16u, 0u, 0u, 0u, (uint8_t)(HML_FLAG_START | HML_FLAG_END), 0x11u);
  if (emit("fuzz_reasm", "single_fragment", "valid", buf, n, m) != 0) return -1;

  /* A complete NFRAG-fragment frame, laid out in arrival order. */
  for (uint16_t i = 0; i < NFRAG; i++) {
    (void)build_frame(seq_buf + (size_t)i * FRAG_BYTES, (uint16_t)FRAG_PAYLOAD, 40u + i, i,
                      (uint16_t)NFRAG,
                      (uint8_t)((i == 0u ? HML_FLAG_START : 0u) | (i == NFRAG - 1u ? HML_FLAG_END : 0u)),
                      (uint8_t)(0x30u + i));
  }
  if (emit("fuzz_reasm", "four_fragments", "valid", seq_buf, NFRAG * FRAG_BYTES, m) != 0) return -1;

  /* The same frame reversed: a pending frame that completes on the straggler. */
  {
    uint8_t rev[NFRAG * FRAG_BYTES];
    for (size_t i = 0; i < NFRAG; i++) {
      memcpy(rev + i * FRAG_BYTES, seq_buf + (NFRAG - 1u - i) * FRAG_BYTES, FRAG_BYTES);
    }
    if (emit("fuzz_reasm", "four_fragments_reversed", "valid", rev, sizeof(rev), m) != 0) return -1;
  }

  /* Every fragment twice: duplicates must be idempotent, not a second frame. */
  {
    uint8_t dup[2u * NFRAG * FRAG_BYTES];
    memcpy(dup, seq_buf, sizeof(seq_buf));
    memcpy(dup + sizeof(seq_buf), seq_buf, sizeof(seq_buf));
    if (emit("fuzz_reasm", "four_fragments_duplicated", "valid", dup, 2u * sizeof(seq_buf), m) != 0)
      return -1;
  }

  /* A hole in the middle: the frame can never be delivered, so it is a gap. */
  {
    uint8_t hole[NFRAG * FRAG_BYTES];
    size_t hoff = 0u;
    for (uint16_t i = 0; i < NFRAG; i++) {
      if (i == 2u) continue;
      memcpy(hole + hoff, seq_buf + (size_t)i * FRAG_BYTES, FRAG_BYTES);
      hoff += FRAG_BYTES;
    }
    if (emit("fuzz_reasm", "four_fragments_hole", "valid", hole, hoff, m) != 0) return -1;
  }

  /* Two stream ids interleaved, which is what the two-slot window exists for. */
  {
    uint8_t two[4u * FRAG_BYTES];
    size_t t = 0u;
    for (uint16_t i = 0; i < 2u; i++) {
      for (uint16_t f = 0; f < 2u; f++) {
        hml_frame_hdr h;
        memset(&h, 0, sizeof(h));
        h.stream_id = (uint16_t)(1u + i);
        h.seq = 100u + f;
        h.frag_idx = f;
        h.frag_cnt = 2u;
        h.flags = (uint8_t)((f == 0u ? HML_FLAG_START : 0u) | HML_FLAG_END);
        h.payload_len = 8u;
        hml_proto_frame_fill(&h, two + t);
        for (uint8_t q = 0; q < 8u; q++) two[t + HML_OFF_PAYLOAD + q] = (uint8_t)(i * 16u + q);
        hml_put_be16(two + t + HML_OFF_PAYLOAD + 8u, hml_crc16(two + t + HML_OFF_PAYLOAD, 8u));
        t += (size_t)(HML_FRAME_HDR_SIZE + 8u + HML_TRAIL_CRC_SIZE);
      }
    }
    if (emit("fuzz_reasm", "two_streams_interleaved", "valid", two, t, m) != 0) return -1;
  }

  /* Truncations of a valid fragment, and no input at all. */
  n = build_frame(buf, 16u, 0u, 0u, 0u, (uint8_t)(HML_FLAG_START | HML_FLAG_END), 0x11u);
  for (unsigned k = 0; k < 6u; k++) {
    char name[32];
    (void)snprintf(name, sizeof(name), "trunc_prefix_%u", k);
    if (emit("fuzz_reasm", name, "truncated", buf, (size_t)k * (n / 6u), m) != 0) return -1;
  }
  if (emit("fuzz_reasm", "empty", "truncated", buf, 0u, m) != 0) return -1;

  /* frag_idx / frag_cnt far outside the arena, with a valid header CRC so the index
   * bound in hml_reasm_feed_datagram is the thing under test. */
  n = build_frame(buf, 16u, 0u, 0xffffu, 1u, (uint8_t)(HML_FLAG_START | HML_FLAG_END), 0x11u);
  hml_put_be16(buf + HML_OFF_HDR_CRC, hml_crc16(buf, (size_t)HML_OFF_HDR_CRC));
  if (emit("fuzz_reasm", "frag_idx_max", "v_0xffff", buf, n, m) != 0) return -1;
  n = build_frame(buf, 16u, 0u, 0u, 0xffffu, (uint8_t)(HML_FLAG_START | HML_FLAG_END), 0x11u);
  hml_put_be16(buf + HML_OFF_HDR_CRC, hml_crc16(buf, (size_t)HML_OFF_HDR_CRC));
  if (emit("fuzz_reasm", "frag_cnt_max", "v_0xffff", buf, n, m) != 0) return -1;

  /* Sequence arithmetic at the 32-bit wrap. Each fragment is well formed, so this
   * is a valid input that happens not to assemble. */
  for (unsigned k = 0; k < 3u; k++) {
    char name[32];
    uint8_t tmp[HML_MAX_FRAG_SIZE];
    (void)build_frame(tmp, 8u, 0xfffffffcu + (uint32_t)k, 3u, 4u, HML_FLAG_END, 0x44u);
    (void)snprintf(name, sizeof(name), "seq_wrap_%u", k);
    if (emit("fuzz_reasm", name, "valid", tmp, (size_t)HML_FRAME_HDR_SIZE + 8u + HML_TRAIL_CRC_SIZE,
             m) != 0)
      return -1;
  }

  /* Arena capacity selectors. The first byte picks the entry, and the input then
   * carries sealed datagrams, so each capacity is exercised with real frames. */
  {
    static const struct {
      unsigned index;
      const char *name;
      const char *tag;
    } kCaps[] = {
        {HML_FUZZ_CAP_0, "cap_zero", "cap_zero"},
        {HML_FUZZ_CAP_1, "cap_one", "cap_one"},
        {HML_FUZZ_CAP_HDR, "cap_hdr", "cap_hdr"},
        {HML_FUZZ_CAP_FRAG, "cap_frag", "cap_frag"},
        {HML_FUZZ_CAP_MAX_FRAME, "cap_max_frame", "cap_max_frame"},
        {HML_FUZZ_CAP_MAX_FRAME_1, "cap_max_frame_1", "cap_max_frame_1"},
        {HML_FUZZ_CAP_7FFFFFFF, "cap_7fffffff", "cap_7fffffff"},
        {HML_FUZZ_CAP_80000000, "cap_80000000", "cap_80000000"},
        {HML_FUZZ_CAP_FFFFFFFF, "cap_ffffffff", "cap_ffffffff"},
        {HML_FUZZ_CAP_SIZE_MAX, "cap_sizemax", "cap_sizemax"},
    };
    for (unsigned c = 0; c < sizeof(kCaps) / sizeof(kCaps[0]); c++) {
      uint8_t sel[2u * HML_MAX_FRAG_SIZE];
      size_t k = 1u;
      sel[0] = (uint8_t)kCaps[c].index;
      /* Two single-fragment datagrams so the arena has something to hold. */
      for (unsigned f = 0; f < 2u; f++) {
        k += build_frame(sel + k, 32u, (uint32_t)f, 0u, 1u,
                         (uint8_t)(HML_FLAG_START | HML_FLAG_END), (uint8_t)(0x60u + f));
      }
      if (emit("fuzz_reasm", kCaps[c].name, kCaps[c].tag, sel, k, m) != 0) return -1;
    }
  }
  return 0;
}

/* ---- hml_dest_cfg corpus ------------------------------------------------- */

static void build_cfg(uint8_t *out, uint8_t kind, uint32_t frame_size) {
  hml_dest_cfg c;
  memset(&c, 0, sizeof(c));
  c.kind = (hml_xport_kind)kind;
  c.ipv4 = 0x7f000001u;
  c.udp_port = 55101u;
  c.qp = 1u;
  c.psn = 1u;
  c.rkey = 0xdeadbeefu;
  c.remote_addr = 0x100000ull;
  c.frame_size = frame_size;
  c.window_len = 16u;
  c.hop_limit = 64u;
  hml_proto_cfg_fill(&c, out);
}

static int corpus_cfg(manifest *m) {
  uint8_t buf[HML_CFG_SIZE + 32u];
  if (emit_dir("fuzz_cfg") != 0) return -1;

  build_cfg(buf, (uint8_t)HML_XPORT_UDP, 9000u);
  if (emit("fuzz_cfg", "valid_udp", "valid", buf, (size_t)HML_CFG_SIZE, m) != 0) return -1;
  build_cfg(buf, (uint8_t)HML_XPORT_ROCEV2, 9000u);
  if (emit("fuzz_cfg", "valid_rocev2", "valid", buf, (size_t)HML_CFG_SIZE, m) != 0) return -1;

  build_cfg(buf, (uint8_t)HML_XPORT_UDP, 0xffffffffu);
  if (emit("fuzz_cfg", "frame_size_ffffffff", "v_ffffffff max_len", buf, (size_t)HML_CFG_SIZE, m) !=
      0)
    return -1;
  build_cfg(buf, (uint8_t)HML_XPORT_UDP, 0x7fffffffu);
  if (emit("fuzz_cfg", "frame_size_7fffffff", "v_7fffffff max_len", buf, (size_t)HML_CFG_SIZE, m) !=
      0)
    return -1;
  build_cfg(buf, (uint8_t)HML_XPORT_UDP, 0x00ffffffu);
  if (emit("fuzz_cfg", "frame_size_ffffff00", "v_ffffff00 max_len", buf, (size_t)HML_CFG_SIZE, m) !=
      0)
    return -1;

  build_cfg(buf, (uint8_t)HML_XPORT_UDP, 0u);
  buf[0] = 2u; /* neither udp nor rocev2 */
  if (emit("fuzz_cfg", "bad_kind", "bad_magic", buf, (size_t)HML_CFG_SIZE, m) != 0) return -1;

  /* pad0, pad1 and the 4 reserved bytes must all be zero. */
  build_cfg(buf, (uint8_t)HML_XPORT_UDP, 0u);
  buf[1] = 1u;
  if (emit("fuzz_cfg", "bad_pad0", "bad_reserved", buf, (size_t)HML_CFG_SIZE, m) != 0) return -1;
  build_cfg(buf, (uint8_t)HML_XPORT_UDP, 0u);
  buf[35] = 1u;
  if (emit("fuzz_cfg", "bad_pad1", "bad_reserved", buf, (size_t)HML_CFG_SIZE, m) != 0) return -1;
  build_cfg(buf, (uint8_t)HML_XPORT_UDP, 0u);
  memset(buf + 36, 0xff, 4u);
  if (emit("fuzz_cfg", "bad_rsvd", "bad_reserved", buf, (size_t)HML_CFG_SIZE, m) != 0) return -1;

  /* Every short length. 40 is the whole record, so the loop stops at 39: the full
   * length is the valid case, already emitted above. */
  build_cfg(buf, (uint8_t)HML_XPORT_UDP, 9000u);
  for (unsigned k = 0; k + 1u < HML_CFG_SIZE; k++) {
    char name[32];
    (void)snprintf(name, sizeof(name), "trunc_%u", k);
    if (emit("fuzz_cfg", name, "truncated", buf, k, m) != 0) return -1;
  }
  if (emit("fuzz_cfg", "empty", "truncated", buf, 0u, m) != 0) return -1;

  build_cfg(buf, (uint8_t)HML_XPORT_UDP, 9000u);
  hml_put_be32(buf + 8, 0xffffffffu); /* qp */
  hml_put_be32(buf + 12, 0x7fffffffu); /* psn */
  if (emit("fuzz_cfg", "qp_psn_max", "v_ffffffff v_7fffffff", buf, (size_t)HML_CFG_SIZE, m) != 0)
    return -1;
  build_cfg(buf, (uint8_t)HML_XPORT_UDP, 9000u);
  memset(buf + 16, 0xff, 8u); /* rkey + remote_addr */
  if (emit("fuzz_cfg", "rkey_addr_max", "v_ffffffff v_sizemax non_utf8", buf, (size_t)HML_CFG_SIZE,
           m) != 0)
    return -1;
  build_cfg(buf, (uint8_t)HML_XPORT_UDP, 9000u);
  memset(buf + 20, 0xff, 8u); /* remote_addr = SIZE_MAX */
  if (emit("fuzz_cfg", "remote_addr_sizemax", "v_sizemax", buf, (size_t)HML_CFG_SIZE, m) != 0)
    return -1;
  build_cfg(buf, (uint8_t)HML_XPORT_UDP, 9000u);
  buf[24] = 0x00u;
  buf[25] = 0x00u;
  buf[26] = 0x00u;
  buf[27] = 0x00u;
  buf[28] = 'a';
  buf[29] = 0u;
  if (emit("fuzz_cfg", "embedded_nul", "embedded_nul", buf, (size_t)HML_CFG_SIZE, m) != 0)
    return -1;
  build_cfg(buf, (uint8_t)HML_XPORT_UDP, 9000u);
  hml_put_be16(buf + 34, 0xffffu); /* window_len */
  if (emit("fuzz_cfg", "window_len_0xffff", "v_0xffff", buf, (size_t)HML_CFG_SIZE, m) != 0)
    return -1;

  /* Transport-registry names: NUL terminated, never longer than 31 bytes, and the
   * fuzz_cfg harness feeds each one to the registry as well as to the parser. */
  {
    static const char *const kNames[8] = {
        "udp", "rocev2", "", "hml-fuzz-probe", "\xff\xfe\xfd", "a",
        "0123456789abcdef0123456789abcde", /* 31 bytes: the longest allowed */
        "0123456789abcdef0123456789abcdef", /* 32 bytes: one over */
    };
    for (unsigned k = 0; k < 8u; k++) {
      uint8_t name[32];
      char file[32];
      memset(name, 0, sizeof(name));
      memcpy(name, kNames[k], strlen(kNames[k]));
      (void)snprintf(file, sizeof(file), "registry_name_%u", k);
      /* The 32-byte name fills the buffer with no NUL at all, which is the only one
       * of these that is not NUL terminated. */
      const char *tags = (k == 7u) ? "no_nul" : "embedded_nul";
      if (emit("fuzz_cfg", file, tags, name, sizeof(name), m) != 0) return -1;
    }
  }
  return 0;
}

/* ---- control codec corpus ------------------------------------------------ */

static size_t build_ctrl(uint8_t *out, uint8_t op, uint16_t flags, const uint8_t *pl,
                         uint16_t plen) {
  hml_ctrl_msg m;
  memset(&m, 0, sizeof(m));
  m.op = op;
  m.flags = flags;
  m.payload = pl;
  m.payload_len = plen;
  uint16_t n = 0;
  hml_ctrl_fill(&m, out, (uint16_t)(HML_CTRL_HDR_SIZE + plen + HML_TRAIL_CRC_SIZE), &n);
  return n;
}

static int corpus_ctrl(manifest *m) {
  uint8_t buf[HML_FUZZ_CTRL_MAX_MSG + 64u];
  uint8_t pl[HML_CTRL_MAX_PAYLOAD];
  if (emit_dir("fuzz_ctrl") != 0) return -1;

  memset(pl, 0xa5, sizeof(pl));

  size_t n = build_ctrl(buf, HML_OP_NOP, HML_CTRL_FLAG_REQ, NULL, 0u);
  if (emit("fuzz_ctrl", "valid_nop", "valid zero_payload", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 4u);
  if (emit("fuzz_ctrl", "valid_ping", "valid", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_POKE, HML_CTRL_FLAG_REQ, pl, 8u);
  if (emit("fuzz_ctrl", "valid_poke", "valid", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_SET_DEST, HML_CTRL_FLAG_REQ, pl, (uint16_t)HML_CFG_SIZE);
  if (emit("fuzz_ctrl", "valid_set_dest", "valid", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, (uint8_t)HML_OP_ERROR, HML_CTRL_FLAG_ERR, pl, 1u);
  if (emit("fuzz_ctrl", "valid_error", "valid", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, 0x7fu, HML_CTRL_FLAG_REQ, pl, 4u);
  if (emit("fuzz_ctrl", "valid_unknown_op", "valid", buf, n, m) != 0) return -1;

  /* The largest payload the control header can declare, with a valid CRC. */
  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, (uint16_t)HML_CTRL_MAX_PAYLOAD);
  if (emit("fuzz_ctrl", "valid_max_payload", "valid max_payload", buf, n, m) != 0) return -1;

  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, (uint16_t)HML_CTRL_MAX_PAYLOAD);
  memset(buf + 12, 0xff, 8u);
  hml_put_be16(buf + n - 2u, hml_crc16(buf, n - 2u));
  if (emit("fuzz_ctrl", "valid_max_payload_non_utf8", "valid max_payload non_utf8", buf, n, m) != 0)
    return -1;

  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 12u);
  memcpy(buf + 12, kNotUtf8, 12u);
  hml_put_be16(buf + 24, hml_crc16(buf, 24u));
  if (emit("fuzz_ctrl", "valid_non_utf8", "valid non_utf8", buf, n, m) != 0) return -1;

  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 8u);
  buf[HML_OFF_DISC_MAGIC] ^= 0xffu;
  if (emit("fuzz_ctrl", "bad_magic", "bad_magic", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 4u);
  buf[HML_OFF_CTRL_VERSION] = 2u;
  if (emit("fuzz_ctrl", "bad_version", "bad_version", buf, n, m) != 0) return -1;

  /* len is a u16 capped at HML_CTRL_MAX_PAYLOAD, so 513 and 0xFFFF are both
   * "one past the end" from the parser's point of view. */
  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 4u);
  hml_put_be16(buf + HML_OFF_CTRL_LEN, (uint16_t)HML_CTRL_MAX_PAYLOAD + 1u);
  if (emit("fuzz_ctrl", "len_over_max", "len_over_max", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 4u);
  hml_put_be16(buf + HML_OFF_CTRL_LEN, 0xffffu);
  if (emit("fuzz_ctrl", "len_0xffff", "len_over_max v_0xffff max_len", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 4u);
  hml_put_be16(buf + HML_OFF_CTRL_LEN, 0x7fffu);
  if (emit("fuzz_ctrl", "len_0x7fff", "len_over_max", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 4u);
  hml_put_be16(buf + HML_OFF_CTRL_RSVD, 1u); /* rsvd must be zero */
  if (emit("fuzz_ctrl", "bad_reserved", "bad_reserved", buf, n, m) != 0) return -1;

  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 4u);
  buf[n - 1u] ^= 0xffu;
  if (emit("fuzz_ctrl", "bad_crc", "bad_icrc", buf, n, m) != 0) return -1;

  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 4u);
  if (emit("fuzz_ctrl", "truncated_header", "truncated", buf, (size_t)HML_CTRL_HDR_SIZE, m) != 0)
    return -1;
  if (emit("fuzz_ctrl", "truncated_crc", "truncated", buf, n - 1u, m) != 0) return -1;
  if (emit("fuzz_ctrl", "truncated_payload", "truncated", buf, n - 3u, m) != 0) return -1;
  if (emit("fuzz_ctrl", "empty", "truncated", buf, 0u, m) != 0) return -1;

  /* hml_ctrl_parse demands len == 12 + len + 2 exactly, so a trailing byte has to
   * be refused rather than skipped. */
  {
    uint8_t ext[HML_FUZZ_CTRL_MAX_MSG + 2u];
    n = build_ctrl(ext, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 4u);
    ext[n] = 0x5au;
    if (emit("fuzz_ctrl", "trailing_junk", "trailing_junk", ext, n + 1u, m) != 0) return -1;
  }
  {
    uint8_t pre[HML_FUZZ_CTRL_MAX_MSG + 2u];
    memset(pre, 0, sizeof(pre));
    n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 8u);
    memcpy(pre + 1, buf, n);
    if (emit("fuzz_ctrl", "leading_junk", "bad_magic", pre, n + 1u, m) != 0) return -1;
  }

  /* A NUL inside the length field's own bytes: the parser must read the u16, not
   * stop at the NUL. */
  {
    uint8_t nul[HML_FUZZ_CTRL_MAX_MSG];
    memset(nul, 0, sizeof(nul));
    nul[0] = (uint8_t)(HML_MAGIC_CTRL >> 24);
    nul[1] = (uint8_t)(HML_MAGIC_CTRL >> 16);
    nul[2] = (uint8_t)(HML_MAGIC_CTRL >> 8);
    nul[3] = (uint8_t)HML_MAGIC_CTRL;
    nul[4] = (uint8_t)HML_CTRL_VERSION;
    nul[5] = HML_OP_PING;
    hml_put_be16(nul + 6, HML_CTRL_FLAG_REQ);
    hml_put_be16(nul + 8, 8u);
    hml_put_be16(nul + 10, 0u);
    memcpy(nul + 12, pl, 8u);
    hml_put_be16(nul + 20, hml_crc16(nul, 20u));
    /* declared len 8, actual payload 4 -> NUL inside the payload */
    memset(nul + 16, 0, 4u);
    hml_put_be16(nul + 20, hml_crc16(nul, 20u));
    if (emit("fuzz_ctrl", "embedded_nul", "embedded_nul", nul, 22u, m) != 0) return -1;
  }
  {
    uint8_t sz[HML_FUZZ_CTRL_MAX_MSG];
    n = build_ctrl(sz, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 4u);
    memset(sz + 12, 0xff, 8u);
    hml_put_be16(sz + 24, hml_crc16(sz, 24u));
    if (emit("fuzz_ctrl", "len_sizemax_payload", "v_sizemax", sz, 26u, m) != 0) return -1;
  }
  return 0;
}

/* ---- beacon corpus (same harness as the control codec) -------------------- */

static size_t build_beacon(uint8_t *out, uint64_t ts) {
  hml_beacon b;
  memset(&b, 0, sizeof(b));
  b.version = (uint8_t)HML_DISC_VERSION;
  b.ttl = 1u;
  b.uuid = 0x00000042u;
  b.caps = HML_CAP_UDP | HML_CAP_MCU;
  b.fw_ver = 0x00010400u;
  b.ts_us = ts;
  hml_beacon_fill(&b, out);
  return (size_t)HML_DISC_SIZE;
}

static int corpus_beacon(manifest *m) {
  uint8_t buf[HML_DISC_SIZE + 16u];
  if (emit_dir("fuzz_ctrl") != 0) return -1;

  size_t n = build_beacon(buf, 64082543642ull);
  if (emit("fuzz_ctrl", "beacon_valid", "valid", buf, n, m) != 0) return -1;
  /* The timestamp is 48 bits on the wire, so the top of a 64-bit value is dropped.
   * Both of these parse; that is the property worth pinning. */
  n = build_beacon(buf, 0x0000ffffffffffffull);
  if (emit("fuzz_ctrl", "beacon_ts_max", "valid", buf, n, m) != 0) return -1;
  n = build_beacon(buf, 0ull);
  if (emit("fuzz_ctrl", "beacon_ts_zero", "valid", buf, n, m) != 0) return -1;

  n = build_beacon(buf, 1234ull);
  buf[HML_OFF_DISC_MAGIC] ^= 0xffu;
  if (emit("fuzz_ctrl", "beacon_bad_magic", "bad_magic", buf, n, m) != 0) return -1;
  n = build_beacon(buf, 1234ull);
  buf[n - 1u] ^= 0xffu;
  if (emit("fuzz_ctrl", "beacon_bad_crc", "bad_icrc", buf, n, m) != 0) return -1;
  n = build_beacon(buf, 1234ull);
  /* hml_beacon_parse does not look at the version byte, so this record still
   * parses. Keeping the case is the point: the CRC still has to hold, and a future
   * version check must not be able to reject a case the corpus calls valid. */
  buf[HML_OFF_DISC_VERSION] = 2u;
  hml_put_be16(buf + HML_DISC_CRC_OFF, hml_crc16(buf, (size_t)HML_DISC_CRC_OFF));
  if (emit("fuzz_ctrl", "beacon_bad_version", "bad_version", buf, n, m) != 0) return -1;

  n = build_beacon(buf, 1234ull);
  for (unsigned k = 0; k < HML_DISC_SIZE; k++) {
    char name[32];
    (void)snprintf(name, sizeof(name), "beacon_trunc_%u", k);
    if (emit("fuzz_ctrl", name, "truncated", buf, k, m) != 0) return -1;
  }
  n = build_beacon(buf, 1234ull);
  memset(buf + 8, 0xff, 6u);
  hml_put_be16(buf + HML_DISC_CRC_OFF, hml_crc16(buf, (size_t)HML_DISC_CRC_OFF));
  if (emit("fuzz_ctrl", "beacon_non_utf8", "non_utf8", buf, n, m) != 0) return -1;
  n = build_beacon(buf, 0x0000ffffffffffffull);
  buf[21] = 0u;
  buf[22] = 0u;
  buf[23] = 0u;
  hml_put_be16(buf + HML_DISC_CRC_OFF, hml_crc16(buf, (size_t)HML_DISC_CRC_OFF));
  if (emit("fuzz_ctrl", "beacon_embedded_nul", "embedded_nul", buf, n, m) != 0) return -1;
  n = build_beacon(buf, 0x0000ffffffffffffull);
  hml_put_be32(buf + 20, 0xffffffffu);
  if (emit("fuzz_ctrl", "beacon_ts_overflow", "v_ffffffff", buf, n, m) != 0) return -1;
  return 0;
}

/* ---- control device corpus ----------------------------------------------- */

static int corpus_ctrl_device(manifest *m) {
  uint8_t buf[HML_FUZZ_CTRL_MAX_MSG + 64u];
  uint8_t pl[HML_CTRL_MAX_PAYLOAD];
  if (emit_dir("fuzz_ctrl_device") != 0) return -1;
  memset(pl, 0x5au, sizeof(pl));

  size_t n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 4u);
  if (emit("fuzz_ctrl_device", "ping", "valid", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_NOP, HML_CTRL_FLAG_REQ, NULL, 0u);
  if (emit("fuzz_ctrl_device", "nop", "valid zero_payload", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_START, HML_CTRL_FLAG_REQ, NULL, 0u);
  if (emit("fuzz_ctrl_device", "start", "valid", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_STOP, HML_CTRL_FLAG_REQ, NULL, 0u);
  if (emit("fuzz_ctrl_device", "stop", "valid", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_RESET, HML_CTRL_FLAG_REQ, NULL, 0u);
  if (emit("fuzz_ctrl_device", "reset", "valid", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_GET_CAPS, HML_CTRL_FLAG_REQ, NULL, 0u);
  if (emit("fuzz_ctrl_device", "get_caps", "valid", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_STATUS, HML_CTRL_FLAG_REQ, NULL, 0u);
  if (emit("fuzz_ctrl_device", "status", "valid", buf, n, m) != 0) return -1;

  /* Every op code the dispatcher does not know, which must produce an error
   * response rather than a crash or a silent drop. */
  for (unsigned op = 0x70u; op < 0x80u; op++) {
    char name[32];
    n = build_ctrl(buf, (uint8_t)op, HML_CTRL_FLAG_REQ, pl, 4u);
    (void)snprintf(name, sizeof(name), "unknown_op_%02x", op);
    if (emit("fuzz_ctrl_device", name, "valid", buf, n, m) != 0) return -1;
  }
  n = build_ctrl(buf, 0xffu, HML_CTRL_FLAG_REQ, pl, (uint16_t)HML_CTRL_MAX_PAYLOAD);
  if (emit("fuzz_ctrl_device", "unknown_op_max_payload", "valid max_payload", buf, n, m) != 0)
    return -1;

  {
    uint8_t cfg[HML_CFG_SIZE];
    build_cfg(cfg, (uint8_t)HML_XPORT_UDP, 9000u);
    n = build_ctrl(buf, HML_OP_SET_DEST, HML_CTRL_FLAG_REQ, cfg, (uint16_t)HML_CFG_SIZE);
    if (emit("fuzz_ctrl_device", "set_dest", "valid", buf, n, m) != 0) return -1;

    build_cfg(cfg, (uint8_t)HML_XPORT_ROCEV2, 9000u);
    n = build_ctrl(buf, HML_OP_SET_DEST, HML_CTRL_FLAG_REQ, cfg, (uint16_t)HML_CFG_SIZE);
    if (emit("fuzz_ctrl_device", "set_dest_rocev2", "valid", buf, n, m) != 0) return -1;

    build_cfg(cfg, (uint8_t)HML_XPORT_UDP, 9000u);
    cfg[HML_OFF_CFG_KIND] = 9u; /* not a transport kind */
    n = build_ctrl(buf, HML_OP_SET_DEST, HML_CTRL_FLAG_REQ, cfg, (uint16_t)HML_CFG_SIZE);
    if (emit("fuzz_ctrl_device", "set_dest_bad_kind", "bad_cfg_kind", buf, n, m) != 0) return -1;

    build_cfg(cfg, (uint8_t)HML_XPORT_UDP, 0xffffffffu);
    n = build_ctrl(buf, HML_OP_SET_DEST, HML_CTRL_FLAG_REQ, cfg, (uint16_t)HML_CFG_SIZE);
    if (emit("fuzz_ctrl_device", "set_dest_frame_ffffffff", "v_ffffffff max_len", buf, n, m) != 0)
      return -1;

    /* One byte short of HML_CFG_SIZE: structurally a valid control message, so the
     * dispatcher is the one that has to notice the payload is the wrong size. */
    build_cfg(cfg, (uint8_t)HML_XPORT_UDP, 9000u);
    n = build_ctrl(buf, HML_OP_SET_DEST, HML_CTRL_FLAG_REQ, cfg, HML_CFG_SIZE - 1u);
    if (emit("fuzz_ctrl_device", "set_dest_short", "valid", buf, n, m) != 0) return -1;
  }
  {
    uint8_t poke[8];
    hml_put_be32(poke + 0, 0xffffffffu);
    hml_put_be32(poke + 4, 0x7fffffffu);
    n = build_ctrl(buf, HML_OP_POKE, HML_CTRL_FLAG_REQ, poke, 8u);
    if (emit("fuzz_ctrl_device", "poke_absurd", "v_ffffffff v_7fffffff", buf, n, m) != 0)
      return -1;
    n = build_ctrl(buf, HML_OP_POKE, HML_CTRL_FLAG_REQ, poke, 4u);
    if (emit("fuzz_ctrl_device", "poke_short", "valid", buf, n, m) != 0) return -1;
    n = build_ctrl(buf, HML_OP_POKE, HML_CTRL_FLAG_REQ, poke, 0u);
    if (emit("fuzz_ctrl_device", "poke_empty", "valid zero_payload", buf, n, m) != 0) return -1;
    n = build_ctrl(buf, HML_OP_READBACK, HML_CTRL_FLAG_REQ, poke, 4u);
    if (emit("fuzz_ctrl_device", "readback_absurd", "v_ffffffff", buf, n, m) != 0) return -1;
  }

  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 4u);
  buf[HML_OFF_DISC_MAGIC] ^= 0xffu;
  if (emit("fuzz_ctrl_device", "bad_magic", "bad_magic", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 4u);
  buf[HML_OFF_CTRL_VERSION] = 2u;
  if (emit("fuzz_ctrl_device", "bad_version", "bad_version", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 4u);
  buf[n - 1u] ^= 0xffu;
  if (emit("fuzz_ctrl_device", "bad_crc", "bad_icrc", buf, n, m) != 0) return -1;
  n = build_ctrl(buf, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 4u);
  if (emit("fuzz_ctrl_device", "truncated", "truncated", buf, n - 3u, m) != 0) return -1;
  if (emit("fuzz_ctrl_device", "empty", "truncated", buf, 0u, m) != 0) return -1;
  {
    uint8_t nul[HML_FUZZ_CTRL_MAX_MSG];
    memset(nul, 0, sizeof(nul));
    nul[HML_OFF_CTRL_RSVD] = 1u;
    if (emit("fuzz_ctrl_device", "bad_reserved", "bad_reserved", nul, 16u, m) != 0) return -1;
  }
  {
    uint8_t u[HML_FUZZ_CTRL_MAX_MSG];
    n = build_ctrl(u, HML_OP_PING, HML_CTRL_FLAG_REQ, pl, 12u);
    memcpy(u + 12, kNotUtf8, 12u);
    hml_put_be16(u + 24, hml_crc16(u, 24u));
    if (emit("fuzz_ctrl_device", "valid_non_utf8", "valid non_utf8", u, n, m) != 0) return -1;
  }
  return 0;
}

/* ---- RoCEv2 demux corpus ------------------------------------------------- */

#define RX_BASE 0x100000ull
#define RX_FRAME 9000u
#define RX_RKEY 0xdeadbeefu
#define RX_WIN 65536ull

#define ROCE_IP_OFF 20u
#define ROCE_UDP_OFF (ROCE_IP_OFF + 8u)
#define ROCE_BTH_OFF ROCE_UDP_OFF
#define ROCE_RETH_OFF (ROCE_BTH_OFF + 12u)
#define ROCE_PAY_OFF (ROCE_RETH_OFF + 16u)
#define ROCE_ICRC_OFF 4u
#define ROCE_MIN (ROCE_PAY_OFF + ROCE_ICRC_OFF)

/* The ICRC covers BTH..payload, so every RETH/BTH mutation below has to recompute it
 * through roce_set_icrc(). Without that, a case that sets a bad rkey or a wild VA is
 * really only exercising the ICRC check, and the field it is named for is never
 * reached. */
static void roce_set_icrc(uint8_t *out, unsigned ipopt, size_t wlen, bool good);

/* IP | UDP | BTH | RETH | write data | ICRC.
 *
 * `ipopt` bytes of IPv4 options are inserted between the UDP header and the BTH.
 * The ICRC does not cover them, so the checksum is unaffected. The UDP destination
 * port is left at 0x1000: both the harness and the demux rewrite it to the live one. */
static size_t build_roce(uint8_t *out, size_t cap, unsigned ipopt, uint64_t va, uint32_t rkey,
                         size_t wlen, uint32_t psn, bool good_icrc, const uint8_t *fill) {
  const size_t fixed = ROCE_PAY_OFF + ipopt + ROCE_ICRC_OFF;
  if (cap < fixed) return 0u;
  if (wlen > cap - fixed) wlen = cap - fixed;
  const size_t total = fixed + wlen;
  memset(out, 0, total);

  const unsigned ihl = 5u + (ipopt / 4u);
  out[0] = (uint8_t)(0x40u | ihl);
  hml_put_be16(out + 2, (uint16_t)total);
  out[8] = 64u;
  out[9] = 17u;
  for (unsigned i = 0; i < ipopt; i++) out[20u + i] = 0x01u; /* NOP options */

  /* The UDP header follows the IP options, so it moves with ipopt. Getting this
   * wrong is invisible until the demux compares the destination port against the
   * live one, which then fails for a reason that has nothing to do with options. */
  uint8_t *u = out + ROCE_IP_OFF + ipopt;
  hml_put_be16(u + 0, 0x1000u);
  hml_put_be16(u + 2, 0u);
  /* The UDP length counts from the UDP header, which sits *after* the IP options,
   * so ipopt is not part of it. The IP total length above is what carries ipopt. */
  hml_put_be16(u + 4, (uint16_t)(8u + 12u + 16u + wlen + ROCE_ICRC_OFF));
  hml_put_be16(u + 6, 0u);

  uint8_t *b = out + ROCE_BTH_OFF + ipopt;
  b[0] = 0x2au;
  hml_put_be32(b + 3, psn & 0x00ffffffu);
  hml_put_be32(b + 8, 0u);

  uint8_t *r = b + 12u;
  hml_put_be64(r + 0, va);
  hml_put_be32(r + 8, rkey);
  hml_put_be32(r + 12, (uint32_t)wlen);

  uint8_t *p = r + 16u;
  for (size_t i = 0; i < wlen; i++) p[i] = (fill != NULL) ? fill[i % 16u] : (uint8_t)i;

  roce_set_icrc(out, ipopt, wlen, good_icrc);
  return total;
}

/* The ICRC covers BTH..payload, so every RETH/BTH mutation has to recompute it.
 * Without this, a case that sets a bad rkey or a wild VA is really only testing the
 * ICRC check, and the field it is named for is never reached. */
static void roce_set_icrc(uint8_t *out, unsigned ipopt, size_t wlen, bool good) {
  uint8_t *bth = out + ROCE_BTH_OFF + ipopt;
  const size_t cov = 12u + 16u + wlen;
  uint32_t c = hml_crc32_ieee(bth, cov);
  if (!good) c ^= 0x00000001u;
  uint8_t *p = bth + 12u + 16u + wlen;
  p[0] = (uint8_t)c;
  p[1] = (uint8_t)(c >> 8);
  p[2] = (uint8_t)(c >> 16);
  p[3] = (uint8_t)(c >> 24);
}

static int corpus_roce(manifest *m) {
  uint8_t buf[4096];
  uint8_t fill[16];
  if (emit_dir("fuzz_roce_rx") != 0) return -1;
  for (unsigned i = 0; i < 16u; i++) fill[i] = (uint8_t)(0xffu - i * 8u);

  size_t n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
  if (emit("fuzz_roce_rx", "valid_write", "valid max_payload non_utf8", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 0u, 1u, true, fill);
  if (emit("fuzz_roce_rx", "valid_zero_write", "valid zero_payload", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE + RX_WIN - 1400u, RX_RKEY, 1400u, 2u, true, fill);
  if (emit("fuzz_roce_rx", "window_end_write", "valid window_end", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE + RX_FRAME, RX_RKEY, 20u, 3u, true, fill);
  if (emit("fuzz_roce_rx", "meta_slot", "valid", buf, n, m) != 0) return -1;
  /* The largest write that still fits the receiver's datagram, so the boundary
   * between "write" and "protocol error" is exercised on both sides. */
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1988u, 4u, true, fill);
  if (emit("fuzz_roce_rx", "valid_max_write", "valid", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1989u, 5u, true, fill);
  if (emit("fuzz_roce_rx", "over_max_write", "wlen_over_max", buf, n, m) != 0) return -1;

  /* A PSN far past the expected one: accepted as a write, counted as out of order
   * rather than rejected. The demux is documented to do exactly this. */
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 0x00fffffeu, true, fill);
  if (emit("fuzz_roce_rx", "psn_jump", "valid", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 0x00ffffffu, true, fill);
  if (emit("fuzz_roce_rx", "psn_max", "v_ffffff00", buf, n, m) != 0) return -1;

  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, false, fill);
  if (emit("fuzz_roce_rx", "bad_icrc", "bad_icrc", buf, n, m) != 0) return -1;
  /* rkey 0 is the demux's "do not check" value, so this is the one wrong-key case
   * that is still accepted. Tagged separately: calling it bad_rkey would be a lie. */
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, 0x00000000u, 1400u, 1u, true, fill);
  if (emit("fuzz_roce_rx", "rkey_zero_disables_check", "valid", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, 0x00000001u, 1400u, 1u, true, fill);
  if (emit("fuzz_roce_rx", "bad_rkey", "bad_rkey", buf, n, m) != 0) return -1;

  n = build_roce(buf, sizeof(buf), 0u, 0xdeadbeefdeadbeefull, RX_RKEY, 1400u, 1u, true, fill);
  if (emit("fuzz_roce_rx", "va_max48", "oob_va", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, 0ull, RX_RKEY, 1400u, 1u, true, fill);
  if (emit("fuzz_roce_rx", "va_zero", "oob_va", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE - 1u, RX_RKEY, 1400u, 1u, true, fill);
  if (emit("fuzz_roce_rx", "va_below_window", "oob_va", buf, n, m) != 0) return -1;
  /* VA at the very end of the window, with a write that does not fit: the demux's
   * bound is on the end of the write, not on the start, so this is out of range even
   * though the address itself is not past window_len. */
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE + RX_WIN, RX_RKEY, 1400u, 1u, true, fill);
  if (emit("fuzz_roce_rx", "va_past_window_end", "oob_va", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE + RX_WIN + 1u, RX_RKEY, 20u, 1u, true, fill);
  if (emit("fuzz_roce_rx", "va_above_window", "oob_va", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
  memset(buf + ROCE_RETH_OFF, 0xff, 8u); /* VA = SIZE_MAX */
  roce_set_icrc(buf, 0u, 1400u, true);
  if (emit("fuzz_roce_rx", "va_sizemax", "oob_va v_sizemax", buf, n, m) != 0) return -1;

  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
  buf[0] = 0x55u; /* version 5 */
  if (emit("fuzz_roce_rx", "bad_version", "bad_version", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
  buf[9] = 6u; /* TCP */
  if (emit("fuzz_roce_rx", "bad_proto", "bad_proto", buf, n, m) != 0) return -1;
  /* The opcode byte is inside ICRC coverage, so each of these recomputes it. */
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
  buf[ROCE_BTH_OFF] = 0x0au; /* RC RDMA Write, also accepted */
  roce_set_icrc(buf, 0u, 1400u, true);
  if (emit("fuzz_roce_rx", "rc_opcode", "valid", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
  buf[ROCE_BTH_OFF] = 0x00u; /* neither UC nor RC */
  roce_set_icrc(buf, 0u, 1400u, true);
  if (emit("fuzz_roce_rx", "bad_opcode", "bad_opcode", buf, n, m) != 0) return -1;

  /* 40 bytes of IPv4 options. The ICRC is unaffected, so this is a valid write that
   * the demux has to reach by honouring IHL rather than assuming a 20-byte header. */
  n = build_roce(buf, sizeof(buf), 40u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
  if (emit("fuzz_roce_rx", "ip_options", "valid ihl_options", buf, n, m) != 0) return -1;

  /* RETH length. The demux masks the top byte (HML_RX_PSN_MASK) and then requires
   * the result to equal the bytes actually present, so every one of these is a
   * protocol error -- and each has to recompute the ICRC or it would just be
   * testing the ICRC. */
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
  hml_put_be32(buf + ROCE_RETH_OFF + 12u, 0xffffffffu);
  roce_set_icrc(buf, 0u, 1400u, true);
  if (emit("fuzz_roce_rx", "reth_len_ffffffff", "v_ffffffff max_len", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
  hml_put_be32(buf + ROCE_RETH_OFF + 12u, 0x7fffffffu);
  roce_set_icrc(buf, 0u, 1400u, true);
  if (emit("fuzz_roce_rx", "reth_len_7fffffff", "v_7fffffff max_len", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
  hml_put_be32(buf + ROCE_RETH_OFF + 12u, 0x00ffffffu);
  roce_set_icrc(buf, 0u, 1400u, true);
  if (emit("fuzz_roce_rx", "reth_len_ffffff00", "v_ffffff00 max_len", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
  hml_put_be32(buf + ROCE_RETH_OFF + 12u, 0x0000ffffu);
  roce_set_icrc(buf, 0u, 1400u, true);
  if (emit("fuzz_roce_rx", "reth_len_0xffff", "v_0xffff", buf, n, m) != 0) return -1;

  /* An IP total length that disagrees with the bytes actually present, in both
   * directions. The demux derives the payload length from the UDP header, not from
   * the IP one, so these are the cases where a future reader that trusts the IP
   * length would go wrong -- and where the UDP length and the datagram have to stay
   * in agreement for the case to be about the IP field at all. */
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
  hml_put_be16(buf + 2, (uint16_t)(n - 100u));
  roce_set_icrc(buf, 0u, 1400u, true);
  if (emit("fuzz_roce_rx", "ip_len_short", "ip_len_short", buf, n, m) != 0) return -1;
  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
  hml_put_be16(buf + 2, (uint16_t)(n + 100u));
  roce_set_icrc(buf, 0u, 1400u, true);
  if (emit("fuzz_roce_rx", "ip_len_long", "ip_len_long", buf, n, m) != 0) return -1;

  n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
  for (unsigned k = 0; k < 24u; k++) {
    char name[32];
    (void)snprintf(name, sizeof(name), "trunc_%u", k);
    if (emit("fuzz_roce_rx", name, "truncated", buf, (size_t)k * (n / 24u), m) != 0) return -1;
  }
  {
    uint8_t nul[2048];
    memset(nul, 0, sizeof(nul));
    n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
    memcpy(nul, buf, ROCE_PAY_OFF);
    if (emit("fuzz_roce_rx", "embedded_nul", "embedded_nul", nul, 1400u, m) != 0) return -1;
  }
  {
    uint8_t u[2048];
    n = build_roce(buf, sizeof(buf), 0u, RX_BASE, RX_RKEY, 1400u, 1u, true, fill);
    memcpy(u, buf, n);
    for (size_t k = ROCE_PAY_OFF; k < ROCE_PAY_OFF + 1400u; k += 7u) u[k] = 0xffu;
    if (emit("fuzz_roce_rx", "non_utf8", "non_utf8", u, n, m) != 0) return -1;
  }
  return 0;
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
    return 2;
  }
  int k = snprintf(g_root, sizeof(g_root), "%s", argv[1]);
  if (k < 0 || (size_t)k >= sizeof(g_root)) {
    fprintf(stderr, "gen_corpus: output dir path too long\n");
    return 2;
  }
  if (hml_fuzz_mkdir_p(g_root) != 0) {
    fprintf(stderr, "gen_corpus: cannot create %s\n", g_root);
    return 2;
  }

  char mpath[600];
  k = snprintf(mpath, sizeof(mpath), "%s/manifest.txt", g_root);
  if (k < 0 || (size_t)k >= sizeof(mpath)) return 2;
  manifest m;
  m.f = fopen(mpath, "wb");
  m.count = 0u;
  if (m.f == NULL) {
    fprintf(stderr, "gen_corpus: cannot write %s\n", mpath);
    return 2;
  }

  int rc = 0;
  if (corpus_frame(&m) != 0) rc = 1;
  if (rc == 0 && corpus_reasm(&m) != 0) rc = 1;
  if (rc == 0 && corpus_cfg(&m) != 0) rc = 1;
  if (rc == 0 && corpus_ctrl(&m) != 0) rc = 1;
  if (rc == 0 && corpus_beacon(&m) != 0) rc = 1;
  if (rc == 0 && corpus_ctrl_device(&m) != 0) rc = 1;
  if (rc == 0 && corpus_roce(&m) != 0) rc = 1;

  (void)fclose(m.f);
  if (rc == 0) printf("gen_corpus: %u seed inputs in %s\n", m.count, g_root);
  return rc;
}
