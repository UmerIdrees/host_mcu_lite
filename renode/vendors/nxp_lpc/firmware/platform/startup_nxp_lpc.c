/*
 * startup_nxp_lpc.c -- reset, heap, console and demo task for the nxp_lpc vendor
 * platform (LPC4337-style Cortex-M4).
 *
 * WHAT IS REAL HERE, and what is shared
 *   SHARED (compiled unchanged for all four ARM vendors, from
 *   renode/firmware/freertos_demo/):
 *     - app/hml_mcu_demo.c        the whole check body, and the only thing that
 *                                 decides whether this run passes
 *     - stub_freertos/FreeRTOS.h  the NOT-FreeRTOS task/queue API declarations
 *     - stub_freertos/freertos_stub.c  its cooperative implementation, whose
 *                                 tick is the ARMv7-M SysTick COUNTFLAG
 *     - the library's own portable C from src/
 *   PER-VENDOR (this file, board_config.h, the linker script):
 *     - the reset vector, the .data copy and the .bss zero
 *     - _sbrk, without which newlib's malloc cannot work and hml_reasm_open()
 *       cannot allocate the reassembly buffers
 *     - the UART data-register write that Renode's UART.NS16550 turns into a
 *       character on the terminal the robot test watches
 *     - the banner-correction line below (see THE ONE THING TO KNOW)
 *
 * READ THIS BEFORE THE UART SECTION: IT IS THE INTERESTING CAVEAT ON THIS BOARD
 *   The LPC43xx USART does NOT have the register block written below. This file
 *   drives UART.NS16550, because that is the console nxp_lpc.resc asks Renode to
 *   model, and an image whose register writes do not match the model it is
 *   running on cannot print a single character ever. Writing real LPC USART
 *   offsets against an NS16550 model would produce a platform whose only
 *   possible outcome is a timeout.
 *   So the BASE ADDRESS IS THE PART'S (USART0, 0x400C1000, per UM10539) and the
 *   OFFSETS ARE NOT. To make this run on real silicon: replace the three
 *   constants in the UART section below with UM10539's USART_STAT / USART_DATA
 *   offsets and its TXRDY bit, and change the model name in nxp_lpc.resc to
 *   match. Both halves are marked. Neither has been checked against a data
 *   sheet or a Renode release, because none was reachable from the machine this
 *   was written on.
 *
 * WHAT IS NOT REAL HERE
 *   - There is no FreeRTOS kernel. The task API is the cooperative stub; read
 *     renode/firmware/freertos_demo/stub_freertos/FreeRTOS.h before quoting
 *     anything about RTOS behaviour. This file must never be called a FreeRTOS
 *     port.
 *   - There is no clock tree. clock_init() below is a documented no-op, because
 *     Renode models neither this part's clock-control block nor the SPI/QSPI
 *     flash controller this image actually executes out of. On real silicon this
 *     is where the PLL/AHB setup, the USART0 clock divider, the pin mux and the
 *     flash controller configuration belong.
 *   - No interrupt is ever enabled or handled. uart_init() is a documented no-op
 *     for the same reason: the emulated console needs no baud divisor. The
 *     USART0_IRQn value in board_config.h is decoration, deliberately.
 *   - _sbrk is a monotonic bump allocator with no guard page, no accounting and
 *     no stack check. newlib's malloc/free handle a monotonic heap correctly, so
 *     free() works, and that is all it is claimed to do.
 *
 * STATUS -- READ BEFORE TRUSTING ANY OF THIS
 *   This file has NEVER BEEN COMPILED. The machine it was written on has no
 *   arm-none-eabi-gcc, no Renode, no mono and no dotnet. If the platform does
 *   not boot, the fix belongs here and in nxp_lpc.resc, and run_vendor_smoke.sh
 *   reports that as a FAILURE, never as a skip.
 */

#include "FreeRTOS.h" /* the STUB header, not the kernel */
#include "board_config.h"
#include "hml_mcu_demo.h"

#include "hml/hml_port.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Linker-provided symbols (firmware/platform/gcc_nxp_lpc_hml_mcu.ld)          */
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
/* Console: USART0 @ 0x400C1000.                                             */
/*                                                                          */
/* THE BASE IS THE PART'S. THE REGISTER BLOCK IS NOT -- see the file header.  */
/* These three constants describe the emulated model, not the LPC43xx USART.  */
/* To target the silicon: swap in UM10539's USART_STAT / USART_DATA offsets   */
/* and its TXRDY bit, and change the model name in nxp_lpc.resc to match.     */
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
/* Vendor bring-up hooks. BOTH ARE DOCUMENTED NO-OPS, and that is deliberate.  */
/* ------------------------------------------------------------------------- */
static void clock_init(void) {
  /* Real silicon: the clock-control block (PLL / AHB / PLL1), the USART0 clock
   * divider, the pin mux and -- the step with no analogue on any other board in
   * this matrix -- the SPI/QSPI flash controller, because this image executes
   * in place out of 0x1A000000 and there is no boot copy. Renode models none of
   * it, so there is nothing to check and a wrong write is a pointless way to
   * fail. This function exists so the seam is obvious, not so it can do
   * something. */
}

static void uart_init(void) {
  /* Real silicon: UE, the word length, the parity, the divisor. The emulated
   * console is a character sink: it needs none of that, and the data register
   * is the only register this harness actually depends on. */
}

/* ------------------------------------------------------------------------- */
/* hml_clock_us -- the frozen port contract, at 1 ms resolution exactly as    */
/* documented for a tick-based MCU port in ports/freertos_lwip.               */
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
  uart_kv(HML_MCU_DEMO_MARK_NOTE " heap-bytes=", (uint32_t)HML_VENDOR_HEAP_BYTES);
  uart_kv(HML_MCU_DEMO_MARK_NOTE " frame-bytes-default=", (uint32_t)HML_VENDOR_FRAME_BYTES);
  uart_hex32(HML_MCU_DEMO_MARK_NOTE " sram-base=", HML_VENDOR_SRAM_ADDR);
  uart_kv(HML_MCU_DEMO_MARK_NOTE " sram-bytes=", (uint32_t)HML_VENDOR_SRAM_BYTES);
  uart_hex32(HML_MCU_DEMO_MARK_NOTE " flash-base=", HML_VENDOR_FLASH_ADDR);
  uart_kv(HML_MCU_DEMO_MARK_NOTE " flash-bytes=", (uint32_t)HML_VENDOR_FLASH_BYTES);
  uart_kv(HML_MCU_DEMO_MARK_NOTE " uart-irq=", (uint32_t)HML_VENDOR_UART_IRQ);
  uart_puts(NULL, HML_MCU_DEMO_MARK_NOTE " no interrupt is enabled or taken on this board: the"
                                         " irq above is documentation, not wiring");
  uart_puts(NULL, HML_MCU_DEMO_MARK_NOTE " the uart base address is the LPC USART0, but the"
                                         " register block is the emulated model's, NOT the"
                                         " LPC USART map");
  uart_puts(NULL, HML_MCU_DEMO_MARK_NOTE " clock and uart init are documented no-ops: Renode"
                                         " models neither the clock block nor a baud divisor");
}

/* ------------------------------------------------------------------------- */
/* The demo task. It runs to completion inside vTaskStartScheduler(), after    */
/* which the stub scheduler idles -- where the image spends the rest of its     */
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
    /* A platform model that never drives SysTick. Reported, never fatal: the
     * stub's bounded fallback already kept the image out of a hang. */
    const char *m = HML_MCU_DEMO_MARK_WARN
                    " systick-did-not-advance pauses-fell-back-to-a-spin-budget";
    uart_puts(NULL, m);
  }
  uart_kv("HML-MCU-DEMO-TICKS ms=", hml_stub_elapsed_ms());
}

int main(void) {
  TaskHandle_t t = NULL;
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

/* ------------------------------------------------------------------------- */
/* Reset handler. ARMv7-M loads SP from the first vector word, so the vector    */
/* table is a plain array and the linker script pins it to the start of flash. */
/* ------------------------------------------------------------------------- */
__attribute__((section(".isr_vector"), used))
void (*const g_vectors[])(void) = {
    (void (*)(void))(uintptr_t)&__stack_top__,
    Reset_Handler,
};

void Reset_Handler(void) {
  uint32_t *src = &__data_load__;
  uint32_t *dst = &__data_start__;

  for (; dst < &__data_end__;) *dst++ = *src++; /* .data: flash -> SRAM */
  for (dst = &__bss_start__; dst < &__bss_end__;) *dst++ = 0u;

  main();
  for (;;) {
  }
}
