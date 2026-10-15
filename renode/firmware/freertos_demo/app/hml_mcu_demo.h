/*
 * hml_mcu_demo.h -- the portable body of the Renode MCU smoke demo.
 *
 * WHAT THIS FILE IS
 *   Real holoscan_mcu_lite library code, driven from a bare-metal image. It
 *   calls the shipping library entry points (hml_crc16, hml_crc32_ieee,
 *   hml_beacon_fill/parse, hml_ctrl_fill/parse, hml_proto_frame_fill/parse,
 *   hml_proto_cfg_fill/parse, hml_reasm_*) and prints one line per check to a
 *   caller-supplied sink. No hml code is reimplemented or stubbed here.
 *
 * WHAT THIS FILE IS NOT
 *   It is not a FreeRTOS application, it is not a sensor HAL, and it does not
 *   open a socket, send a datagram or touch a peripheral. There is no lwIP in
 *   the image. The RTOS it is driven by is a cooperative stub -- see
 *   ../stub_freertos/README.md. The whole point of the image is to prove the
 *   codec/CRC/reassembly code compiles and produces correct answers for a
 *   Cortex-M, nothing more. See ../../../docs/renode.md for the full boundary.
 *
 * PORTABILITY
 *   No MMIO, no FreeRTOS, no lwIP, no libc beyond <string.h>: the same file is
 *   compiled for Cortex-M (platform/startup_stub_rtos.c) and for the host
 *   self-test (platform/main_host_selftest.c). The host build proves nothing
 *   about Cortex-M execution; it only keeps this file compiling.
 */
#ifndef HML_MCU_DEMO_H
#define HML_MCU_DEMO_H

#include <stddef.h>
#include <stdint.h>

/* Longest line this file can emit, including the room a caller needs for its
 * own formatting. Output is line-at-a-time and never blocks. */
#define HML_MCU_DEMO_LINE_MAX 160u

/* UART markers the Renode robot test greps for. Every marker the robot file
 * names must be defined here, and renode_selfcheck asserts exactly that, so a
 * rename on either side breaks the self-check instead of silently making the
 * robot test wait for a line nobody prints. */
#define HML_MCU_DEMO_MARK_READY "HML-MCU-DEMO-READY"
#define HML_MCU_DEMO_MARK_DONE  "HML-MCU-DEMO-DONE"
#define HML_MCU_DEMO_MARK_FAIL  "HML-MCU-DEMO-FAIL"
#define HML_MCU_DEMO_MARK_WARN  "HML-MCU-DEMO-WARN"
#define HML_MCU_DEMO_MARK_PHASE "HML-MCU-DEMO-PHASE"
#define HML_MCU_DEMO_MARK_NOTE  "HML-MCU-DEMO-NOTE"
/* Emitted by the image's task, not by the portable body, but asserted by the
 * robot test, so it belongs in the same list. */
#define HML_MCU_DEMO_MARK_TASK  "HML-MCU-DEMO-TASK"

/* Sink for the demo's output. puts() receives one line with no trailing '\n'. */
typedef struct hml_mcu_demo_io {
  void *ctx;
  void (*puts)(void *ctx, const char *line);
} hml_mcu_demo_io;

typedef struct hml_mcu_demo_cfg {
  /* Passed straight to hml_reasm_open(). 0 selects the library default, which
   * is HML_MAX_FRAME_SIZE (1 MiB) and therefore TWO 1 MiB mallocs -- see the
   * memory contract in ../../../docs/renode.md section 3. */
  uint32_t frame_bytes;
  uint32_t device_uuid;
  uint32_t device_caps;
  /* Cooperative pause between phases, so the stub RTOS tick visibly advances.
   * NULL means "do not pause". */
  void (*pause)(void *ctx, uint32_t ms);
  void *pause_ctx;
} hml_mcu_demo_cfg;

typedef struct hml_mcu_demo_result {
  uint32_t checks;
  uint32_t failures;
  uint32_t frames_delivered;
} hml_mcu_demo_result;

/* Runs every check. Returns 0 when result->failures == 0, 1 when at least one
 * check failed, -1 on a NULL argument. Never returns early on a failed check:
 * the remaining checks still run so one boot reports every divergence. */
int hml_mcu_demo_run(const hml_mcu_demo_cfg *cfg, const hml_mcu_demo_io *io,
                     hml_mcu_demo_result *res);

#endif /* HML_MCU_DEMO_H */
