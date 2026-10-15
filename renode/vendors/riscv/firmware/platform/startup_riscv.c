/*
 * startup_riscv.c -- reset, heap, console, tick and demo task for the riscv
 * vendor platform (GD32VF103-style RV32IMAC).
 *
 * WHAT IS REAL HERE, and what is shared
 *   SHARED (compiled unchanged, from renode/firmware/freertos_demo/):
 *     - app/hml_mcu_demo.c        the whole check body, and the only thing that
 *                                 decides whether this run passes
 *     - stub_freertos/FreeRTOS.h  the NOT-FreeRTOS task/queue API declarations
 *     - the library's own portable C from src/
 *   PER-VENDOR (this file, board_config.h, the linker script):
 *     - everything else. RISC-V is the row where the shared stub DOES NOT link:
 *       its tick is the ARMv7-M SysTick COUNTFLAG, and RISC-V has no SysTick.
 *       This file therefore implements the whole FreeRTOS.shim API surface the
 *       image actually uses -- xTaskGetTickCount, xTaskCreate,
 *       vTaskStartScheduler, vTaskDelay, taskYIELD, hml_stub_elapsed_ms,
 *       hml_stub_systick_stalled -- ticked instead by the MMIO machine timer
 *       (CLINT mtime). The queue half of the shared header (xQueueCreate and
 *       friends) is declared but never called by anything in this image, so it
 *       is deliberately not implemented here: vendoring two dozen dead symbols
 *       nobody can observe would only invite claims of coverage.
 *
 * WHAT IS NOT REAL HERE
 *   - There is no FreeRTOS kernel. The task API is a cooperative shim written
 *     in this file; read stub_freertos/FreeRTOS.h before quoting anything about
 *     RTOS behaviour. This file must never be called a FreeRTOS port.
 *   - There is no clock tree. clock_init() below is a documented no-op, because
 *     Renode does not model this part's clock-control registers and poking
 *     unmodelled registers is only a way to fail. On real silicon this is where
 *     the RCU PLL configuration, the flash wait states and the pin mux belong.
 *   - No interrupt is ever enabled or handled, and no PLIC or NVIC is wired in
 *     riscv.resc at all. The uart-irq printed by the identity block is the
 *     literal 0 from board_config.h and is never delivered.
 *   - _sbrk is a monotonic bump allocator with no guard page, no accounting and
 *     no stack check. newlib's malloc/free handle a monotonic heap correctly, so
 *     free() works, and that is all it is claimed to do.
 *
 * THE TICK, AND WHY THIS FILE OWNS IT
 *   The shared stub's tick is SysTick COUNTFLAG (ARMv7-M only). Nothing ARM
 *   exists on this row, so this file counts forward motion of the CLINT machine
 *   timer instead: mtime is a 64-bit counter at the Renode RiscVClint's standard
 *   offsets (low word at +0xBFF8, high word at +0xBFFC, base 0x02000000 -- flag
 *   the riscv.resc model if that layout does not match your Renode release).
 *   Each detected forward motion of mtime is one tick, mirroring COUNTFLAG's
 *   "a reload happened" semantics rather than pretending to be a wall clock. If
 *   mtime never advances (a model that does not clock the timer), wait_ticks()
 *   falls back to a bounded spin budget and sets hml_stub_systick_stalled():
 *   the demo prints that as a warning, and a broken platform model is visible
 *   instead of an infinite hang. No measurement of emulated time may ever be
 *   quoted as anything but "tick resolution only".
 *
 * RESET IS FIRMWARE, NOT A VECTOR TABLE
 *   A RISC-V core starts at the address the RESET device sends it to. Renode's
 *   default brings it to the start of flash (address 0, matching this board),
 *   where RESET_VECTOR and .text.init are pinned by the linker script. mtvec is
 *   then reprogrammed by Reset_Handler to this file's trap entry; a trap
 *   thereafter lands in riscv_mtvec_entry(), which spins on purpose -- the
 *   visible dead hang that turns a firmware bug into a fixed address instead of
 *   executing garbage. Nothing in this harness traps, so the entry is
 *   defensive, documented as such, and not asserted by the robot test.
 *
 * STATUS -- READ BEFORE TRUSTING ANY OF THIS
 *   This file has NEVER BEEN COMPILED. The machine it was written on has no
 *   riscv32 toolchain, no Renode, no mono and no dotnet, and no GD32VF103 data
 *   sheet was available. Every constant leans on a part's layout and an
 *   emulated register block that has not been checked against anything. If the
 *   platform does not boot, the fix belongs here and in riscv.resc, and
 *   run_vendor_smoke.sh reports that as a FAILURE, never as a skip.
 */

#include "FreeRTOS.h" /* the STUB header, not the kernel */
#include "board_config.h"
#include "hml_mcu_demo.h"

#include "hml/hml_port.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Linker-provided symbols (firmware/platform/gcc_riscv_hml_mcu.ld)          */
/* ------------------------------------------------------------------------- */
extern uint32_t __data_load__;
extern uint32_t __data_start__;
extern uint32_t __data_end__;
extern uint32_t __bss_start__;
extern uint32_t __bss_end__;
extern uint32_t __stack_top__;
extern uint32_t __heap_start__;
extern uint32_t __heap_end__;

int main(void);
void Reset_Handler(void);

/* ------------------------------------------------------------------------- */
/* Console: UART0 @ 0x40000000.                                               */
/*                                                                          */
/* THE BASE IS THE PART'S. THE REGISTER BLOCK IS NOT.                         */
/* No GD32VF103 USART model that this harness has ever seen exists to it, and   */
/* Renode is not installed anywhere this matrix was written, so this file      */
/* drives UART.NS16550 -- the same decision, and the same caveat, as the        */
/* microchip_sam and nxp_lpc rows -- because an image whose register writes do  */
/* not match the model it is running on cannot print a character at all. The    */
/* real USART lacks the 5-bit LSR/THR 16550 block entirely. To make this run   */
/* on real silicon, change BOTH halves: these constants and the model name in  */
/* riscv.resc. Changing only one makes a timeout the only outcome.             */
/* ------------------------------------------------------------------------- */
#define HML_UART_THR (*(volatile uint8_t *)(HML_VENDOR_UART_ADDR + 0x00u)) /* write = transmit */
#define HML_UART_LSR (*(volatile uint8_t *)(HML_VENDOR_UART_ADDR + 0x05u))
#define HML_UART_LSR_THRE 0x20u
#define HML_UART_TXE_SPINS 100000u /* bounded: a stuck THRE must not hang the image */

static void uart_putc(char c) {
  /* Poll THRE, but only for a bounded number of spins. If the model never
   * raises THRE the byte is still written: a lost observability byte is a far
   * better failure mode than an image that never reaches its DONE line. */
  uint32_t spins = 0u;
  while ((HML_UART_LSR & HML_UART_LSR_THRE) == 0u && spins < HML_UART_TXE_SPINS) spins++;
  HML_UART_THR = (uint8_t)c;
}

static void uart_puts(void *ctx, const char *line) {
  (void)ctx;
  size_t n = 0u;
  for (; line[n] != '\0'; n++) {
    uart_putc(line[n]);
    if (line[n] == '\n') uart_putc('\r');
  }
  if (n == 0u || line[n - 1u] != '\n') {
    uart_putc('\n');
    uart_putc('\r');
  }
}

static void uart_kv(const char *key, uint32_t value) {
  char buf[12];
  unsigned k = 0u;
  if (value == 0u) buf[k++] = '0';
  while (value != 0u && k < sizeof(buf)) {
    buf[k++] = (char)('0' + (value % 10u));
    value /= 10u;
  }
  for (const char *s = key; *s != '\0'; s++) uart_putc(*s);
  for (unsigned i = 0; i < k; i++) uart_putc(buf[k - 1u - i]);
  uart_putc('\n');
}

static void uart_hex32(const char *key, uint32_t value) {
  static const char k_hex[] = "0123456789abcdef";
  for (const char *s = key; *s != '\0'; s++) uart_putc(*s);
  for (int i = 28; i >= 0; i -= 4) uart_putc(k_hex[(value >> (unsigned)i) & 0xfu]);
  uart_putc('\n');
}

/* ------------------------------------------------------------------------- */
/* Reset and trap entry. See the header comment: mtvec is firmware text, and   */
/* a trap lands in a visible dead hang by design.                             */
/*                                                                          */
/* The .option push/norelax/pop is a belt-and-braces double with the          */
/* -Wl,--no-relax flag in firmware/vendor.env: the linker script pins          */
/* RESET_VECTOR and .text.init to the start of flash, and relaxation moving    */
/* instructions around after the section assignment would undo that pinning.   */
/* ------------------------------------------------------------------------- */
__attribute__((section(".text.init"), naked, noreturn))
void Reset_Handler(void) {
  __asm__ volatile(
      "    .option push\n"
      "    .option norelax\n"
      "    la   sp, __stack_top__\n"     /* stack from the linker's .stack region */
      "    la   t0, riscv_mtvec_entry\n"
      "    csrw mtvec, t0\n"             /* traps -> the visible dead hang below */
      "    .option pop\n"
      "    tail _start_c\n");
}

__attribute__((section(".riscv_trap_vector"), naked, noreturn))
void riscv_mtvec_entry(void) {
  __asm__ volatile("1: j 1b"); /* infinite loop: a firmware bug becomes a fixed-address hang */
}

static void _start_c(void) __attribute__((noreturn));

static void _start_c(void) {
  uint32_t *src = &__data_load__;
  uint32_t *dst = &__data_start__;

  for (; dst < &__data_end__;) *dst++ = *src++; /* .data: flash -> SRAM */
  for (dst = &__bss_start__; dst < &__bss_end__;) *dst++ = 0u;

  main();
  for (;;) {
  }
}

/* ------------------------------------------------------------------------- */
/* Vendor bring-up hooks. BOTH ARE DOCUMENTED NO-OPS, and that is deliberate.  */
/* ------------------------------------------------------------------------- */
static void clock_init(void) {
  /* Real silicon: RCU PLL configuration, the AHB/APB prescalers, the flash
   * wait states for the 0x00000000 window, and the pin mux. Renode models none
   * of it, so there is nothing that could be checked here and a wrong write is
   * a pointless way to fail. This function exists so the seam is obvious, not
   * so it can do something. */
}

static void uart_init(void) {
  /* Real silicon: USART_CTL0 UEN, the word length in CTL0, the parity in CTL0,
   * the baud divisor in BAUD. The emulated console is a character sink: it
   * needs none of that, and the data register is the only register this harness
   * actually depends on. */
}

/* ------------------------------------------------------------------------- */
/* CLINT machine timer. The tick source for the re-implemented stub. See the   */
/* header comment for what "a tick" means and what it does not claim.          */
/* ------------------------------------------------------------------------- */
#define HML_CLINT_BASE 0x02000000u
#define HML_CLINT_MTIME_LO (*(volatile uint32_t *)(HML_CLINT_BASE + 0xBFF8u))
#define HML_CLINT_MTIME_HI (*(volatile uint32_t *)(HML_CLINT_BASE + 0xBFFCu))

/* Bounded fallback so nothing can spin forever. Mirrors the ARM stub's budget
 * wording: at nominal 1 ms per tick this is a generous multi-second budget
 * before the tick source is declared stalled. */
#define HML_STUB_SPIN_BUDGET 20000000u

#define HML_STUB_MAX_TASKS 4u

struct stub_task {
  TaskFunction_t fn;
  void *arg;
  const char *name;
  UBaseType_t prio; /* stored, then ignored: the shim has no priorities */
  UBaseType_t stack_words;
  bool live;
};

static struct stub_task g_tasks[HML_STUB_MAX_TASKS];
static TickType_t g_ticks;
static int g_systick_stalled;
static uint64_t g_last_mtime;

/* The 64-bit MMIO read with the standard break-torn-read retry. Lo=low word,
 * then high word, then low again: if the two low reads differ, the 32-bit half
 * straddled a carry and the whole read is retried. */
static uint64_t riscv_mtime_read(void) {
  uint32_t hi0, lo, hi1;
  do {
    hi0 = HML_CLINT_MTIME_HI;
    lo = HML_CLINT_MTIME_LO;
    hi1 = HML_CLINT_MTIME_HI;
  } while (hi0 != hi1);
  return ((uint64_t)hi0 << 32u) | (uint64_t)lo;
}

/* Advance the tick by up to `want` mtime edges, or give up. */
static void wait_ticks(TickType_t want) {
  if (want == 0u) return;
  if (want > 1000u) want = 1000u; /* no demo needs more; keeps a bad call sane */

  for (TickType_t i = 0; i < want; i++) {
    uint32_t spins = 0u;
    uint64_t now = riscv_mtime_read();
    while (now == g_last_mtime) {
      if (++spins > HML_STUB_SPIN_BUDGET) {
        g_systick_stalled = 1;
        g_last_mtime = now; /* count the tick anyway: never hang, always make progress */
        g_ticks++;
        return;
      }
      now = riscv_mtime_read();
    }
    g_last_mtime = now;
    g_ticks++;
  }
}

uint32_t hml_stub_elapsed_ms(void) { return (uint32_t)(g_ticks * portTICK_PERIOD_MS); }

int hml_stub_systick_stalled(void) { return g_systick_stalled; }

TickType_t xTaskGetTickCount(void) { return g_ticks; }

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, UBaseType_t stack_words, void *arg,
                      UBaseType_t prio, TaskHandle_t *out) {
  for (unsigned i = 0; i < HML_STUB_MAX_TASKS; i++) {
    if (g_tasks[i].live) continue;
    g_tasks[i].fn = fn;
    g_tasks[i].name = name;
    g_tasks[i].arg = arg;
    g_tasks[i].prio = prio;
    g_tasks[i].stack_words = stack_words;
    g_tasks[i].live = true;
    if (out != NULL) *out = &g_tasks[i];
    return pdPASS;
  }
  if (out != NULL) *out = NULL;
  return pdFAIL; /* no dynamic allocation: the table is static and bounded */
}

void vTaskDelay(TickType_t ticks) { wait_ticks(ticks); }

void taskYIELD(void) { wait_ticks(1u); }

void vTaskStartScheduler(void) {
  for (;;) {
    bool ran_any = false;
    for (unsigned i = 0; i < HML_STUB_MAX_TASKS; i++) {
      if (!g_tasks[i].live || g_tasks[i].fn == NULL) continue;
      ran_any = true;
      g_tasks[i].fn(g_tasks[i].arg);
      /* The real kernel deletes a task that returns; so does this shim. */
      g_tasks[i].live = false;
    }
    if (!ran_any) wait_ticks(10u);
  }
}

/* ------------------------------------------------------------------------- */
/* hml_clock_us -- the frozen port contract, at 1 ms resolution exactly as    */
/* documented for a tick-based MCU port in ports/freertos_lwip. On this row    */
/* "ms" means "mtime edges counted as ticks", for the same reason the ARM      */
/* stub's ms means SysTick COUNTFLAG edges: tick resolution only.             */
/* ------------------------------------------------------------------------- */
uint64_t hml_clock_us(void) {
  return ((uint64_t)xTaskGetTickCount() * 1000000ull) / ((uint64_t)portTICK_PERIOD_MS * 1000ull);
}

/* ------------------------------------------------------------------------- */
/* newlib heap: a monotonic bump allocator over the linker-defined region.     */
/* ------------------------------------------------------------------------- */
void *_sbrk(int incr) {
  static uint32_t *brk = NULL;
  uint32_t *prev;

  if (brk == NULL) brk = &__heap_start__;
  prev = brk;
  if (incr < 0) {
    if (brk + incr < &__heap_start__) return (void *)-1; /* never shrink below the start */
    brk += incr;
  } else {
    if (brk + (incr / (int)sizeof(uint32_t)) >= &__heap_end__) return (void *)-1;
    brk += incr / (int)sizeof(uint32_t);
  }
  return (void *)prev;
}

/* ------------------------------------------------------------------------- */
/* THE ONE THING TO KNOW ABOUT THE SHARED DEMO BODY                          */
/*   app/hml_mcu_demo.c is compiled unchanged from the single-platform harness, */
/*   so two of the strings it prints are base-platform text:                   */
/*     "image=hml_mcu_demo.elf"       -- wrong name on this board             */
/*     "the heap is in SDRAM by linker script" -- wrong on this board, which  */
/*                                          has no SDRAM at all               */
/*   Renaming those would mean copying or editing the shared body, which is   */
/*   exactly what sharing it is meant to prevent. So this board CORRECTS them  */
/*   from the outside: main() prints its own identity line first, using the    */
/*   real artefact name and this board's real memory map. Where the two lines  */
/*   disagree, this one is right.                                             */
/* ------------------------------------------------------------------------- */
static void print_board_identity(void) {
  uart_puts(NULL, HML_MCU_DEMO_MARK_NOTE " vendor=" HML_VENDOR_NAME " arch=" HML_VENDOR_ARCH);
  uart_puts(NULL, HML_MCU_DEMO_MARK_NOTE " image=" HML_VENDOR_ELF_NAME
                                         " (the shared banner's hml_mcu_demo.elf name is"
                                         " base-platform text; on this board it is wrong)");
  uart_puts(NULL, HML_MCU_DEMO_MARK_NOTE " this board has no external SDRAM: the shared"
                                         " banner line saying SDRAM is base-platform text");
  uart_puts(NULL, HML_MCU_DEMO_MARK_NOTE " riscv has no vector table: mtvec is firmware text"
                                         " and a trap lands in a visible dead hang");
  uart_puts(NULL, HML_MCU_DEMO_MARK_NOTE " this is the mtime row: the tick is the mmio clint"
                                         " machine timer, implemented in startup_riscv.c, not"
                                         " the arm systick the shared stub uses");
  uart_kv(HML_MCU_DEMO_MARK_NOTE " heap-bytes=", (uint32_t)HML_VENDOR_HEAP_BYTES);
  uart_kv(HML_MCU_DEMO_MARK_NOTE " frame-bytes-default=", (uint32_t)HML_VENDOR_FRAME_BYTES);
  uart_hex32(HML_MCU_DEMO_MARK_NOTE " sram-base=", HML_VENDOR_SRAM_ADDR);
  uart_kv(HML_MCU_DEMO_MARK_NOTE " sram-bytes=", (uint32_t)HML_VENDOR_SRAM_BYTES);
  uart_hex32(HML_MCU_DEMO_MARK_NOTE " flash-base=", HML_VENDOR_FLASH_ADDR);
  uart_kv(HML_MCU_DEMO_MARK_NOTE " flash-bytes=", (uint32_t)HML_VENDOR_FLASH_BYTES);
  uart_kv(HML_MCU_DEMO_MARK_NOTE " uart-irq=", (uint32_t)HML_VENDOR_UART_IRQ);
  uart_puts(NULL, HML_MCU_DEMO_MARK_NOTE " no interrupt is enabled or taken, and no nvic or"
                                         " plic is wired: the uart-irq above is fictional and"
                                         " never delivered");
  uart_puts(NULL, HML_MCU_DEMO_MARK_NOTE " clock and uart init are documented no-ops: Renode"
                                         " models neither the rcu nor a baud divisor");
}

/* ------------------------------------------------------------------------- */
/* The demo task. It runs to completion inside vTaskStartScheduler(), after    */
/* which the shim scheduler idles -- where the image spends the rest of its     */
/* emulated life. Renode keeps running that idle loop until the robot test has  */
/* seen the DONE line.                                                        */
/* ------------------------------------------------------------------------- */
static void demo_pause(void *ctx, uint32_t ms) {
  (void)ctx;
  vTaskDelay(pdMS_TO_TICKS(ms));
}

static void demo_task(void *arg) {
  hml_mcu_demo_cfg cfg;
  hml_mcu_demo_io io;
  hml_mcu_demo_result res;
  int rc;

  (void)arg;
  memset(&cfg, 0, sizeof(cfg));
  memset(&io, 0, sizeof(io));
  memset(&res, 0, sizeof(res));
  cfg.pause = demo_pause;
  cfg.frame_bytes = HML_VENDOR_FRAME_BYTES;
  io.ctx = NULL;
  io.puts = uart_puts;

  rc = hml_mcu_demo_run(&cfg, &io, &res);
  uart_kv(HML_MCU_DEMO_MARK_TASK " rc=", (uint32_t)(rc < 0 ? 1u : (uint32_t)rc));

  if (hml_stub_systick_stalled()) {
    /* A platform model that never drives the CLINT mtime. Reported, never
     * fatal: the bounded spin fallback already kept the image out of a hang. */
    const char *m = HML_MCU_DEMO_MARK_WARN
                    " mtime-did-not-advance pauses-fell-back-to-a-spin-budget";
    uart_puts(NULL, m);
  }
  uart_kv("HML-MCU-DEMO-TICKS ms=", hml_stub_elapsed_ms());
}

int main(void) {
  TaskHandle_t t = NULL;
  /* One round of tick so wait_ticks() has a sane baseline: mtime at the very
   * first read could lag the reset value by less than one tick. */
  g_last_mtime = riscv_mtime_read();
  clock_init();
  uart_init();
  print_board_identity();
  if (xTaskCreate(demo_task, "hml_demo", 512u, NULL, 1u, &t) != pdPASS) {
    const char *m = HML_MCU_DEMO_MARK_FAIL " the stub task table has no free slot";
    uart_puts(NULL, m);
    for (;;) {
    }
  }
  vTaskStartScheduler(); /* never returns */
  for (;;) {
  }
}