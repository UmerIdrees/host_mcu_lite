/*
 * freertos_stub.c -- the implementation behind the NOT-FreeRTOS headers in this
 * directory. Read ../FreeRTOS.h first: everything here is a cooperative,
 * single-task shim, and nothing in it models preemption, priorities, the tick
 * interrupt, context switches or any real-time guarantee.
 *
 * The two interesting pieces:
 *
 *   the tick      a counter of SysTick COUNTFLAG edges. COUNTFLAG is real
 *                 Cortex-M hardware (SYST_CSR bit 16, cleared on read) and is
 *                 modelled by Renode's NVIC, so vTaskDelay(ms) really does let
 *                 emulated time pass. If a platform model never drives
 *                 SysTick, the wait falls back to a bounded spin budget and
 *                 sets hml_stub_systick_stalled(); the demo prints that as a
 *                 warning, so a broken platform model is visible instead of
 *                 becoming an infinite hang.
 *
 *   the scheduler vTaskStartScheduler() runs each live task once per round in
 *                 creation order. A task that returns is "deleted". When no
 *                 task is left it idles with a bounded SysTick wait. It never
 *                 returns, which matches the real vTaskStartScheduler().
 */

#include "FreeRTOS.h"

#include <string.h>

/* ---- SysTick (ARMv7-M system control block) ----------------------------- */
#define HML_STUB_SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define HML_STUB_SYST_COUNTFLAG 0x00010000u

/* Bounded fallback so nothing can spin forever. At 1 ms per tick this is a
 * generous multi-second budget before the stub gives up on SysTick. */
#define HML_STUB_SPIN_BUDGET 20000000u

#define HML_STUB_MAX_TASKS 4u
#define HML_STUB_MAX_QUEUES 4u
#define HML_STUB_QUEUE_SLOTS 8u
#define HML_STUB_MAX_ITEM 64u

struct stub_task {
  TaskFunction_t fn;
  void *arg;
  const char *name;
  UBaseType_t prio; /* stored, then ignored: the stub has no priorities */
  UBaseType_t stack_words;
  bool live;
};

struct stub_queue {
  uint8_t slot[HML_STUB_QUEUE_SLOTS][HML_STUB_MAX_ITEM];
  UBaseType_t item_size;
  uint8_t head;
  uint8_t count;
  bool used;
};

static struct stub_task g_tasks[HML_STUB_MAX_TASKS];
static struct stub_queue g_queues[HML_STUB_MAX_QUEUES];
static TickType_t g_ticks;
static int g_systick_stalled;

/* Advance the tick by up to `want` COUNTFLAG edges, or give up. */
static void wait_ticks(TickType_t want) {
  if (want == 0u) return;
  if (want > 1000u) want = 1000u; /* no demo needs more; keeps a bad call sane */

  for (TickType_t i = 0; i < want; i++) {
    uint32_t spins = 0u;
    while ((HML_STUB_SYST_CSR & HML_STUB_SYST_COUNTFLAG) == 0u) {
      if (++spins > HML_STUB_SPIN_BUDGET) {
        g_systick_stalled = 1;
        return; /* count the tick anyway: never hang, always make progress */
      }
    }
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
      /* The real kernel deletes a task that returns; so does the stub. */
      g_tasks[i].live = false;
    }
    if (!ran_any) wait_ticks(10u);
  }
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
  if (item_size == 0u || item_size > HML_STUB_MAX_ITEM) return NULL;
  (void)length; /* the ring is a fixed HML_STUB_QUEUE_SLOTS deep */
  for (unsigned i = 0; i < HML_STUB_MAX_QUEUES; i++) {
    if (g_queues[i].used) continue;
    memset(&g_queues[i], 0, sizeof(g_queues[i]));
    g_queues[i].used = true;
    g_queues[i].item_size = item_size;
    return &g_queues[i];
  }
  return NULL;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait) {
  (void)wait; /* the stub never blocks: lwIP's rx callback must not either */
  if (q == NULL || item == NULL || !q->used) return pdFALSE;
  if (q->count >= HML_STUB_QUEUE_SLOTS) return pdFALSE; /* full: drop now */
  memcpy(q->slot[q->head], item, q->item_size);
  q->head = (uint8_t)((q->head + 1u) % HML_STUB_QUEUE_SLOTS);
  q->count++;
  return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait) {
  if (q == NULL || item == NULL || !q->used) return pdFALSE;
  if (q->count == 0u) {
    /* "Block" degrades to a bounded SysTick wait, then reports no data. */
    if (wait != 0u && wait != portMAX_DELAY) wait_ticks(wait);
    return pdFALSE;
  }
  {
    /* Oldest item sits `count` slots behind the insert head. */
    uint8_t tail =
        (uint8_t)((q->head + HML_STUB_QUEUE_SLOTS - q->count) % HML_STUB_QUEUE_SLOTS);
    memcpy(item, q->slot[tail], q->item_size);
    q->count--;
  }
  return pdTRUE;
}

void vQueueDelete(QueueHandle_t q) {
  if (q == NULL) return;
  memset(q, 0, sizeof(*q));
}
