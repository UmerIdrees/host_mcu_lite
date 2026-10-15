#ifndef HML_E2E_CPU_H
#define HML_E2E_CPU_H

/*
 * e2e_cpu.h -- shared check harness for the CPU-only end-to-end runner.
 *
 * Every scene runs on the build machine's CPU, over 127.0.0.1 only, and drives
 * the shipping library: the same hml_stream / hml_reasm / hml_control /
 * hml_rocev2 / hml_roce_demux code the tools and the Renode firmware image
 * call. Nothing here mocks, stubs or short-circuits a library path. See
 * docs/cpu_only_e2e.md for what that does and does not establish.
 *
 * Machine-readable output contract, one line per assertion:
 *
 *   E2E scene=<scene> case=<name> PASS <detail>
 *   E2E scene=<scene> case=<name> FAIL <detail>
 *   E2E scene=<scene> skip=<id> <reason>
 *   E2E scene=<scene> result=<PASS|FAIL> checks=<n> fails=<m> elapsed_ms=<n>
 *
 * Exit status: 0 when every case passed, 1 when any case failed or any step
 * could not be executed. A step that cannot be executed FAILS; only an
 * explicitly unavailable external dependency may SKIP, and then it must say
 * which one.
 */

#include "hml/hml_port.h"
#include "hml/hml_protocol.h"
#include "hml/hml_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Private loopback range. Deliberately disjoint from every other suite in the
 * tree so a parallel ctest cannot collide: 45100-45299 test_soak_ctrl,
 * 45300-45499 test_soak_roce, 45502/45511 test_lifecycle/test_loopback,
 * 45600-45699 the M4 bridge demo, 14791/14793/14893/14894 the T1a CLI tests,
 * 55100-55102 the shipped tools. Port 4791 is never used: it is privileged.
 */
#define E2E_PORT_BASE 45700u
#define E2E_PORT_COUNT 100u

typedef struct e2e_harness {
  const char *scene;
  unsigned checks;
  unsigned fails;
  unsigned skipped;
  uint64_t t0_us;
} e2e_harness;

void e2e_emit(e2e_harness *h, const char *name, bool ok, const char *detail);
void e2e_skip(e2e_harness *h, const char *id, const char *reason);
int e2e_finish(e2e_harness *h);

void e2e_case(e2e_harness *h, const char *name, bool ok);
void e2e_eq_u(e2e_harness *h, const char *name, unsigned long long got,
              unsigned long long want);
void e2e_eq_i(e2e_harness *h, const char *name, long got, long want);
void e2e_eq_s(e2e_harness *h, const char *name, const char *got, const char *want);

#define E2E_OK(h, name, cond) e2e_case((h), (name), (cond))
#define E2E_EQ_U(h, name, got, want) e2e_eq_u((h), (name), (got), (want))
#define E2E_EQ_I(h, name, got, want) e2e_eq_i((h), (name), (got), (want))
#define E2E_EQ_S(h, name, got, want) e2e_eq_s((h), (name), (got), (want))

/*
 * First free UDP port in [E2E_PORT_BASE, E2E_PORT_BASE+E2E_PORT_COUNT), or 0
 * when the whole range is occupied. The probe is a raw socket WITHOUT
 * SO_REUSEADDR, because hml_sock sets SO_REUSEADDR and two SO_REUSEADDR
 * sockets may share a UDP port -- probing the same way the library binds would
 * report a busy port as free.
 */
uint16_t e2e_port_pick(void);

/* Ask for a port explicitly, so the matrix can pin a row to a known-free one. */
bool e2e_port_free(uint16_t port);

void e2e_sleep_us(uint64_t us);
uint64_t e2e_now_us(void);

/*
 * Run-wide knobs set from the command line before any scene starts.
 * g_e2e_frames    frame count for the socket scenes (>= 3)
 * g_e2e_deadline  absolute hml_clock_us() value after which every wait loop
 *                 must give up and the scene must report a failure rather than
 *                 block. Set from --seconds; 0 means "derive per scene".
 */
extern unsigned g_e2e_frames;
extern uint64_t g_e2e_deadline;

/* Milliseconds left before g_e2e_deadline, or 0 when no deadline is armed. */
int e2e_budget_ms(void);

/* Serialise one HML data datagram exactly as src/transports/udp does. Returns
 * the datagram length. Used by the scenes that need to hand-craft bytes. */
size_t e2e_build_dg(uint8_t *dg, size_t cap, uint16_t stream, uint32_t seq, uint16_t frag_idx,
                    uint16_t frag_cnt, uint8_t flags, uint64_t ts_us, const uint8_t *payload,
                    uint16_t len);

int e2e_scene_udp(e2e_harness *h);
int e2e_scene_roce(e2e_harness *h);
int e2e_scene_reasm(e2e_harness *h);
int e2e_scene_control(e2e_harness *h);
int e2e_scene_mcu(e2e_harness *h);

#endif /* HML_E2E_CPU_H */
