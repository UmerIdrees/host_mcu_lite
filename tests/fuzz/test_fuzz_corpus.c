/*
 * test_fuzz_corpus -- the seed corpus is a contract, so it is checked as one.
 *
 * The corpus is generated (tests/fuzz/gen_corpus.c) rather than committed, which
 * means it can silently lose a case, or a case can quietly stop being the thing
 * its name claims. This test is the regression net for that.
 *
 * It does NOT rebuild the cases itself. A second copy of the generators would be a
 * second thing to keep in sync, and a drift between the two would make the test
 * pass while the corpus rotted -- exactly what it exists to catch. Instead it runs
 * the real generator into a scratch directory, reads back <dir>/manifest.txt, and
 * asserts against the bytes that are actually on disk.
 *
 * Three kinds of check:
 *
 *   1. Every required case class appears at least once. A class that stops being
 *      generated is a coverage regression that no single case would reveal.
 *   2. Every tag on every row is true of that row's bytes. gen_corpus is the only
 *      thing allowed to assert a shape, and this is what holds it to it.
 *   3. Every generated input runs through its own harness and exits clean.
 *
 * The tag vocabulary is documented at the top of gen_corpus.c; this file is the
 * other half of that contract.
 *
 *   test_fuzz_corpus <gen_corpus-path> [harness-dir]
 *
 * harness-dir defaults to ".", the directory the harness binaries are built in, so
 * the byte-contract checks still run (and are still worth running) where the
 * binaries are not.
 */

#include "hml_fuzz.h"

#include "hml/hml_crc.h"
#include "hml/hml_ctrl.h"
#include "hml/hml_discovery.h"
#include "hml/hml_protocol.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* The receiver window the RoCEv2 corpus cases are built against. */
#define CORPUS_RX_BASE 0x100000ull
#define CORPUS_RX_WIN 65536ull

static unsigned g_failed;
static unsigned g_checked;

static void fail(const char *label, const char *fmt, ...) {
  va_list ap;
  fprintf(stderr, "FAIL %s: ", label);
  va_start(ap, fmt);
  (void)vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  g_failed++;
}

#define CHECK(cond, label, ...)                \
  do {                                         \
    g_checked++;                               \
    if (!(cond)) fail((label), __VA_ARGS__);  \
  } while (0)

/* ---- tags ---------------------------------------------------------------- */

static bool has_tag(const char *tags, const char *tag) {
  size_t n = strlen(tag);
  for (const char *p = tags; (p = strstr(p, tag)) != NULL; p += n) {
    bool left = (p == tags) || p[-1] == ' ';
    bool right = p[n] == '\0' || p[n] == ' ';
    if (left && right) return true;
  }
  return false;
}

static bool has_any_tag(const char *tags, const char *const *names, unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    if (has_tag(tags, names[i])) return true;
  }
  return false;
}

/* ---- byte predicates ----------------------------------------------------- */

static bool contains_u16(const uint8_t *d, size_t n, uint16_t v) {
  for (size_t i = 0; i + 2u <= n; i++) {
    if (hml_get_be16(d + i) == v) return true;
  }
  return false;
}

static bool contains_u32(const uint8_t *d, size_t n, uint32_t v) {
  for (size_t i = 0; i + 4u <= n; i++) {
    if (hml_get_be32(d + i) == v) return true;
  }
  return false;
}

static bool contains_u64(const uint8_t *d, size_t n, uint64_t v) {
  for (size_t i = 0; i + 8u <= n; i++) {
    if (hml_get_be64(d + i) == v) return true;
  }
  return false;
}

static bool high_byte_between(const uint8_t *d, size_t n, size_t from, size_t to) {
  if (from >= n) return false;
  if (to > n) to = n;
  for (size_t i = from; i < to; i++) {
    if (d[i] >= 0x80u) return true;
  }
  return false;
}

static bool nul_after_zero(const uint8_t *d, size_t n) {
  for (size_t i = 1; i < n; i++) {
    if (d[i] == 0x00u) return true;
  }
  return false;
}

/* ---- what the library's own parsers say ---------------------------------- */

/* One frame datagram occupying all of [d, d+n): magic, version, header CRC, a
 * payload_len that matches the bytes, and a payload CRC over exactly the declared
 * payload. */
static bool frame_ok(const uint8_t *d, size_t n) {
  if (n < (size_t)HML_FRAME_HDR_SIZE + HML_TRAIL_CRC_SIZE) return false;
  if (hml_get_be32(d) != HML_MAGIC_FRAME) return false;
  if (d[HML_OFF_VERSION] != (uint8_t)HML_PROTO_VERSION) return false;
  uint16_t plen = hml_get_be16(d + HML_OFF_PAYLOAD_LEN);
  if (n != (size_t)HML_FRAME_HDR_SIZE + plen + HML_TRAIL_CRC_SIZE) return false;
  if (hml_get_be16(d + HML_OFF_HDR_CRC) != hml_crc16(d, (size_t)HML_OFF_HDR_CRC)) return false;
  return hml_get_be16(d + HML_OFF_PAYLOAD + plen) == hml_crc16(d + HML_OFF_PAYLOAD, plen);
}

/* The same checks, but for a frame that starts at d with n bytes still available.
 * A reassembly input is several datagrams back to back, and only the last one ends
 * at the end of the buffer, so requiring 28 + payload_len + 2 == n here would call
 * every non-final frame truncated. Returns the frame's total size, or 0. */
static size_t frame_at(const uint8_t *d, size_t n) {
  if (n < (size_t)HML_FRAME_HDR_SIZE + HML_TRAIL_CRC_SIZE) return 0u;
  if (hml_get_be32(d) != HML_MAGIC_FRAME) return 0u;
  if (d[HML_OFF_VERSION] != (uint8_t)HML_PROTO_VERSION) return 0u;
  uint16_t plen = hml_get_be16(d + HML_OFF_PAYLOAD_LEN);
  size_t total = (size_t)HML_FRAME_HDR_SIZE + plen + HML_TRAIL_CRC_SIZE;
  if (total > n) return 0u;
  if (hml_get_be16(d + HML_OFF_HDR_CRC) != hml_crc16(d, (size_t)HML_OFF_HDR_CRC)) return 0u;
  if (hml_get_be16(d + HML_OFF_PAYLOAD + plen) != hml_crc16(d + HML_OFF_PAYLOAD, plen)) return 0u;
  return total;
}

/* A concatenation of frame datagrams, each complete on its own: what a fuzz_reasm
 * input actually is. "valid" means every embedded datagram parses, not that the
 * whole buffer is a single frame. */
static bool frames_ok(const uint8_t *d, size_t n) {
  size_t off = 0u;
  unsigned count = 0u;
  while (off < n) {
    size_t total = frame_at(d + off, n - off);
    if (total == 0u) return false;
    off += total;
    if (++count > 4096u) return false; /* a "valid" case that never terminates */
  }
  return count > 0u;
}

static bool ctrl_ok(const uint8_t *d, size_t n) {
  hml_ctrl_msg m;
  const uint8_t *p = NULL;
  return hml_ctrl_parse(d, n, &m, &p) == HML_OK;
}

static bool beacon_ok(const uint8_t *d, size_t n) {
  hml_beacon b;
  return hml_beacon_parse(d, n, &b) == HML_OK;
}

static bool cfg_ok(const uint8_t *d, size_t n) {
  hml_dest_cfg out;
  return n == (size_t)HML_CFG_SIZE && hml_proto_cfg_parse(d, n, &out) == HML_OK;
}

/* RoCEv2. The accept path needs the harness to rewrite the UDP destination port to
 * the live one, so what can be checked here is everything the demux validates before
 * that: the IPv4 version and IHL, the protocol number, the BTH opcode, and the ICRC
 * over BTH..ICRC. The ICRC is little-endian on the wire, unlike everything else. */
#define ROCE_BTH_OFF 28u
#define ROCE_RETH_OFF (ROCE_BTH_OFF + 12u)
#define ROCE_PAY_OFF (ROCE_RETH_OFF + 16u)
#define ROCE_ICRC (4u)

static bool roce_shape_ok(const uint8_t *d, size_t n) {
  if (n < ROCE_PAY_OFF + ROCE_ICRC) return false;
  if ((d[0] >> 4) != 4u) return false;
  size_t ihl = (size_t)(d[0] & 0x0fu) * 4u;
  if (ihl < 20u || ihl > 60u) return false;
  if (d[9] != 17u) return false;
  if (n < ihl + 8u + 12u + 16u + ROCE_ICRC) return false;
  if (d[ihl + 8u] != 0x2au && d[ihl + 8u] != 0x0au) return false;

  /* The IP total length and the UDP length both have to agree with the datagram, or
   * "the payload length" is whatever the reader believes. */
  size_t iplen = hml_get_be16(d + 2);
  if (iplen > n) return false;
  size_t udplen = hml_get_be16(d + ihl + 4u);
  if (udplen < 8u) return false;
  size_t udp_total = ihl + 8u + (udplen - 8u);
  if (udp_total > n) return false;

  size_t paylen = udplen - 8u;
  if (paylen < 12u + 16u + ROCE_ICRC) return false;
  size_t cov = paylen - ROCE_ICRC;
  const uint8_t *bth = d + ihl + 8u;
  uint32_t got = (uint32_t)bth[cov + 0u] | ((uint32_t)bth[cov + 1u] << 8) |
                 ((uint32_t)bth[cov + 2u] << 16) | ((uint32_t)bth[cov + 3u] << 24);
  return got == hml_crc32_ieee(bth, cov);
}

/* ---- the per-row contract ------------------------------------------------ */

static void check_row(const char *harness, const char *file, const char *tags, const uint8_t *d,
                      size_t n) {
  char label[1200];
  (void)snprintf(label, sizeof(label), "%s/%s", harness, file);

  const bool is_frame = strcmp(harness, "fuzz_frame") == 0;
  const bool is_reasm = strcmp(harness, "fuzz_reasm") == 0;
  const bool is_any_frame = is_frame || is_reasm;
  const bool is_ctrl = strcmp(harness, "fuzz_ctrl") == 0;
  const bool is_dev = strcmp(harness, "fuzz_ctrl_device") == 0;
  const bool is_cfg = strcmp(harness, "fuzz_cfg") == 0;
  const bool is_roce = strcmp(harness, "fuzz_roce_rx") == 0;
  const bool known = is_any_frame || is_ctrl || is_dev || is_cfg || is_roce;
  if (!known) {
    fail(label, "manifest names a harness no contract is written for: %s", harness);
    return;
  }

  /* A registry-name probe is a bare 32-byte name, not a record. */
  const bool is_name = is_cfg && n == 32u;

  if (has_tag(tags, "valid")) {
    bool ok;
    if (is_any_frame) ok = frames_ok(d, n);
    else if (is_ctrl) ok = ctrl_ok(d, n) || beacon_ok(d, n);
    else if (is_dev) ok = ctrl_ok(d, n);
    else if (is_cfg) ok = cfg_ok(d, n) || is_name;
    else ok = roce_shape_ok(d, n);
    CHECK(ok, label, "tagged valid but the library's own parser rejects these bytes");
  }

  if (has_tag(tags, "truncated")) {
    bool incomplete;
    if (is_any_frame) incomplete = !frames_ok(d, n);
    else if (is_ctrl) incomplete = !ctrl_ok(d, n) && !beacon_ok(d, n);
    else if (is_dev) incomplete = !ctrl_ok(d, n);
    else if (is_cfg) incomplete = !cfg_ok(d, n);
    else incomplete = !roce_shape_ok(d, n);
    CHECK(incomplete, label, "tagged truncated but the record on disk is complete");
  }

  if (has_tag(tags, "trailing_junk")) {
    /* A complete record plus extra bytes. The parsers are length-driven and have to
     * ignore the tail rather than trust it. */
    bool ok;
    if (is_frame) {
      ok = frame_ok(d, n - 1u) && !frame_ok(d, n);
    } else if (is_ctrl || is_dev) {
      ok = ctrl_ok(d, n - 1u) && !ctrl_ok(d, n);
    } else {
      ok = false;
    }
    CHECK(ok, label, "tagged trailing_junk but dropping the last byte does not change the verdict");
  }

  if (has_tag(tags, "bad_magic")) {
    bool wrong;
    if (is_any_frame) wrong = hml_get_be32(d) != HML_MAGIC_FRAME;
    else if (is_ctrl) wrong = hml_get_be32(d) != HML_MAGIC_CTRL && hml_get_be32(d) != HML_MAGIC_BEACON;
    else if (is_dev) wrong = hml_get_be32(d) != HML_MAGIC_CTRL;
    else if (is_cfg) wrong = d[HML_OFF_CFG_KIND] != (uint8_t)HML_XPORT_UDP &&
                             d[HML_OFF_CFG_KIND] != (uint8_t)HML_XPORT_ROCEV2;
    else wrong = d[ROCE_BTH_OFF] != 0x2au && d[ROCE_BTH_OFF] != 0x0au;
    CHECK(wrong, label, "tagged bad_magic but the magic/opcode is still the good one");
  }

  if (has_tag(tags, "bad_version")) {
    bool wrong;
    if (is_any_frame) wrong = d[HML_OFF_VERSION] != (uint8_t)HML_PROTO_VERSION;
    else if (is_ctrl || is_dev) wrong = d[HML_OFF_CTRL_VERSION] != (uint8_t)HML_CTRL_VERSION;
    else if (is_roce) wrong = (uint8_t)(d[0] >> 4) != 4u;
    else wrong = true;
    CHECK(wrong, label, "tagged bad_version but the version field is still valid");
  }

  if (has_tag(tags, "bad_opcode")) {
    CHECK(is_roce && n > ROCE_BTH_OFF && d[ROCE_BTH_OFF] != 0x2au && d[ROCE_BTH_OFF] != 0x0au, label,
          "tagged bad_opcode but the BTH opcode is one the demux accepts");
  }
  if (has_tag(tags, "bad_proto")) {
    CHECK(is_roce && n > 9u && d[9] != 17u, label,
          "tagged bad_proto but the IP protocol field is still UDP");
  }
  if (has_tag(tags, "bad_rkey")) {
    /* Not zero: rkey 0 is the demux's documented "do not check" value and is
     * accepted, so a case with rkey 0 is not a bad-key case at all. */
    CHECK(is_roce && n >= ROCE_RETH_OFF + 12u && hml_get_be32(d + ROCE_RETH_OFF + 8u) != 0u, label,
          "tagged bad_rkey but the RETH remote key is zero, which disables the check");
  }
  if (has_tag(tags, "bad_reserved")) {
    bool bad;
    if (is_cfg) {
      bad = d[HML_OFF_CFG_PAD0] != 0u || d[HML_OFF_CFG_PAD1] != 0u;
      for (unsigned i = 0; i < 4u; i++) bad = bad || d[HML_OFF_CFG_RSVD + i] != 0u;
    } else {
      bad = hml_get_be16(d + HML_OFF_CTRL_RSVD) != 0u;
    }
    CHECK(bad, label, "tagged bad_reserved but every reserved byte is zero");
  }
  if (has_tag(tags, "ihl_options")) {
    CHECK(is_roce && n > 0u && (uint8_t)(d[0] >> 4) == 4u && (d[0] & 0x0fu) != 5u, label,
          "tagged ihl_options but the IP header length is the plain 20 bytes");
  }

  if (has_tag(tags, "len_over_max")) {
    bool over;
    if (is_any_frame) over = hml_get_be16(d + HML_OFF_PAYLOAD_LEN) > (uint16_t)HML_MAX_FRAG_PAYLOAD;
    else over = hml_get_be16(d + HML_OFF_CTRL_LEN) > (uint16_t)HML_CTRL_MAX_PAYLOAD;
    CHECK(over, label, "tagged len_over_max but the declared length is within the maximum");
  }

  if (has_tag(tags, "non_utf8")) {
    size_t from = 0u, to = n;
    if (is_any_frame) from = (size_t)HML_OFF_PAYLOAD;
    else if (is_ctrl || is_dev) from = HML_CTRL_HDR_SIZE;
    else if (is_cfg) { from = HML_OFF_CFG_QP; to = HML_OFF_CFG_FRAME_SIZE; }
    else from = ROCE_PAY_OFF;
    CHECK(high_byte_between(d, n, from, to), label,
          "tagged non_utf8 but there is no byte >= 0x80 outside the length fields");
  }
  if (has_tag(tags, "non_utf8_header")) {
    CHECK(is_frame && n > 0u && high_byte_between(d, n, 0u, (size_t)HML_OFF_PAYLOAD), label,
          "tagged non_utf8_header but the header has no high byte");
  }
  if (has_tag(tags, "embedded_nul")) {
    CHECK(nul_after_zero(d, n), label, "tagged embedded_nul but there is no NUL after byte 0");
  }
  if (has_tag(tags, "max_payload")) {
    bool full;
    if (is_any_frame) {
      full = hml_get_be16(d + HML_OFF_PAYLOAD_LEN) == (uint16_t)HML_MAX_FRAG_PAYLOAD;
    } else if (is_ctrl || is_dev) {
      full = hml_get_be16(d + HML_OFF_CTRL_LEN) == (uint16_t)HML_CTRL_MAX_PAYLOAD;
    } else if (is_roce) {
      full = n >= 1400u;
    } else {
      full = false;
    }
    CHECK(full, label, "tagged max_payload but the payload is not at the maximum");
  }
  if (has_tag(tags, "zero_payload")) {
    bool zero;
    if (is_any_frame) zero = hml_get_be16(d + HML_OFF_PAYLOAD_LEN) == 0u;
    else if (is_ctrl || is_dev) zero = hml_get_be16(d + HML_OFF_CTRL_LEN) == 0u;
    else if (is_roce) zero = hml_get_be32(d + ROCE_RETH_OFF + 12u) == 0u;
    else zero = false;
    CHECK(zero, label, "tagged zero_payload but a payload is present");
  }
  if (has_tag(tags, "bad_icrc")) {
    bool broken;
    if (is_any_frame) broken = !frames_ok(d, n);
    else if (is_ctrl) broken = !ctrl_ok(d, n) && !beacon_ok(d, n);
    else if (is_dev) broken = !ctrl_ok(d, n);
    else if (is_roce) broken = !roce_shape_ok(d, n);
    else broken = true;
    CHECK(broken, label, "tagged bad_icrc but the record still verifies");
  }

  if (has_tag(tags, "oob_va")) {
    /* The demux bounds the END of the write, not the address: a VA at the last byte
     * of the window is out of range if the write runs past the edge. Rejecting an
     * address merely equal to window_base + window_len would be a weaker check than
     * the library's, and would let a real out-of-range case pass. */
    bool oob;
    if (is_roce && n >= ROCE_RETH_OFF + 16u) {
      uint64_t va = hml_get_be64(d + ROCE_RETH_OFF);
      uint64_t wlen = hml_get_be32(d + ROCE_RETH_OFF + 12u) & 0x00ffffffu;
      uint64_t end = CORPUS_RX_BASE + CORPUS_RX_WIN;
      oob = va < CORPUS_RX_BASE || va > end || wlen > end - va;
    } else {
      oob = false;
    }
    CHECK(oob, label, "tagged oob_va but the write fits inside the receiver window");
  }
  if (has_tag(tags, "window_end")) {
    bool at_end;
    if (is_roce && n >= ROCE_RETH_OFF + 8u) {
      uint64_t va = hml_get_be64(d + ROCE_RETH_OFF);
      at_end = va + 1400u == CORPUS_RX_BASE + CORPUS_RX_WIN;
    } else {
      at_end = false;
    }
    CHECK(at_end, label, "tagged window_end but the write does not end at the window edge");
  }

  /* Value tags: an exact big-endian value has to be present in the bytes. */
  if (has_tag(tags, "v_0xffff")) {
    CHECK(contains_u16(d, n, 0xffffu), label, "tagged v_0xffff but 0xFFFF is not in the bytes");
  }
  if (has_tag(tags, "v_0x2000")) {
    CHECK(contains_u16(d, n, 0x2000u), label, "tagged v_0x2000 but 0x2000 is not in the bytes");
  }
  if (has_tag(tags, "v_7fffffff")) {
    CHECK(contains_u32(d, n, 0x7fffffffu), label,
          "tagged v_7fffffff but 0x7FFFFFFF is not in the bytes");
  }
  if (has_tag(tags, "v_ffffff00")) {
    CHECK(contains_u32(d, n, 0x00ffffffu), label,
          "tagged v_ffffff00 but 0x00FFFFFF is not in the bytes");
  }
  if (has_tag(tags, "v_ffffffff")) {
    CHECK(contains_u32(d, n, 0xffffffffu), label,
          "tagged v_ffffffff but 0xFFFFFFFF is not in the bytes");
  }
  if (has_tag(tags, "v_sizemax")) {
    CHECK(contains_u64(d, n, 0xffffffffffffffffull), label,
          "tagged v_sizemax but 0xFFFFFFFFFFFFFFFF is not in the bytes");
  }
  if (has_tag(tags, "max_len")) {
    /* "an absurd declared length" is only meaningful next to the value that
     * produced it, so the two tags have to agree. */
    static const char *const kExtremes[] = {"v_ffffffff", "v_7fffffff", "v_ffffff00", "v_sizemax",
                                            "v_0xffff", "v_0x2000"};
    CHECK(has_any_tag(tags, kExtremes, sizeof(kExtremes) / sizeof(kExtremes[0])), label,
          "tagged max_len but no extremal length value is tagged alongside it");
  }

  if (has_tag(tags, "wlen_over_max")) {
    /* A RETH length past what the receiver's datagram buffer can carry, which is
     * the boundary the harness clamps its own sealed writes to. */
    CHECK(is_roce && n >= ROCE_RETH_OFF + 16u && hml_get_be32(d + ROCE_RETH_OFF + 12u) > 1988u,
          label, "tagged wlen_over_max but the RETH length fits the receive buffer");
  }
  if (has_tag(tags, "bad_cfg_kind")) {
    /* For a control harness, a SET_DEST whose payload config names no transport. The
     * message itself is well formed, so this is the dispatcher's business. */
    size_t at = HML_CTRL_HDR_SIZE;
    CHECK((is_ctrl || is_dev) && n >= at + 1u &&
              d[at] != (uint8_t)HML_XPORT_UDP && d[at] != (uint8_t)HML_XPORT_ROCEV2,
          label, "tagged bad_cfg_kind but the config names a real transport");
  }
  if (has_tag(tags, "no_nul")) {
    /* A registry name that uses all 32 bytes and so is not NUL terminated. */
    CHECK(is_cfg && n == 32u && nul_after_zero(d, n) == false, label,
          "tagged no_nul but there is a NUL after byte 0");
  }
  if (has_tag(tags, "ip_len_short") || has_tag(tags, "ip_len_long")) {
    bool bad;
    if (is_roce && n > 4u) {
      size_t iplen = hml_get_be16(d + 2);
      bad = (iplen < n) == has_tag(tags, "ip_len_short");
    } else {
      bad = false;
    }
    CHECK(bad, label, "the IP total length does not disagree with the datagram the tag claims");
  }

  /* Capacity tags: the first byte picks an entry in the arena table, and the tag
   * has to name that entry. The table lives in hml_fuzz.c so the harness and the
   * generator cannot pick different capacity for the same byte. */
  {
    static const char *const kCapTags[] = {"cap_zero",      "cap_one",
                                           "cap_hdr",       "cap_frag",
                                           "cap_max_frame", "cap_max_frame_1",
                                           "cap_7fffffff",  "cap_80000000",
                                           "cap_ffffffff",  "cap_sizemax"};
    static const unsigned kCapIndex[] = {
        HML_FUZZ_CAP_0,          HML_FUZZ_CAP_1,        HML_FUZZ_CAP_HDR,
        HML_FUZZ_CAP_FRAG,       HML_FUZZ_CAP_MAX_FRAME, HML_FUZZ_CAP_MAX_FRAME_1,
        HML_FUZZ_CAP_7FFFFFFF,   HML_FUZZ_CAP_80000000,  HML_FUZZ_CAP_FFFFFFFF,
        HML_FUZZ_CAP_SIZE_MAX};
    unsigned named = HML_FUZZ_REASM_NCAPS;
    for (unsigned i = 0; i < sizeof(kCapTags) / sizeof(kCapTags[0]); i++) {
      if (has_tag(tags, kCapTags[i])) named = kCapIndex[i];
    }
    /* Only the rows that claim a capacity are checked; a reassembly case with no
     * capacity tag is a normal fragment-sequence case, not a malformed one. */
    if (named < HML_FUZZ_REASM_NCAPS) {
      CHECK(is_reasm && n > 0u, label, "a capacity tag on a case that is not a reassembly input");
      if (is_reasm && n > 0u) {
        unsigned picked = (unsigned)(d[0] % HML_FUZZ_REASM_NCAPS);
        CHECK(named == picked, label, "capacity tag names index %u but byte 0 selects index %u",
              named, picked);
        (void)hml_fuzz_reasm_cap(named);
      }
    }
  }
}

/* ---- corpus generation + manifest walk ----------------------------------- */

static int run_program(char *const argv[]) {
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    (void)execv(argv[0], argv);
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 128 + WTERMSIG(status);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <gen_corpus-path> [harness-dir]\n", argv[0]);
    return 2;
  }
  const char *bin_dir = argc >= 3 ? argv[2] : ".";

  char dir[] = "/tmp/hml_fuzz_corpus_XXXXXX";
  if (mkdtemp(dir) == NULL) {
    fprintf(stderr, "test_fuzz_corpus: mkdtemp failed: %s\n", strerror(errno));
    return 1;
  }
  int rc = run_program((char *const[]){argv[1], dir, NULL});
  if (rc != 0) {
    fprintf(stderr, "test_fuzz_corpus: gen_corpus exited %d\n", rc);
    return 1;
  }

  char mpath[600];
  (void)snprintf(mpath, sizeof(mpath), "%s/manifest.txt", dir);
  FILE *mf = fopen(mpath, "rb");
  if (mf == NULL) {
    fprintf(stderr, "test_fuzz_corpus: cannot read %s: %s\n", mpath, strerror(errno));
    return 1;
  }

  static const char *const kRequired[] = {
      "valid",         "truncated",     "trailing_junk", "bad_magic",       "bad_version",
      "bad_opcode",    "bad_proto",     "bad_rkey",      "bad_reserved",    "ihl_options",
      "len_over_max",  "non_utf8",      "non_utf8_header", "embedded_nul",  "max_payload",
      "zero_payload",  "bad_icrc",      "oob_va",        "window_end",      "v_0xffff",
      "v_0x2000",      "v_7fffffff",    "v_ffffff00",    "v_ffffffff",     "v_sizemax",
      "max_len",       "cap_sizemax",   "cap_one"};
  enum { NREQUIRED = sizeof(kRequired) / sizeof(kRequired[0]) };
  unsigned seen[NREQUIRED];
  memset(seen, 0, sizeof(seen));

  char line[1024];
  unsigned rows = 0u, ran = 0u, bytes = 0u;
  static uint8_t buf[HML_FUZZ_MAX_INPUT];
  while (fgets(line, (int)sizeof(line), mf) != NULL) {
    char *nl = strchr(line, '\n');
    if (nl != NULL) *nl = '\0';
    if (line[0] == '\0') continue;

    /* harness \t file \t bytes \t tags */
    char *tab = strchr(line, '\t');
    if (tab == NULL) continue;
    *tab = '\0';
    char *harness = line;
    char *rest = tab + 1;
    tab = strchr(rest, '\t');
    if (tab == NULL) continue;
    *tab = '\0';
    char *file = rest;
    rest = tab + 1;
    tab = strchr(rest, '\t');
    if (tab == NULL) continue;
    *tab = '\0';
    char *sizestr = rest;
    char *tags = tab + 1;

    long want = strtol(sizestr, NULL, 10);
    char path[1100];
    (void)snprintf(path, sizeof(path), "%s/%s/%s", dir, harness, file);
    size_t n = 0u;
    if (!hml_fuzz_read_file(path, buf, sizeof(buf), &n)) {
      fail(file, "manifest lists it but %s cannot be read", path);
      continue;
    }
    if (want < 0 || (size_t)want != n) {
      fail(file, "manifest says %ld bytes, the file has %zu", want, n);
      continue;
    }
    rows++;
    bytes += (unsigned)n;

    for (unsigned k = 0; k < NREQUIRED; k++) {
      if (has_tag(tags, kRequired[k])) seen[k]++;
    }
    check_row(harness, file, tags, buf, n);

    char hpath[1100];
    (void)snprintf(hpath, sizeof(hpath), "%s/%s", bin_dir, harness);
    int hrc = run_program((char *const[]){hpath, path, NULL});
    if (hrc != 0) {
      fail(file, "harness %s exited %d on its own seed case", harness, hrc);
    }
    ran++;
  }
  (void)fclose(mf);

  for (unsigned k = 0; k < NREQUIRED; k++) {
    CHECK(seen[k] > 0u, "corpus", "required case class \"%s\" appears in no seed", kRequired[k]);
  }
  CHECK(rows > 0u, "corpus", "the manifest is empty");
  CHECK(ran == rows, "corpus", "not every manifest row was run through a harness");

  {
    char cmd[700];
    (void)snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "note: could not remove %s\n", dir);
  }

  printf("test_fuzz_corpus: rows=%u bytes=%u harnessed=%u checks=%u failed=%u\n", rows, bytes, ran,
         g_checked, g_failed);
  return g_failed == 0u ? 0 : 1;
}
