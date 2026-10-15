/*
 * host_vendor_selftest.c -- HOST-ONLY runner, shared by every vendor platform.
 *
 * One file in renode/vendors/ rather than five copies. It is compiled once per
 * vendor, each time with -I pointed at that vendor's firmware/platform
 * directory so that "board_config.h" resolves to that vendor's header, and it
 * is registered as the CTest test <vendor>_renode_native_selftest.
 *
 * READ THIS BEFORE TRUSTING ANYTHING THAT TEST PRINTS
 *
 *   This program runs hml_mcu_demo_run() compiled for the BUILD MACHINE's
 *   architecture, linked against the BUILD MACHINE's library, on the build
 *   machine. No Cortex-M and no RV32 instruction executes. No RTOS executes.
 *   Renode is not involved, is never invoked, and need not be installed. There
 *   is no emulator, no UART, no MMIO, no timer, no memory map and no linker
 *   script in play.
 *
 *   Its two jobs, and both are narrow and real:
 *
 *     1. It keeps every vendor's board_config.h compiling under
 *        -Wall -Wextra -Wpedantic -Werror on a machine with no cross compiler,
 *        and asserts the values in it are self-consistent (a non-zero frame cap,
 *        heap == 2 * cap + the reassembler context, an 8-byte aligned heap, ...).
 *
 *     2. It runs the shared demo body at THIS vendor's frame cap, so the cap
 *        recorded in renode/boards/index is known to be one the library's
 *        reassembly checks actually accept. A typo like frame_bytes = 8192 * 2
 *        on the 32 KiB RISC-V part would otherwise only show up as a botched
 *        emulated boot, days later, on a machine with Renode installed.
 *
 *   It is emphatically NOT a microcontroller result and must never be reported
 *   as one. An x86_64 run says nothing about Cortex-M or RV32 codegen, about
 *   alignment traps, endianness, stack depth, MMIO behaviour, interrupt
 *   semantics or timing. The genuine per-vendor execution is
 *   renode/vendors/<vendor>/<vendor>.robot, and that one is skipped (exit 77)
 *   whenever Renode or the matching cross toolchain is missing -- which, on the
 *   machine this harness was written on, is always.
 */

#include "board_config.h"
#include "hml_mcu_demo.h"

#include <stdio.h>
#include <string.h>

/* board_config.h has to describe itself, so a bad vendor header is a COMPILE
 * error on every machine instead of a surprise emulated boot on somebody else's. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(HML_VENDOR_FRAME_BYTES > 0u,
               "a vendor board must declare a non-zero --frame-bytes default");
_Static_assert(HML_VENDOR_HEAP_BYTES == 2u * HML_VENDOR_FRAME_BYTES +
                                               HML_VENDOR_REASM_CTX_BYTES,
               "the heap must be exactly the two hml_reasm_open() buffers plus "
               "the reassembler context they are created inside");
_Static_assert(HML_VENDOR_FLASH_BYTES > 0u && HML_VENDOR_SRAM_BYTES > 0u,
               "a vendor board must declare a non-empty flash and SRAM window");
_Static_assert((HML_VENDOR_HEAP_ADDR & 7u) == 0u,
               "the heap must be 8-byte aligned for newlib _sbrk");
_Static_assert(HML_VENDOR_UART_ADDR != 0u, "a vendor board must declare a UART base");
_Static_assert(HML_VENDOR_STACK_BYTES > 0u, "a vendor board must declare a stack size");
#endif

static int g_fail;

static void stdout_puts(void *ctx, const char *line) {
  (void)ctx;
  (void)printf("%s\n", line);
}

static void check(int ok, const char *what) {
  if (ok) return;
  printf("host-selftest FAIL vendor=%s: %s\n", HML_VENDOR_NAME, what);
  g_fail = 1;
}

int main(void) {
  hml_mcu_demo_cfg cfg;
  hml_mcu_demo_io io;
  hml_mcu_demo_result res;
  int rc;

  printf("NOTE: host build of the shared demo body at the %s frame cap. This is NOT a\n",
         HML_VENDOR_NAME);
  printf("      %s run, NOT a Cortex-M/RV32 run and NOT a Renode run.\n", HML_VENDOR_ARCH);
  printf("      See renode/vendors/%s/README.md.\n", HML_VENDOR_NAME);
  printf("host-selftest vendor=%s arch=%s elf=%s uart=0x%08lx frame_bytes=%lu\n",
         HML_VENDOR_NAME, HML_VENDOR_ARCH, HML_VENDOR_ELF_NAME,
         (unsigned long)HML_VENDOR_UART_ADDR, (unsigned long)HML_VENDOR_FRAME_BYTES);

  /* The board header's arithmetic, re-checked at runtime so a wrong answer is
   * reported with the vendor's name attached rather than only as a link error. */
  check(HML_VENDOR_HEAP_BYTES == 2u * HML_VENDOR_FRAME_BYTES +
                                       HML_VENDOR_REASM_CTX_BYTES,
        "heap_bytes must be 2 * frame_bytes + the reassembler context "
        "(the two hml_reasm_open() buffers do not stand alone)");
  check(HML_VENDOR_FRAME_BYTES != 0u,
        "frame_bytes 0 means the 1 MiB library default, which needs 2 MiB of heap "
        "this part does not have");
  check(HML_VENDOR_UART_ADDR != 0u, "uart_addr must be non-zero");
  check(HML_VENDOR_HEAP_ADDR >= HML_VENDOR_SRAM_ADDR &&
            HML_VENDOR_HEAP_ADDR + HML_VENDOR_HEAP_BYTES <=
                HML_VENDOR_SRAM_ADDR + HML_VENDOR_SRAM_BYTES,
        "the declared heap must lie inside the declared SRAM window");

  memset(&cfg, 0, sizeof(cfg));
  memset(&io, 0, sizeof(io));
  memset(&res, 0, sizeof(res));
  io.ctx = NULL;
  io.puts = stdout_puts;

  /* THE POINT OF THE PER-VENDOR RUN: the cap this vendor's index row declares
   * must be a cap the library's reassembly checks actually pass at. */
  cfg.frame_bytes = HML_VENDOR_FRAME_BYTES;
  rc = hml_mcu_demo_run(&cfg, &io, &res);

  printf("host-selftest vendor=%s frame_bytes=%lu checks=%lu failures=%lu rc=%d\n",
         HML_VENDOR_NAME, (unsigned long)HML_VENDOR_FRAME_BYTES, (unsigned long)res.checks,
         (unsigned long)res.failures, rc);

  check(rc == 0, "the shared demo body must report no failed check at this cap");
  check(res.checks >= 20u, "too few checks ran; the demo body is probably truncated");
  check(res.failures == 0u, "the demo body reported failed checks at this cap");

  if (g_fail) {
    printf("host-selftest FAILED for %s. That is a HOST result; the real per-vendor\n",
           HML_VENDOR_NAME);
    printf("      result is renode/vendors/%s/%s.robot, which has never run.\n",
           HML_VENDOR_NAME, HML_VENDOR_NAME);
    return 1;
  }
  printf("host-selftest OK for %s (host build only; the per-vendor run is "
         "%s_renode_smoke).\n",
         HML_VENDOR_NAME, HML_VENDOR_NAME);
  return 0;
}
