/*
 * fuzz_ctrl -- hml_ctrl_parse / hml_ctrl_fill (hml_ctrl.h) and the discovery
 * beacon codec (hml_discovery.h).
 *
 * Both records are reached by a datagram from any host on the control port, and
 * both are parsed before anything decides what the request means, so they are
 * the cheapest way to make a device misbehave.
 *
 * Invariants asserted:
 *   - hml_ctrl_parse is fail-closed on length: the wire total is exactly
 *     12 + len + 2, so trailing junk must be rejected as firmly as truncation.
 *     A sealed record truncated or extended by one byte must never parse.
 *   - a sealed control record always parses, its payload pointer is exactly 12
 *     bytes in, and hml_ctrl_fill reproduces the identical bytes;
 *   - a filled record whose declared length is one byte shorter than the bytes
 *     present is never accepted;
 *   - hml_beacon_parse accepts nothing but exactly HML_DISC_SIZE bytes and the
 *     48-bit timestamp never exceeds 2^48-1;
 *   - hml_ctrl_fill refuses (writes nothing, reports 0) when max_dst is smaller
 *     than the record it was asked for, so a short buffer cannot be overrun.
 */

#include "hml_fuzz.h"

#include "hml/hml_ctrl.h"
#include "hml/hml_discovery.h"

#include <stdio.h>
#include <string.h>

static void run_ctrl(const uint8_t *data, size_t n) {
  hml_ctrl_msg m;
  const uint8_t *pl = NULL;

  (void)hml_ctrl_parse(data, n, &m, &pl);

  size_t sweep = n < 64u ? n : 64u;
  for (size_t k = 0; k <= sweep; k++) {
    hml_ctrl_msg mk;
    const uint8_t *pk = NULL;
    if (hml_ctrl_parse(data, k, &mk, &pk) != HML_OK) continue;
    if (pk == NULL) hml_fuzz_finding("hml_ctrl_parse returned HML_OK with a null payload");
    if (pk < data || (size_t)(pk - data) + (size_t)mk.payload_len > k) {
      hml_fuzz_finding("accepted payload of %u bytes at offset %td escapes the %zu byte input",
                       (unsigned)mk.payload_len, pk - data, k);
    }
    if (mk.payload_len > (uint16_t)HML_CTRL_MAX_PAYLOAD) {
      hml_fuzz_finding("accepted payload_len %u above the %u byte maximum",
                       (unsigned)mk.payload_len, (unsigned)HML_CTRL_MAX_PAYLOAD);
    }
  }

  if (hml_ctrl_parse(NULL, n, &m, &pl) != HML_ERR_ARG)
    hml_fuzz_finding("hml_ctrl_parse(NULL, ..) did not return HML_ERR_ARG");
  if (hml_ctrl_parse(data, n, NULL, &pl) != HML_ERR_ARG)
    hml_fuzz_finding("hml_ctrl_parse(.., NULL, ..) did not return HML_ERR_ARG");
  if (hml_ctrl_parse(data, n, &m, NULL) != HML_ERR_ARG)
    hml_fuzz_finding("hml_ctrl_parse(.., .., NULL) did not return HML_ERR_ARG");

  /* hml_ctrl_fill must be all-or-nothing: a buffer one byte too small gets
   * nothing written and reports 0, rather than a truncated record. */
  uint8_t small[HML_FUZZ_MAX_SEAL];
  hml_ctrl_msg probe;
  memset(&probe, 0, sizeof(probe));
  probe.op = HML_OP_PING;
  probe.flags = HML_CTRL_FLAG_REQ;
  probe.payload = data;
  probe.payload_len = (uint16_t)(n > (uint16_t)HML_CTRL_MAX_PAYLOAD ? (uint16_t)HML_CTRL_MAX_PAYLOAD
                                                                    : (uint16_t)n);
  uint16_t wrote = 0xbeefu;
  hml_ctrl_fill(&probe, small, 1u, &wrote);
  if (wrote != 0u) hml_fuzz_finding("hml_ctrl_fill reported %u bytes into a 1 byte buffer",
                                    (unsigned)wrote);
  wrote = 0xbeefu;
  hml_ctrl_fill(NULL, small, (uint16_t)sizeof(small), &wrote);
  if (wrote != 0u) hml_fuzz_finding("hml_ctrl_fill(NULL) reported %u bytes",
                                    (unsigned)wrote);
  wrote = 0xbeefu;
  hml_ctrl_fill(&probe, NULL, (uint16_t)sizeof(small), &wrote);
  if (wrote != 0u) hml_fuzz_finding("hml_ctrl_fill(.., NULL, ..) did not report 0 bytes");
  hml_ctrl_fill(&probe, small, (uint16_t)sizeof(small), NULL);
  {
    /* payload_len above the maximum must be refused outright. */
    hml_ctrl_msg big = probe;
    big.payload_len = (uint16_t)(HML_CTRL_MAX_PAYLOAD + 1u);
    wrote = 0xbeefu;
    hml_ctrl_fill(&big, small, (uint16_t)sizeof(small), &wrote);
    if (wrote != 0u) hml_fuzz_finding("hml_ctrl_fill accepted payload_len %u",
                                      (unsigned)big.payload_len);
    big.payload_len = 0xfffdu;
    wrote = 0xbeefu;
    hml_ctrl_fill(&big, small, (uint16_t)sizeof(small), &wrote);
    if (wrote != 0u) hml_fuzz_finding("hml_ctrl_fill accepted payload_len 0x%04x",
                                      (unsigned)big.payload_len);
  }

  /* Sealed: the accept path plus an exact byte-for-byte round trip. */
  uint8_t sealed[HML_FUZZ_MAX_SEAL];
  size_t sl = hml_fuzz_seal_ctrl(data, n, sealed, sizeof(sealed));
  if (sl == 0u) return;

  hml_ctrl_msg sm;
  const uint8_t *sp = NULL;
  if (hml_ctrl_parse(sealed, sl, &sm, &sp) != HML_OK)
    hml_fuzz_finding("a sealed control record was rejected");
  if (sp != sealed + 12u) {
    hml_fuzz_finding("control payload pointer is %p, want %p", (const void *)sp,
                     (const void *)(sealed + 12u));
  }
  if ((size_t)sm.payload_len + 14u != sl) {
    hml_fuzz_finding("sealed length %zu does not match 12 + %u + 2", sl,
                     (unsigned)sm.payload_len);
  }
  if (sm.payload != sp) hml_fuzz_finding("hml_ctrl_msg.payload does not alias the payload");

  uint8_t again[HML_FUZZ_MAX_SEAL];
  uint16_t again_n = 0;
  hml_ctrl_fill(&sm, again, (uint16_t)sizeof(again), &again_n);
  if (again_n != sl) {
    hml_fuzz_finding("hml_ctrl_fill wrote %u bytes for a %zu byte record", (unsigned)again_n,
                     sl);
  }
  if (memcmp(sealed, again, sl) != 0) {
    hml_fuzz_finding("hml_ctrl_fill did not reproduce the parsed control record");
  }

  /* Fail closed at both edges. */
  hml_ctrl_msg em;
  const uint8_t *ep = NULL;
  if (sl > 1u && hml_ctrl_parse(sealed, sl - 1u, &em, &ep) == HML_OK) {
    hml_fuzz_finding("a control record truncated by one byte was accepted");
  }
  uint8_t extended[HML_FUZZ_MAX_SEAL + 1u];
  memcpy(extended, sealed, sl);
  extended[sl] = 0u;
  if (hml_ctrl_parse(extended, sl + 1u, &em, &ep) == HML_OK) {
    hml_fuzz_finding("a control record with one trailing junk byte was accepted");
  }
}

static void run_beacon(const uint8_t *data, size_t n) {
  hml_beacon b;
  (void)hml_beacon_parse(data, n, &b);
  if (hml_beacon_parse(NULL, n, &b) != HML_ERR_ARG)
    hml_fuzz_finding("hml_beacon_parse(NULL, ..) did not return HML_ERR_ARG");
  if (hml_beacon_parse(data, n, NULL) != HML_ERR_ARG)
    hml_fuzz_finding("hml_beacon_parse(.., NULL) did not return HML_ERR_ARG");
  hml_beacon_fill(NULL, NULL);

  size_t sweep = n < 40u ? n : 40u;
  for (size_t k = 0; k <= sweep; k++) {
    hml_beacon bk;
    if (hml_beacon_parse(data, k, &bk) == HML_OK && k != (size_t)HML_DISC_SIZE) {
      hml_fuzz_finding("a %zu byte record was accepted as a %u byte beacon", k,
                       (unsigned)HML_DISC_SIZE);
    }
  }

  uint8_t sealed[HML_FUZZ_MAX_SEAL];
  size_t sl = hml_fuzz_seal_beacon(data, n, sealed, sizeof(sealed));
  if (sl == 0u) return;
  hml_beacon b1;
  if (hml_beacon_parse(sealed, sl, &b1) != HML_OK)
    hml_fuzz_finding("a sealed 28 byte beacon was rejected");
  if (b1.ts_us > 0x0000ffffffffffffull) {
    hml_fuzz_finding("beacon ts_us %llu exceeds the 48-bit field", (unsigned long long)b1.ts_us);
  }
  uint8_t again[HML_FUZZ_MAX_SEAL];
  hml_beacon_fill(&b1, again);
  if (memcmp(sealed, again, (size_t)HML_DISC_SIZE) != 0)
    hml_fuzz_finding("hml_beacon_fill did not reproduce the parsed beacon");
}

static void run(const uint8_t *data, size_t n) {
  run_ctrl(data, n);
  run_beacon(data, n);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  run(data, size);
  return 0;
}

#ifndef HML_FUZZ_LIBFUZZER
int main(int argc, char **argv) { return hml_fuzz_main(argc, argv, "fuzz_ctrl", run, NULL); }
#endif
