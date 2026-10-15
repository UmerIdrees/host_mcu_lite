/*
 * main_host_selftest.c -- HOST-ONLY runner for the demo body.
 *
 * READ THIS BEFORE TRUSTING THE TEST CALLED renode_demo_native_selftest:
 *
 *   This program runs hml_mcu_demo_run() compiled for the build machine's
 *   architecture, with the library compiled for the build machine's
 *   architecture, on the build machine, against the build machine's libc. No
 *   Cortex-M instruction executes. No FreeRTOS executes. Renode is not
 *   involved. There is no emulator, no UART, no MMIO, no SysTick and no
 *   memory map.
 *
 *   Its entire purpose is to keep app/hml_mcu_demo.c compiling under
 *   -Wall -Wextra -Wpedantic -Werror and to keep the UART marker strings in
 *   sync with renode/holoscan_mcu_lite.robot, on a machine that has neither
 *   arm-none-eabi-gcc nor Renode installed. That is a real, narrow, useful
 *   job. It is emphatically NOT a microcontroller test result and must never
 *   be reported as one: an x86_64 run says nothing about Cortex-M codegen,
 *   alignment, endianness traps, stack depth, MMIO, interrupts or timing.
 *
 *   The genuine Cortex-M execution is renode/holoscan_mcu_lite.robot, and
 *   that one is skipped whenever Renode or the ARM toolchain is missing.
 */

#include "hml_mcu_demo.h"

#include <stdio.h>
#include <string.h>

static void stdout_puts(void *ctx, const char *line) {
  (void)ctx;
  (void)printf("%s\n", line);
}

int main(void) {
  hml_mcu_demo_cfg cfg;
  hml_mcu_demo_io io;
  hml_mcu_demo_result res;
  int rc;

  printf("NOTE: native x86/host build of the demo body. This is NOT a Cortex-M run,\n");
  printf("      NOT a FreeRTOS run and NOT a Renode run. See docs/renode.md.\n");

  memset(&cfg, 0, sizeof(cfg));
  memset(&io, 0, sizeof(io));
  memset(&res, 0, sizeof(res));
  io.ctx = NULL;
  io.puts = stdout_puts;

  rc = hml_mcu_demo_run(&cfg, &io, &res);
  printf("native-selftest checks=%u failures=%u frames_delivered=%u rc=%d\n", (unsigned)res.checks,
         (unsigned)res.failures, (unsigned)res.frames_delivered, rc);
  if (rc != 0) {
    printf("native-selftest FAILED: the demo body reported %u failed check(s).\n",
           (unsigned)res.failures);
    return 1;
  }
  if (res.checks < 20u) {
    printf("native-selftest FAILED: only %u checks ran, expected at least 20; "
           "the demo was probably truncated.\n",
           (unsigned)res.checks);
    return 1;
  }
  /* The robot test greps for these; a rename that misses the .robot file must
   * fail here rather than turn into a mysterious timeout under Renode. */
  if (strstr(HML_MCU_DEMO_MARK_READY, "HML-MCU-DEMO") == NULL ||
      strstr(HML_MCU_DEMO_MARK_DONE, "HML-MCU-DEMO") == NULL) {
    printf("native-selftest FAILED: a marker string no longer carries the HML-MCU-DEMO prefix.\n");
    return 1;
  }
  printf("native-selftest OK (host build only; the Cortex-M path is renode_smoke).\n");
  return 0;
}
