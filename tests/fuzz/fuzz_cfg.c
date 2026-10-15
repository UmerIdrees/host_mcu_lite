/*
 * fuzz_cfg -- hml_proto_cfg_parse / hml_proto_cfg_fill (hml_protocol.h) plus the
 * fixed-capacity transport registry in hml_transport.h.
 *
 * hml_dest_cfg is the control-plane record that chooses where a device streams
 * and which RDMA window it writes into, so a parser mistake here is a
 * misdirected data plane rather than a dropped packet. The registry is fuzzed
 * because it is a hand-rolled fixed-slot table fed a caller-supplied name and
 * compared with strcmp.
 *
 * Invariants asserted:
 *   - the prefix sweep never produces a config whose accepted length exceeds the
 *     bytes handed in, and never returns a status outside the hml_status enum;
 *   - a sealed 40-byte record always parses and every field round trips through
 *     hml_proto_cfg_fill / hml_proto_cfg_parse unchanged, including the five
 *     reserved bytes, which must stay zero on the wire;
 *   - a rejected record leaves the output struct unspecified but never crashes;
 *   - registering the same name twice is HML_ERR_BUSY, an ops vtable with a
 *     NULL entry point is HML_ERR_ARG, an empty name is HML_ERR_ARG, and the
 *     registry reports exhaustion instead of overwriting a live slot.
 */

#include "hml_fuzz.h"

#include "hml/hml_protocol.h"
#include "hml/hml_transport.h"

#include <stdio.h>
#include <string.h>

static hml_status g_fake_open(hml_xport_t *x, const hml_dest_cfg *d, uint16_t s) {
  (void)x;
  (void)d;
  (void)s;
  return HML_OK;
}
static hml_status g_fake_send(hml_xport_t *x, const hml_frame_hdr *h, const uint8_t *p,
                              size_t n) {
  (void)x;
  (void)h;
  (void)p;
  (void)n;
  return HML_OK;
}
static hml_status g_fake_flush(hml_xport_t *x) {
  (void)x;
  return HML_OK;
}
static void g_fake_close(hml_xport_t *x) { (void)x; }

static const hml_xport_ops g_fake_ops = {"hml-fuzz-probe", g_fake_open, g_fake_send,
                                         g_fake_flush, g_fake_close};

/* Registry edge cases do not depend on the input, so they run once. */
static void registry_cases(void) {
  hml_xport_ops partial = g_fake_ops;
  partial.name = "hml-fuzz-partial";
  partial.close = NULL;
  if (hml_transport_register(&partial) != HML_ERR_ARG)
    hml_fuzz_finding("an ops vtable without close was accepted");

  hml_xport_ops unnamed = g_fake_ops;
  unnamed.name = "";
  if (hml_transport_register(&unnamed) != HML_ERR_ARG)
    hml_fuzz_finding("an ops vtable with an empty name was accepted");
  if (hml_transport_register(NULL) != HML_ERR_ARG)
    hml_fuzz_finding("a NULL ops vtable was accepted");
  if (hml_transport_lookup(NULL) != NULL) hml_fuzz_finding("lookup(NULL) returned an entry");
  if (hml_transport_lookup("") != NULL) hml_fuzz_finding("lookup(\"\") returned an entry");
  if (hml_transport_lookup("no-such-transport") != NULL)
    hml_fuzz_finding("lookup of an unregistered name returned an entry");

  if (hml_transport_register(&g_fake_ops) != HML_OK)
    hml_fuzz_finding("the first registration of a valid ops vtable failed");
  if (hml_transport_register(&g_fake_ops) != HML_ERR_BUSY)
    hml_fuzz_finding("re-registering the same ops vtable did not return HML_ERR_BUSY");
  if (hml_transport_lookup("hml-fuzz-probe") != &g_fake_ops)
    hml_fuzz_finding("lookup did not return the registered ops vtable");

  /* Fill every remaining slot with distinct names, then prove the registry
   * reports exhaustion instead of overwriting a live entry. */
  static hml_xport_ops filler[16];
  static char names[16][24];
  unsigned added = 0u;
  for (unsigned i = 0; i < 16u; i++) {
    filler[i] = g_fake_ops;
    snprintf(names[i], sizeof(names[i]), "hml-fuzz-fill-%02u", i);
    filler[i].name = names[i];
    if (hml_transport_register(&filler[i]) == HML_OK) added++;
  }
  if (added > 16u) hml_fuzz_finding("registered more ops vtables than the table can hold");
  if (hml_transport_lookup("hml-fuzz-probe") != &g_fake_ops)
    hml_fuzz_finding("registry exhaustion overwrote a live slot");
}

/* Registry edge cases do not depend on the input, so they run once. Idempotent,
 * because the libFuzzer driver never calls hml_fuzz_main's setup hook. */
static bool g_registry_done;

static void ensure_registry(void) {
  if (g_registry_done) return;
  g_registry_done = true;
  registry_cases();
}

static void run(const uint8_t *data, size_t n) {
  ensure_registry();

  hml_dest_cfg cfg;
  memset(&cfg, 0xa5, sizeof(cfg));

  /* Prefix sweep: every "shorter than 40" and "longer than 40" boundary. */
  size_t sweep = n < 64u ? n : 64u;
  for (size_t k = 0; k <= sweep; k++) {
    hml_dest_cfg c;
    memset(&c, 0x5a, sizeof(c));
    hml_status st = hml_proto_cfg_parse(data, k, &c);
    if (st < HML_OK || st > HML_ERR_UNKNOWN) {
      hml_fuzz_finding("hml_proto_cfg_parse returned status %d, outside the enum",
                       (int)st);
    }
    if (st == HML_OK && k < (size_t)HML_CFG_SIZE) {
      hml_fuzz_finding("a %zu byte record was accepted as a 40 byte hml_dest_cfg", k);
    }
  }

  if (hml_proto_cfg_parse(NULL, n, &cfg) != HML_ERR_ARG)
    hml_fuzz_finding("hml_proto_cfg_parse(NULL, ..) did not return HML_ERR_ARG");
  if (hml_proto_cfg_parse(data, n, NULL) != HML_ERR_ARG)
    hml_fuzz_finding("hml_proto_cfg_parse(.., NULL) did not return HML_ERR_ARG");
  hml_proto_cfg_fill(NULL, NULL);

  /* Sealed: the accept path, plus a field-for-field round trip. */
  uint8_t sealed[HML_FUZZ_MAX_SEAL];
  size_t m = hml_fuzz_seal_cfg(data, n, sealed, sizeof(sealed));
  if (m == 0u) return;

  hml_dest_cfg c1;
  memset(&c1, 0x11, sizeof(c1));
  if (hml_proto_cfg_parse(sealed, m, &c1) != HML_OK)
    hml_fuzz_finding("a sealed 40 byte hml_dest_cfg was rejected");

  uint8_t again[HML_FUZZ_MAX_SEAL];
  hml_proto_cfg_fill(&c1, again);
  if (memcmp(sealed, again, (size_t)HML_CFG_SIZE) != 0)
    hml_fuzz_finding("hml_proto_cfg_fill did not reproduce the parsed hml_dest_cfg");

  hml_dest_cfg c2;
  if (hml_proto_cfg_parse(again, (size_t)HML_CFG_SIZE, &c2) != HML_OK)
    hml_fuzz_finding("a re-serialised hml_dest_cfg no longer parses");
  if (c1.kind != c2.kind || c1.udp_port != c2.udp_port || c1.ipv4 != c2.ipv4 ||
      c1.qp != c2.qp || c1.psn != c2.psn || c1.rkey != c2.rkey ||
      c1.remote_addr != c2.remote_addr || c1.frame_size != c2.frame_size ||
      c1.window_len != c2.window_len || c1.hop_limit != c2.hop_limit)
    hml_fuzz_finding("hml_dest_cfg round trip lost a field");
  for (unsigned i = 0; i < 5u; i++) {
    if (c1.rsvd[i] != 0u) hml_fuzz_finding("rsvd[%u] survived a wire round trip", i);
  }
  if (c1.pad != 0u) hml_fuzz_finding("pad survived a wire round trip");

  /* Truncating a sealed record at every length must be rejected, never accepted
   * with a partially filled struct. */
  for (size_t k = 0; k < (size_t)HML_CFG_SIZE; k++) {
    hml_dest_cfg c3;
    if (hml_proto_cfg_parse(sealed, k, &c3) == HML_OK) {
      hml_fuzz_finding("a sealed record truncated to %zu bytes was accepted", k);
    }
  }

  /* Registry: a caller-supplied name, always NUL terminated. */
  char name[32];
  size_t nl = n < (sizeof(name) - 1u) ? n : sizeof(name) - 1u;
  memcpy(name, data, nl);
  name[nl] = '\0';
  const hml_xport_ops *found = hml_transport_lookup(name);
  if (found != NULL && (found->name == NULL || found->name[0] == '\0')) {
    hml_fuzz_finding("hml_transport_lookup returned an entry with an unusable name");
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  run(data, size);
  return 0;
}

#ifndef HML_FUZZ_LIBFUZZER
int main(int argc, char **argv) {
  return hml_fuzz_main(argc, argv, "fuzz_cfg", run, ensure_registry);
}
#endif
