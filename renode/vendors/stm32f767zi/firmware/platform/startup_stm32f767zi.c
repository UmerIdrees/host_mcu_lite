/*
 * startup_stm32f767zi.c -- reset, cache/FPU bring-up, heap, console and demo
 * task for the stm32f767zi vendor platform (STM32F767ZI Nucleo, Cortex-M7).
 *
 * WHAT IS REAL HERE, and what is shared
 *   SHARED (compiled unchanged from renode/firmware/freertos_demo/):
 *     - app/hml_mcu_demo.c        the whole check body, and the only thing that
 *                                 decides whether this run passes
 *     - stub_freertos/FreeRTOS.h  the NOT-FreeRTOS task/queue API declarations
 *     - stub_freertos/freertos_stub.c  its cooperative implementation, whose
 *                                 tick is the ARMv7-M SysTick COUNTFLAG
 *     - the library's own portable C from src/
 *   PER-VENDOR (this file, board_config.h, the linker script):
 *     - the reset vector, the .data copy and the .bss zero
 *     - the Cortex-M7 specific bring-up below (FPU via CPACR, caches via
 *       SCB), which the Cortex-M4 vendors do not need
 *     - _sbrk, without which newlib's malloc cannot work
 *     - the USART data-register write that turns into a character on the
 *       terminal the robot test watches
 *     - the banner-correction lines (see THE ONE THING TO KNOW)
 *
 * WHAT IS NOT REAL HERE
 *   - There is no FreeRTOS kernel. The task API is the cooperative stub; read
 *     renode/firmware/freertos_demo/stub_freertos/FreeRTOS.h before quoting
 *     anything about RTOS behaviour. This file must never be called a FreeRTOS
 *     port.
 *   - There is no clock tree. clock_init() is a documented no-op, because
 *     Renode does not model this part's RCC and poking unmodelled registers is
 *     only a way to fail. On real silicon this is where RCC/PLL configuration,
 *     the PCLK enables and the pin muxes belong -- and the MAC's
 *     ETH_CLOCK_ENABLE and the SYSCFG ETH_MII_RMII_SEL bit in particular, both
 *     of which the PHY-facing example needs.
 *   - No interrupt is ever enabled or handled. uart_init() only sets the two
 *     bits the emulated model gates transmission on. HML_VENDOR_ETH_IRQ is
 *     decoration, like HML_VENDOR_UART_IRQ: the MAC shim polls its descriptor
 *     ring rather than taking ETH_IRQn.
 *   - _sbrk is a monotonic bump allocator with no guard page, no accounting and
 *     no stack check. newlib's malloc/free handle a monotonic heap correctly,
 *     so free() works, and that is all it is claimed to do.
 *
 * THE CACHE QUESTION, STATED HONESTLY
 *   A Cortex-M7 caches both instruction fetches and data, and this image is
 *   linked -mcpu=cortex-m7, so the caches ARE enabled below. That is what real
 *   silicon needs and it is what makes the FPU usable. It also means a real
 *   MAC DMA descriptor ring would have to be coherent with the data cache, and
 *   on a real part that is a cache-maintenance discipline (or MPU-marked
 *   non-cacheable memory) that this file does NOT implement. Renode's SCB is
 *   modelled, so the enable works here, but nothing in this file makes a
 *   statement about coherency. See the network example's README for what that
 *   would require on silicon.
 */

#include "FreeRTOS.h" /* the STUB header, not the kernel */
#include "board_config.h"
#include "hml_mcu_demo.h"

#include "hml/hml_port.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Linker-provided symbols (firmware/platform/gcc_stm32f767zi_hml_mcu.ld)      */
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

/*
 * Which demo body this image runs. 0 is the codec body, which is the default and
 * is what stm32f767zi_hml_mcu_demo.elf and its passing robot test use. 1 is the
 * network body. The switch exists so the two are separate ELFs built from one
 * startup file, not one ELF that decides at run time -- see main().
 *
 * HML_VENDOR_APP_NETWORK is also what build_vendor.sh keys off to pick
 * hml_mcu_net_demo.c over hml_mcu_demo.c, so the macro and the source list cannot
 * disagree.
 */
#ifndef HML_VENDOR_APP_NETWORK
#define HML_VENDOR_APP_NETWORK 0
#endif

#if HML_VENDOR_APP_NETWORK
/* The network demo body, in app/hml_mcu_net_demo.c. Declared rather than included:
 * the two bodies share a console, not a file. */
int hml_mcu_net_demo_run(void);
#endif

/* ------------------------------------------------------------------------- */
/* Cortex-M7 system registers. Addresses are architectural, not vendor ones.   */
/* ------------------------------------------------------------------------- */
#define SCB_CPACR (*(volatile uint32_t *)0xE000ED88u) /* coprocessor access control */
#define SCB_CPACR_CP10_CP11_FULL (0xFull << 20)

#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08u) /* vector table offset */

#define SCB_ICIALLU (*(volatile uint32_t *)0xE000EF50u) /* I-cache invalidate all to PoU */
#define SCB_DCISW (*(volatile uint32_t *)0xE000EF60u)   /* D-cache invalidate by set/way */
#define SCB_CCSIDR (*(volatile uint32_t *)0xE000ED80u)  /* cache size ID: LoU bits, sets/ways */
#define SCB_CSSELR (*(volatile uint32_t *)0xE000ED84u)  /* cache size selection: 0=I,1=D */
#define SCB_CCR (*(volatile uint32_t *)0xE000ED14u)     /* cache control: I and D enables */
#define SCB_CCR_IC 0x00000001u
#define SCB_CCR_DC 0x00000002u
#define SCB_CSSELR_SEL_DCACHE 0u
#define SCB_CSSELR_SEL_ICACHE 1u

/*
 * The D-cache enable is behind a knob so that the decision in cache_init() is
 * visible in one place and can be argued about, rather than being a deleted
 * line. It is 0 because the D-cache silently breaks this platform's console
 * (see cache_init below for the measurement). Setting it to 1 compiles the
 * enable back in, and the resulting image will hang the robot test on this
 * platform -- that is the expected consequence, not a new bug.
 */
#ifndef HML_F767_ENABLE_ICACHE
#define HML_F767_ENABLE_ICACHE 0
#endif
#ifndef HML_F767_ENABLE_DCACHE
#define HML_F767_ENABLE_DCACHE 0
#endif

/* ------------------------------------------------------------------------- */
/* Console: USART3. THE REGISTER MAP BELOW IS RENODE'S UART.STM32_UART, NOT     */
/* THE F7's OWN USART, and startup_stm32f767zi.resc explains why in full:       */
/* UART.STM32_UART is the model Renode's own tests/peripherals/STM32_UART.robot */
/* pins down (STATUS=0x00, DATA=0x04, BRR=0x08, CR1=0x0C, UE=0x2000,           */
/* TE=0x0004), while UART.STM32F7_USART is untested and rejected every          */
/* character this image wrote. selfcheck_vendors.sh checks the BASE against    */
/* the .resc and the index row.                                                 */
/* ------------------------------------------------------------------------- */
#define HML_UART_SR (*(volatile uint32_t *)(HML_VENDOR_UART_ADDR + 0x00u))
#define HML_UART_DR (*(volatile uint32_t *)(HML_VENDOR_UART_ADDR + 0x04u))
#define HML_UART_BRR (*(volatile uint32_t *)(HML_VENDOR_UART_ADDR + 0x08u))
#define HML_UART_CR1 (*(volatile uint32_t *)(HML_VENDOR_UART_ADDR + 0x0Cu))
#define HML_UART_SR_TXE 0x0080u      /* STATUS.TXE: the model gates DATA writes on this */
#define HML_UART_CR1_UE 0x2000u      /* USART_CR1.UE */
#define HML_UART_CR1_TE 0x0004u      /* USART_CR1.TE: bit 2 in THIS model, not bit 3.
                                      * The F7's own USART uses bit 3 here; the
                                      * STM32_UART model does not. */
#define HML_UART_TXE_SPINS 100000u   /* bounded: a stuck TXE must not hang the image */

/* The console is observability plumbing, so it is written to the model in the
 * way the model actually behaves, and the measurement is recorded here because
 * the naive version looks correct and is not.
 *
 * A REAL STM32 USART must be polled for TXE before each byte. UART.STM32_UART
 * HAS NO TXE BIT: its STATUS register at +0x00 defines only IDLE (0x10) and
 * RECEIVE-NOT-EMPTY (0x20), as Renode's own tests/peripherals/STM32_UART.robot
 * asserts, and that test writes characters with no status poll at all. Polling
 * bit 7 of STATUS here, as the F7's own model would require, therefore never
 * succeeds: every single character burned the full 100000-iteration spin bound,
 * and the image crawled. That is not a hang, so it is nastier than a hang --
 * it stalled a few characters past the READY banner, which is exactly where
 * this was found. Do not reintroduce a TXE poll here.
 *
 * The write is still a plain volatile store to the data register with the
 * transmitter enabled by CR1, and a character lost by dropping a status wait is
 * a far better failure mode than an image that never reaches its DONE line. */
static void uart_putc(char c) {
  (void)HML_UART_SR; /* touched so the STATUS read stays in the tested map */
  HML_UART_DR = (uint32_t)(unsigned char)c;
}

/*
 * The three reporting helpers below are NOT static, because the network example's
 * demo body (app/hml_mcu_net_demo.c) has to print through the same console this
 * file owns. Making them static was fine when the only body was demo_task() in
 * this file; with a second body on the same board they are the board's console
 * API. The implementations are unchanged, and the codec image's output is
 * unchanged, because the codec path still calls them exactly as before.
 */
void hml_console_puts(void *ctx, const char *line) {
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

void hml_console_kv(const char *key, uint32_t value) {
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

void hml_console_hex32(const char *key, uint32_t value) {
  static const char k_hex[] = "0123456789abcdef";
  for (const char *s = key; *s != '\0'; s++) uart_putc(*s);
  for (int i = 28; i >= 0; i -= 4) uart_putc(k_hex[(value >> (unsigned)i) & 0xfu]);
  uart_putc('\n');
}

/* ------------------------------------------------------------------------- */
/* Cortex-M7 bring-up. THE FPU AND CACHE PARTS ARE REAL WRITES TO REAL        */
/* ARCHITECTURAL REGISTERS, unlike clock_init() below.                         */
/* ------------------------------------------------------------------------- */
static void fpu_init(void) {
  /* CP10/CP11 = full access. Without this, an -mfpu=fpv5-d16 build takes a
   * UsageFault the first time it touches a double. The part has an FPU; the
   * reset value of CPACR denies access, so this write is required. */
  SCB_CPACR |= SCB_CPACR_CP10_CP11_FULL;
  __asm__ volatile("dsb" ::: "memory");
  __asm__ volatile("isb" ::: "memory");
}

static void cache_init(void) {
  /* Enable I-cache and D-cache: standard Cortex-M7 bring-up, and the reason a
   * -mcpu=cortex-m7 image behaves like a real F7 rather than an M4 with extra
   * instructions. Each cache is invalidated by set/way BEFORE it is enabled,
   * which is the ARM-recommended order and avoids executing with a cold cache
   * while the SCB still reports it as off.
   *
   * BOTH CACHES ARE DELIBERATELY LEFT OFF, and both decisions are measured.
   *
   * The D-cache was the first thing tried and it breaks the console: the image
   * runs, but every character written to the USART is dropped by the model with
   * a 'transmitter is not enabled, ignoring' warning, tens of thousands of them,
   * and the robot test times out waiting for a line that can never arrive. The
   * cause is coherency. Renode's Cortex-M honours the SCB D-cache enable, but a
   * dirty cache line holding the CR1 write is not driven out to the peripheral,
   * so the USART never learns that UE and TE were set.
   *
   * The I-cache is worse: enabling it makes the image hang COMPLETELY. Renode
   * logs the SCB ICIALLU write as unhandled, CCR takes the IC bit, and the
   * machine then never reaches the first character of the board banner and
   * never faults either -- it just stops. That is a silent wedge, not a crash,
   * and it is the worst possible failure mode to leave in a startup path.
   *
   * THAT IS THE SAME TRAP A REAL STM32F767 SETS FOR A REAL MAC. On silicon the
   * fix is NOT to leave the cache off: it is to keep the cache on and either
   * clean-and-invalidate around every descriptor-ring access (SCB_DCCIMVAC /
   * DCCMVAC) or place the ring in non-cacheable memory through the MPU, and to
   * enable the I-cache only once the vector table and literal pools are safe to
   * fetch through it. This image does none of that, so the honest choice is
   * cache-off, where a plain volatile store reaches the peripheral and the demo
   * is testable. Cache-off is a normal, defensible M7 configuration and is what
   * you want for DMA anyway. Whoever re-enables either cache owns that
   * discipline, owns the silicon validation, and owns the claim. */
#if HML_F767_ENABLE_ICACHE
  if ((SCB_CCR & SCB_CCR_IC) == 0u) {
    SCB_ICIALLU = 0u;
    __asm__ volatile("dsb" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
    SCB_CCR |= SCB_CCR_IC;
  }
#endif
#if HML_F767_ENABLE_DCACHE
  if ((SCB_CCR & SCB_CCR_DC) == 0u) {
    SCB_CSSELR = SCB_CSSELR_SEL_DCACHE;
    __asm__ volatile("dsb" ::: "memory");
    /* LoU = line offset bits; sets and ways are both 1 + the encoded field. */
    uint32_t ccsidr = SCB_CCSIDR;
    uint32_t sets = (ccsidr & 0x0FFFu) + 1u;
    uint32_t ways = ((ccsidr >> 3u) & 0x03FFu) + 1u;
    for (uint32_t s = 0u; s < sets; s++) {
      for (uint32_t w = 0u; w < ways; w++) {
        SCB_DCISW = (s << 5u) | w;
        __asm__ volatile("dsb" ::: "memory");
      }
    }
    SCB_CSSELR = SCB_CSSELR_SEL_ICACHE;
    __asm__ volatile("dsb" ::: "memory");
    SCB_CCR |= SCB_CCR_DC;
    __asm__ volatile("dsb" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
  }
#endif /* HML_F767_ENABLE_DCACHE */
}

/* ------------------------------------------------------------------------- */
/* Vendor bring-up hook. A DOCUMENTED NO-OP, and that is deliberate.          */
/* ------------------------------------------------------------------------- */
static void clock_init(void) {
  /* Real silicon: enable the SYSCFG and ETH clocks, set the MII/RMII selector
   * bit, program the APB prescalers, the PLL and the flash wait states, then
   * mux the console USART3 and the ETH pins to their alternate functions.
   * Renode models neither the RCC registers nor the flash wait states, so there
   * is nothing that could be checked here and a wrong write is a pointless way
   * to fail. This function exists so the seam is obvious, not so it can do
   * something. */
}

static void uart_init(void) {
  /* USART_CR1.UE | USART_CR1.TE, then a BRR divisor. A REAL write, not
   * decoration: the emulated USART drops bytes written to the data register
   * while UE is clear, and so does the real part. Renode's own test for this
   * model (tests/peripherals/STM32_UART.robot) enables it and then programs
   * BRR the same way, so the sequence below is the one that is actually known
   * to work against a real Renode model rather than a guess. The divisor value
   * only sets a line rate the emulated console does not enforce. */
  HML_UART_BRR = 0x10u;
  HML_UART_CR1 = HML_UART_CR1_UE | HML_UART_CR1_TE;
}

/* ------------------------------------------------------------------------- */
/* hml_clock_us -- the frozen port contract, at 1 ms resolution exactly as    */
/* documented for a tick-based MCU port in ports/freertos_lwip.               */
/* ------------------------------------------------------------------------- */
uint64_t hml_clock_us(void) {
  return ((uint64_t)xTaskGetTickCount() * 1000000ull) / ((uint64_t)portTICK_PERIOD_MS * 1000ull);
}

#if HML_VENDOR_APP_NETWORK
/* hml_clock_ms is hml_clock_us in milliseconds, kept here rather than open-coded in
 * the demo so the conversion exists exactly once and the demo cannot get the
 * scaling wrong. Network-image only, for the same reason as systick_init below. */
uint64_t hml_clock_ms(void) { return hml_clock_us() / 1000ull; }
#endif

#if HML_VENDOR_APP_NETWORK
/* ------------------------------------------------------------------------- */
/* systick_init -- SysTick, and ONLY in the network image.                     */
/*                                                                            */
/* WHY IT EXISTS, and why it is not in the codec image                         */
/*   clock_init() above is a documented no-op, so SysTick is left at its reset  */
/*   value: ENABLE clear. The shared FreeRTOS stub's wait_ticks() therefore      */
/*   never sees a COUNTFLAG edge, spins out its budget, sets its "systick      */
/*   stalled" flag and returns -- which is exactly why the codec image prints   */
/*   HML-MCU-DEMO-WARN systick-did-not-advance and still passes. g_ticks never  */
/*   moves, so hml_clock_us() returns 0 forever.                               */
/*                                                                            */
/*   That is acceptable for the codec body, which never reads the clock, and    */
/*   unacceptable for lwIP. The stack is built with -Dsys_now=hml_lwip_now_ms,  */
/*   and a sys_now that returns a constant 0 is not a slow clock, it is a      */
/*   broken one: the ARP cache never ages, so a stale entry is served forever,  */
/*   and every lwIP timeout (UDP reassembly, ARP retry) either never fires or   */
/*   fires on the first poll. The network run would then depend on the exact    */
/*   order of operations inside lwIP rather than on the timebase being right.   */
/*                                                                            */
/*   It is compiled out of the codec image by the HML_VENDOR_APP_NETWORK guard */
/*   above, so the codec ELF cannot be affected by it even in principle: the   */
/*   whole block is absent from that translation unit, not merely unreached.   */
/*                                                                            */
/* WHAT IT WRITES                                                              */
/*   RVR is loaded for a 1 ms tick at the part's real 168 MHz core clock        */
/*   (168000 - 1). TICKINT is deliberately LEFT CLEAR: this image has no        */
/*   SysTick_Handler and takes no interrupts at all, so an enabled TICKINT      */
/*   would vector to a null handler. COUNTFLAG is what the stub's wait_ticks()  */
/*   polls, and it is set by the counter wrapping regardless of TICKINT.        */
/*                                                                            */
/* HONESTY NOTE: Renode's CPU here has no explicit frequency, so the modelled   */
/* tick period is nominal. What this guarantees is that the timebase is         */
/* MONOTONIC and non-zero, which is what lwIP needs; it does not guarantee 1 ms  */
/* of emulated time. The network robot test asserts the counter advanced, not  */
/* that it advanced by a particular amount.                                     */
/* ------------------------------------------------------------------------- */
#define SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018u)
#define SYST_CSR_ENABLE (1u << 0)
#define SYST_CSR_CLKSOURCE (1u << 2)
#define SYST_RELOAD_1MS_168MHZ (168000u - 1u)

static void systick_init(void) {
  SYST_CSR = 0u;              /* disable before reprogramming, as the part requires */
  SYST_RVR = SYST_RELOAD_1MS_168MHZ;
  SYST_CVR = 0u;              /* writing CVR clears both it and COUNTFLAG */
  SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_CLKSOURCE;
  __asm__ volatile("dsb" ::: "memory");
}
#endif /* HML_VENDOR_APP_NETWORK */

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
/*   so some of the strings it prints are base-platform text (the hml_mcu_demo.elf */
/*   image name, the SDRAM claim). Renaming those would mean editing the shared    */
/*   body, which is exactly what sharing it is meant to prevent. So this board      */
/*   CORRECTS them from the outside: main() prints its own identity line first.    */
/*   Where the two disagree, this one is right.                                    */
/* ------------------------------------------------------------------------- */
static void print_board_identity(void) {
  hml_console_puts(NULL, HML_MCU_DEMO_MARK_NOTE " vendor=" HML_VENDOR_NAME " arch=" HML_VENDOR_ARCH);
  hml_console_puts(NULL, HML_MCU_DEMO_MARK_NOTE " image=" HML_VENDOR_ELF_NAME
                                         " (the shared banner's hml_mcu_demo.elf name is"
                                         " base-platform text; on this board it is wrong)");
  hml_console_puts(NULL, HML_MCU_DEMO_MARK_NOTE " this board HAS a modelled network peripheral,"
                                         " which no other vendor row has");
  hml_console_kv(HML_MCU_DEMO_MARK_NOTE " heap-bytes=", (uint32_t)HML_VENDOR_HEAP_BYTES);
  hml_console_kv(HML_MCU_DEMO_MARK_NOTE " frame-bytes-default=", (uint32_t)HML_VENDOR_FRAME_BYTES);
  hml_console_hex32(HML_MCU_DEMO_MARK_NOTE " sram-base=", HML_VENDOR_SRAM_ADDR);
  hml_console_kv(HML_MCU_DEMO_MARK_NOTE " sram-bytes=", (uint32_t)HML_VENDOR_SRAM_BYTES);
  hml_console_hex32(HML_MCU_DEMO_MARK_NOTE " flash-base=", HML_VENDOR_FLASH_ADDR);
  hml_console_kv(HML_MCU_DEMO_MARK_NOTE " flash-bytes=", (uint32_t)HML_VENDOR_FLASH_BYTES);
  hml_console_kv(HML_MCU_DEMO_MARK_NOTE " uart-irq=", (uint32_t)HML_VENDOR_UART_IRQ);
  hml_console_hex32(HML_MCU_DEMO_MARK_NOTE " eth-base=", HML_VENDOR_ETH_ADDR);
  hml_console_kv(HML_MCU_DEMO_MARK_NOTE " eth-irq=", (uint32_t)HML_VENDOR_ETH_IRQ);
  hml_console_puts(NULL, HML_MCU_DEMO_MARK_NOTE " eth is the ONLY vendor row with a modelled MAC:"
                                         " Network.SynopsysEthernetMAC from stm32f777.repl");
  hml_console_puts(NULL, HML_MCU_DEMO_MARK_NOTE " no interrupt is enabled or taken on this board:"
                                         " the irq values above are documentation, not wiring");
  hml_console_puts(NULL, HML_MCU_DEMO_MARK_NOTE " fpu is enabled; caches are deliberately OFF"
                                         " (enabling either one wedges or stalls this"
                                         " model, see cache_init); dma coherency is NOT"
                                         " implemented");
  hml_console_puts(NULL, HML_MCU_DEMO_MARK_NOTE " clock init is a documented no-op: Renode models"
                                         " neither RCC nor a baud divisor");
}

/* ------------------------------------------------------------------------- */
/* The demo task. It runs to completion inside vTaskStartScheduler(), after    */
/* which the stub scheduler idles -- where the image spends the rest of its     */
/* emulated life. Renode keeps running that idle loop until the robot test has  */
/* seen the DONE line.                                                        */
/* ------------------------------------------------------------------------- */
#if !HML_VENDOR_APP_NETWORK
/* The codec body's scheduler glue. Compiled out of the network image, because
 * -Werror is on in both builds and an unused static function is an error: leaving
 * it in would mean weakening the warning set for the codec image to keep the
 * network image building, which is the wrong trade. Nothing above or below this
 * guard changes when HML_VENDOR_APP_NETWORK is 0. */
static void demo_pause(void *ctx, uint32_t ms) {
  (void)ctx;
  vTaskDelay(pdMS_TO_TICKS(ms));
}
#endif

#if !HML_VENDOR_APP_NETWORK
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
  io.puts = hml_console_puts;

  rc = hml_mcu_demo_run(&cfg, &io, &res);
  hml_console_kv(HML_MCU_DEMO_MARK_TASK " rc=", (uint32_t)(rc < 0 ? 1u : (uint32_t)rc));

  if (hml_stub_systick_stalled()) {
    /* A platform model that never drives SysTick. Reported, never fatal: the
     * stub's bounded fallback already kept the image out of a hang. */
    const char *m = HML_MCU_DEMO_MARK_WARN
                    " systick-did-not-advance pauses-fell-back-to-a-spin-budget";
    hml_console_puts(NULL, m);
  }
  hml_console_kv("HML-MCU-DEMO-TICKS ms=", hml_stub_elapsed_ms());
}

#endif /* !HML_VENDOR_APP_NETWORK */

int main(void) {
  TaskHandle_t t = NULL;
  fpu_init();
  cache_init();
  clock_init();
  uart_init();
  print_board_identity();

#if HML_VENDOR_APP_NETWORK
  /* ------------------------------------------------------------------------
   * THE NETWORK IMAGE. A second demo body on the same board, selected at compile
   * time rather than by a run-time flag, for one concrete reason: the codec
   * image is a PASSING acceptance test and this switch must not be able to
   * change a single byte of it. With HML_VENDOR_APP_NETWORK at its default of 0,
   * everything above and below this block is compiled exactly as it was, so
   * stm32f767zi_hml_mcu_demo.elf is unchanged and its robot test stays green.
   *
   * What differs is the body, and the difference is the point of the row:
   *   - the codec body runs the shared hml_mcu_demo_run() under the FreeRTOS
   *     stub scheduler and asserts that NO datagram ever leaves the image;
   *   - this body calls lwIP and the MAC driver, and asserts that an HML datagram
   *     DOES leave, byte for byte, which the network robot test checks on the
   *     emulated wire.
   *
   * The stack is not started through a scheduler here. lwIP is NO_SYS, so there
   * is no task to create: the netif and the transport are driven directly, and
   * hml_netif_poll() from the demo body IS the receive pump.
   * ---------------------------------------------------------------------- */
  hml_console_puts(NULL,
                   HML_MCU_DEMO_MARK_NOTE " image=NETWORK: lwIP 2.2.1 + this board's MAC driver,"
                                           " the codec body is NOT linked into this ELF");
  hml_console_puts(NULL,
                   HML_MCU_DEMO_MARK_NOTE " this image is expected to put a real HML/UDP"
                                           " datagram on the emulated PHY");
  hml_console_hex32(HML_MCU_DEMO_MARK_NOTE " eth-base=", HML_VENDOR_ETH_ADDR);
  hml_console_kv(HML_MCU_DEMO_MARK_NOTE " eth-irq=", (uint32_t)HML_VENDOR_ETH_IRQ);
  /* Before the demo body, and only here: the codec image must keep its
   * deliberately stalled tick and its HML-MCU-DEMO-WARN line. */
  systick_init();
  (void)t;
  (void)hml_mcu_net_demo_run();
  /* The demo returns rather than looping forever. Renode is left running the rest
   * of this line's execution as a tight loop, which is exactly where the codec
   * image idles too, and the robot test has already seen the DONE line by then. */
  for (;;) {
  }
#else
  if (xTaskCreate(demo_task, "hml_demo", 512u, NULL, 1u, &t) != pdPASS) {
    const char *m = HML_MCU_DEMO_MARK_FAIL " the stub task table has no free slot";
    hml_console_puts(NULL, m);
    for (;;) {
    }
  }
  vTaskStartScheduler(); /* never returns */
  for (;;) {
  }
#endif
}

/* ------------------------------------------------------------------------- */
/* Reset handler. ARMv7-M loads SP from the first vector word, so the vector    */
/* table is a plain array and the linker script pins it to the start of flash.  */
/* ------------------------------------------------------------------------- */
__attribute__((section(".isr_vector"), used))
void (*const g_vectors[])(void) = {
    (void (*)(void))(uintptr_t)&__stack_top__,
    Reset_Handler,
};

void Reset_Handler(void) {
  uint32_t *src = &__data_load__;
  uint32_t *dst = &__data_start__;

  SCB_VTOR = 0x08000000u; /* point the part at its own vector table */
  for (; dst < &__data_end__;) *dst++ = *src++; /* .data: flash -> SRAM */
  for (dst = &__bss_start__; dst < &__bss_end__;) *dst++ = 0u;

  main();
  for (;;) {
  }
}
