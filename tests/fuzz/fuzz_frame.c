/*
 * fuzz_frame -- hml_proto_frame_parse / hml_proto_frame_fill (hml_protocol.h).
 *
 * The T0 data-plane datagram parser is the first thing every untrusted UDP byte
 * touches, so this harness is deliberately blunt about it:
 *
 *   1. the raw input is parsed, and parsed again asking for the payload;
 *   2. every truncation prefix of the input is parsed, so every "the length
 *      field promises more than arrived" boundary is hit explicitly rather than
 *      by luck;
 *   3. the input is re-sealed into a structurally valid datagram and parsed
 *      again, which is the only way to reach the accept path without coverage
 *      feedback;
 *   4. a header round trip is checked: fill from the parsed header, re-parse,
 *      every field must come back identical.
 *
 * Invariants asserted (a violation is a FINDING, not a crash):
 *   - a sealed datagram always parses, and its payload pointer/length are
 *     exactly 28 + 2 bytes into the buffer and the requested length;
 *   - fill(parse(x)) round-trips field for field.
 * Everything else -- rejections, error codes -- is free to differ; the harness
 * only records that a status came back in range.
 */

#include "hml_fuzz.h"

#include "hml/hml_protocol.h"

#include <stdio.h>
#include <string.h>

static void run(const uint8_t *data, size_t n) {
  hml_frame_hdr h;
  const uint8_t *payload = NULL;

  (void)hml_proto_frame_parse(data, n, &h, NULL);
  (void)hml_proto_frame_parse(data, n, &h, &payload);

  /* Every prefix length: covers each truncation boundary, including n == 0. */
  size_t sweep = n < (size_t)HML_MAX_FRAG_SIZE + 8u ? n : (size_t)HML_MAX_FRAG_SIZE + 8u;
  for (size_t k = 0; k <= sweep; k++) {
    hml_frame_hdr pk;
    const uint8_t *pp = NULL;
    hml_status pst = hml_proto_frame_parse(data, k, &pk, &pp);
    if (pst != HML_OK) continue;
    if (pp == NULL) continue;
    /* The parser only hands back a payload when the whole datagram is there,
     * so payload + payload_len must stay inside the buffer. */
    if (pp < data || (size_t)(pp - data) + (size_t)pk.payload_len > k) {
      hml_fuzz_finding("accepted payload of %u bytes at offset %td escapes the %zu byte input",
                       (unsigned)pk.payload_len, pp - data, k);
    }
  }

  /* NULL / zero arguments must be rejected, not dereferenced. */
  if (hml_proto_frame_parse(NULL, n, &h, &payload) != HML_ERR_ARG)
    hml_fuzz_finding("hml_proto_frame_parse(NULL, ..) did not return HML_ERR_ARG");
  if (hml_proto_frame_parse(data, n, NULL, &payload) != HML_ERR_ARG)
    hml_fuzz_finding("hml_proto_frame_parse(.., NULL, ..) did not return HML_ERR_ARG");
  hml_proto_frame_fill(NULL, NULL);

  /* Sealed: the accept path. */
  uint8_t sealed[HML_FUZZ_MAX_SEAL];
  size_t m = hml_fuzz_seal_frame(data, n, sealed, sizeof(sealed));
  if (m == 0u) return;

  hml_frame_hdr sh;
  const uint8_t *sp = NULL;
  hml_status st = hml_proto_frame_parse(sealed, m, &sh, &sp);
  if (st != HML_OK) {
    hml_fuzz_finding("a sealed %zu byte datagram was rejected with status %d: the "
                     "accept path is unreachable",
                     m, (int)st);
  }
  if (sp != sealed + HML_OFF_PAYLOAD) {
    hml_fuzz_finding("payload pointer is %p, want %p", (const void *)sp,
                     (const void *)(sealed + HML_OFF_PAYLOAD));
  }
  if ((size_t)sh.payload_len + (size_t)HML_FRAME_HDR_SIZE + HML_TRAIL_CRC_SIZE != m) {
    hml_fuzz_finding("sealed length %zu does not match 28 + %u + 2", m,
                     (unsigned)sh.payload_len);
  }

  /* fill(parse(sealed)) must round trip. hml_proto_frame_fill only writes the
   * 28-byte header, so the payload and its CRC have to be carried over as well --
   * parse() checks the trailing CRC, and handing it a header with no body behind it
   * would fail for a reason that has nothing to do with the round trip. */
  uint8_t again[HML_FUZZ_MAX_SEAL];
  hml_proto_frame_fill(&sh, again);
  if (m > (size_t)HML_FRAME_HDR_SIZE) {
    memcpy(again + HML_OFF_PAYLOAD, sealed + HML_OFF_PAYLOAD, m - (size_t)HML_OFF_PAYLOAD);
  }
  hml_frame_hdr rh;
  const uint8_t *rp = NULL;
  if (hml_proto_frame_parse(again, m, &rh, &rp) != HML_OK)
    hml_fuzz_finding("a re-serialised datagram no longer parses");
  if (rh.stream_id != sh.stream_id || rh.seq != sh.seq || rh.frag_idx != sh.frag_idx ||
      rh.frag_cnt != sh.frag_cnt || rh.ts_us != sh.ts_us || rh.flags != sh.flags ||
      rh.payload_len != sh.payload_len) {
    hml_fuzz_finding("header round trip lost a field: %u/%u/%u/%u/%llu/%u/%u", rh.stream_id,
                     rh.seq, rh.frag_idx, rh.frag_cnt, (unsigned long long)rh.ts_us, rh.flags,
                     rh.payload_len);
  }
  if (memcmp(sealed, again, (size_t)HML_FRAME_HDR_SIZE) != 0)
    hml_fuzz_finding("re-serialised header differs from the parsed datagram");
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  run(data, size);
  return 0;
}

#ifndef HML_FUZZ_LIBFUZZER
int main(int argc, char **argv) { return hml_fuzz_main(argc, argv, "fuzz_frame", run, NULL); }
#endif
