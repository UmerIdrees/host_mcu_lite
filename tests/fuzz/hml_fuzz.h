#ifndef HML_FUZZ_H
#define HML_FUZZ_H

#include "hml/hml_ctrl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Shared plumbing for the hml fuzz harnesses (Track M reliability).
 *
 * Every harness is a standalone main() that reads one input from argv[1], so the
 * same object file works with a libFuzzer driver (-fsanitize=fuzzer supplies its
 * own main and calls LLVMFuzzerTestOneInput) and with fuzz_replay, the
 * self-contained deterministic driver in this directory.
 *
 *   <harness> <file>                  run one corpus input, exit 0 / 3
 *   <harness> --selfmutate --iters N  in-process deterministic mutation loop
 *   <harness> --help
 *
 * Exit codes: 0 clean, 2 usage, 3 FINDING, 4 harness setup error.
 *
 * A FINDING is a defect in the code under test, not in the harness: it covers a
 * fatal signal, an unbounded allocation, a per-input hang, and any invariant the
 * harness can prove about the library. The harness reports the finding on stdout
 * and stderr and exits 3; it never lets a crash escape as a raw signal death, so
 * a crashing input is always attributable to a named corpus file.
 */

#define HML_FUZZ_EXIT_OK 0
#define HML_FUZZ_EXIT_USAGE 2
#define HML_FUZZ_EXIT_FINDING 3
#define HML_FUZZ_EXIT_ERROR 4

/* One input is capped so a hostile corpus entry cannot make a harness walk an
 * unbounded amount of memory. Everything the wire can carry is far below this. */
#define HML_FUZZ_MAX_INPUT (64u * 1024u)

/* Largest buffer a seal_* helper can produce. */
#define HML_FUZZ_MAX_SEAL 4096u

/* Largest control datagram: 12-byte header + HML_CTRL_MAX_PAYLOAD + crc16. The
 * frozen public header hml_ctrl.h deliberately does not spell the layout out (only
 * the codec in src/control does), so the harnesses that do not link the control
 * server spell it out here. test_fuzz_corpus checks the corpus against the real
 * codec, so a divergence shows up as a failing case rather than a silent one. */
#define HML_FUZZ_CTRL_MAX_MSG (12u + HML_CTRL_MAX_PAYLOAD + 2u)

/* Control header layout, matching HML_CTRL_RSVD_OFF in hml_ctrl_codec.c. */
#define HML_CTRL_HDR_SIZE 12u
#define HML_OFF_CTRL_MAGIC 0u
#define HML_OFF_CTRL_VERSION 4u
#define HML_OFF_CTRL_OP 5u
#define HML_OFF_CTRL_FLAGS 6u
#define HML_OFF_CTRL_LEN 8u
#define HML_OFF_CTRL_RSVD 10u

/* Beacon layout, matching the offsets in hml_discovery.c. The timestamp is 48 bits
 * wide, which is why the 64-bit value a case asks for has to be masked or dropped. */
#define HML_OFF_DISC_MAGIC 0u
#define HML_OFF_DISC_VERSION 4u
#define HML_OFF_DISC_UUID 8u
#define HML_OFF_DISC_TS 20u
#define HML_DISC_CRC_OFF 26u

/* hml_dest_cfg layout, matching the CFG_OFF_* values in hml_protocol.c. */
#define HML_OFF_CFG_KIND 0u
#define HML_OFF_CFG_PAD0 1u
#define HML_OFF_CFG_UDP_PORT 2u
#define HML_OFF_CFG_IPV4 4u
#define HML_OFF_CFG_QP 8u
#define HML_OFF_CFG_PSN 12u
#define HML_OFF_CFG_RKEY 16u
#define HML_OFF_CFG_REMOTE_ADDR 20u
#define HML_OFF_CFG_FRAME_SIZE 28u
#define HML_OFF_CFG_WINDOW_LEN 32u
#define HML_OFF_CFG_HOP_LIMIT 34u
#define HML_OFF_CFG_PAD1 35u
#define HML_OFF_CFG_RSVD 36u

/* ---- harness skeleton --------------------------------------------------- */

typedef void (*hml_fuzz_input_fn)(const uint8_t *data, size_t size);
typedef void (*hml_fuzz_once_fn)(void);

/* Installs the signal handlers, the address-space limit and the allocation
 * guard, then runs either the single-input or the self-mutation mode. once, if
 * non-NULL, runs exactly once before the first input. */
int hml_fuzz_main(int argc, char **argv, const char *harness, hml_fuzz_input_fn run,
                  hml_fuzz_once_fn once);

/* Report a defect in the code under test and terminate with exit code 3. */
void hml_fuzz_finding(const char *fmt, ...);

/* ---- structural re-sealing ----------------------------------------------
 *
 * A mutation-only fuzzer almost never produces a byte string that survives a
 * CRC check, so without help it would only ever exercise the reject paths. Each
 * seal_* helper rebuilds a structurally valid record around the fuzz bytes --
 * correct magic, recomputed CRCs, a length field consistent with the bytes
 * actually present -- so one input reaches both the reject path (fed raw) and
 * the deep accept path (fed sealed).
 */

/* Frame datagram: magic+version+CRCs fixed, payload_len taken from data[0..1]
 * clamped to HML_MAX_FRAG_PAYLOAD, every other header field taken from data. */
size_t hml_fuzz_seal_frame(const uint8_t *data, size_t n, uint8_t *out, size_t cap);

/* Same idea for the control datagram (12-byte header + crc16) and for the
 * 40-byte hml_dest_cfg / 28-byte beacon records. */
size_t hml_fuzz_seal_ctrl(const uint8_t *data, size_t n, uint8_t *out, size_t cap);
size_t hml_fuzz_seal_cfg(const uint8_t *data, size_t n, uint8_t *out, size_t cap);
size_t hml_fuzz_seal_beacon(const uint8_t *data, size_t n, uint8_t *out, size_t cap);

/* ---- reassembly arena capacity selection --------------------------------- */

/* fuzz_reasm also takes the arena capacity to open from the input, because the
 * capacity is the interesting size_t: it is what an unchecked call to
 * hml_reasm_open would multiply by something. The first input byte selects an
 * index into this table, and both the harness and the corpus generator go through
 * hml_fuzz_reasm_cap() so the two cannot drift.
 *
 * The table deliberately includes 0, 1, every protocol constant, values just past
 * the clamps, and the three shapes of "absurd": 0x7FFFFFFF, 0x80000000 (which is
 * negative read as a signed size), and SIZE_MAX. */
#define HML_FUZZ_REASM_NCAPS 12u
#define HML_FUZZ_CAP_0 0u
#define HML_FUZZ_CAP_1 1u
#define HML_FUZZ_CAP_HDR 2u
#define HML_FUZZ_CAP_FRAG 3u
#define HML_FUZZ_CAP_4K 4u
#define HML_FUZZ_CAP_64K 5u
#define HML_FUZZ_CAP_MAX_FRAME 6u
#define HML_FUZZ_CAP_MAX_FRAME_1 7u
#define HML_FUZZ_CAP_7FFFFFFF 8u
#define HML_FUZZ_CAP_80000000 9u
#define HML_FUZZ_CAP_FFFFFFFF 10u
#define HML_FUZZ_CAP_SIZE_MAX 11u

/* Requested arena capacity for a table index, or 0 if index >= NCAPS. */
size_t hml_fuzz_reasm_cap(unsigned index);

/* ---- misc helpers ------------------------------------------------------- */

/* xorshift32: the mutation loop must be reproducible, so no libc rand(). */
uint32_t hml_fuzz_rng(uint32_t *state);

/* Deterministic byte-level mutation. Returns the new length. */
size_t hml_fuzz_mutate(const uint8_t *seed, size_t n, uint32_t *rng, uint8_t *out,
                       size_t cap);

/* Read a whole file, bounded by HML_FUZZ_MAX_INPUT. Returns false on error. */
bool hml_fuzz_read_file(const char *path, uint8_t *out, size_t cap, size_t *n);

/* Recursive mkdir -p, used by the corpus generator. Returns 0 on success. */
int hml_fuzz_mkdir_p(const char *path);

/* ---- UDP port allocation for the socket-backed harnesses ---------------- */

/* True when a plain UDP bind to 127.0.0.1:port succeeds right now. Uses a
 * socket without SO_REUSEADDR, so a port already taken by a peer is reported as
 * busy even when that peer set SO_REUSEADDR. */
bool hml_fuzz_port_free(uint16_t port);

/* First free port at or after base, wrapping inside [lo, hi]. Returns 0 when
 * the whole range is busy. */
uint16_t hml_fuzz_port_pick(uint16_t base, uint16_t lo, uint16_t hi, unsigned tries);

#endif /* HML_FUZZ_H */
