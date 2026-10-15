/*
 * startup_stub_rtos.c -- the Cortex-M entry point for the Renode smoke demo.
 *
 * WHAT IS REAL HERE
 *   - the reset vector, the .data copy and the .bss zero, all against the
 *     symbols exported by platform/gcc_arm_hml_mcu.ld;
 *   - the UART data-register write that Renode's UART.STM32_UART model turns
 *     into a character on the terminal the robot test watches;
 *   - the SysTick read that drives the stub RTOS tick (see stub_freertos/);
 *   - hml_clock_us(), part of the frozen port contract: hml_reasm.c calls it
 *     for the EOF-latency tap, so a real definition must exist in the image;
 *   - _sbrk, without which newlib's malloc cannot work and hml_reasm_open()
 *     would fail: the demo's reassembly buffers come from that heap.
 *
 * WHAT IS NOT REAL HERE
 *   - There is no FreeRTOS kernel. The task API here is the cooperative stub
 *     in ../stub_freertos -- read that directory's FreeRTOS.h before quoting
 *     anything about RTOS behaviour. This file must never be described as a
 *     FreeRTOS port.
 *   - There is no clock tree, no PLL, and no interrupt is ever enabled or
 *     handled. The vector table holds two entries (initial stack, reset) and
 *     nothing else, so a fault lands on whatever follows in flash. The demo is
 *     written not to fault and the robot test fails if it does.
 *   - _sbrk is a bump allocator with no protection. newlib's malloc/free
 *     handle a monotonic heap correctly, so free() works, but there is no
 *     fragmentation accounting, no growth limit and no stack guard.
 *   - The heap is placed in the emulated SDRAM window by the linker script.
 *     On real silicon that window is an external SDRAM chip behind an FMC,
 *     and bringing it up is board work this file does not do.
 */

#include "FreeRTOS.h" /* the STUB header, not the kernel */
#include "hml_mcu_demo.h"

#include "hml/hml_port.h"

#include <string.h>

/* Frame cap handed to hml_reasm_open(). 0 keeps the library default of
 * HML_MAX_FRAME_SIZE (1 MiB), which costs TWO 1 MiB buffers -- the reason the
 * linker script puts the heap in the SDRAM window. build_mcu.sh --frame-bytes
 * sets it. See docs/renode.md section 3. */
#ifndef HML_MCU_FRAME_BYTES
#define HML_MCU_FRAME_BYTES 0
#endif

/* ------------------------------------------------------------------------- */
/* Linker-provided symbols (platform/gcc_arm_hml_mcu.ld)                       */
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
/* UART: USART2 registers, at the address modelled in the renode .resc files  */
/* ------------------------------------------------------------------------- */
#define HML_UART_BASE 0x40004400u
#define HML_UART_SR (*(volatile uint32_t *)(HML_UART_BASE + 0x00u))
#define HML_UART_DR (*(volatile uint32_t *)(HML_UART_BASE + 0x04u))
#define HML_UART_CR1 (*(volatile uint32_t *)(HML_UART_BASE + 0x0Cu))
#define HML_UART_SR_TXE 0x0080u
#define HML_UART_CR1_UE 0x2000u /* USART_CR1.UE: the peripheral ignores DR until this is set */
#define HML_UART_CR1_TE 0x0008u /* USART_CR1.TE: transmitter enable */
#define HML_UART_TXE_SPINS 100000u /* bounded: a stuck TXE must not hang the image */

static void uart_init(void) {
  /* USART_CR1.UE|USART_CR1.TE. Renode's UART.STM32_UART drops every byte written
   * to DR while UE is clear -- "Trying to transmit a character, but the
   * transmitter is not enabled" -- so this is a real register write, not a
   * cosmetic one: it is also exactly what real silicon needs. The word length,
   * parity and BRR baud divisor are deliberately left alone: the emulated
   * console has no line rate to match, and nothing here could check them. */
  HML_UART_CR1 = HML_UART_CR1_UE | HML_UART_CR1_TE;
}

static void uart_putc(char c) {
  /* Poll TXE, but only for a bounded number of spins. If the model never
   * raises TXE the byte is still written: a lost observability byte is a far
   * better failure mode than an image that never reaches its DONE line. */
  uint32_t spins = 0u;
  while ((HML_UART_SR & HML_UART_SR_TXE) == 0u && spins < HML_UART_TXE_SPINS) spins++;
  HML_UART_DR = (uint32_t)(unsigned char)c;
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
/* The demo task. It runs to completion inside vTaskStartScheduler(), after    */
/* which the stub scheduler idles -- where the image spends the rest of its    */
/* emulated life. Renode keeps running that idle loop until the robot test has */
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
  cfg.frame_bytes = (uint32_t)HML_MCU_FRAME_BYTES;
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
  uart_init();
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
