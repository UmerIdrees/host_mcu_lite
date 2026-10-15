/*
 * FreeRTOS.h -- THIS IS NOT FreeRTOS.
 *
 * ===========================================================================
 *  STUB. A hand-written, single-task, cooperative stand-in for the small slice
 *  of the FreeRTOS API that ports/freertos_lwip/hml_port_freertos.c uses:
 *
 *      TickType_t, portTICK_PERIOD_MS, portMAX_DELAY, pdMS_TO_TICKS,
 *      pdTRUE/pdFALSE/pdPASS/pdFAIL, xTaskGetTickCount,
 *      xTaskCreate, vTaskStartScheduler, vTaskDelay, taskYIELD,
 *      QueueHandle_t, xQueueCreate, xQueueSend, xQueueReceive, vQueueDelete
 *
 *  It is a compatibility shim so the hml MCU demo can be compiled and run as
 *  bare-metal code under an emulator. It is NOT a FreeRTOS port, and no claim
 *  in this repository may describe it as one. Specifically it is:
 *
 *    * NOT preemptive. Nothing here preempts anything. A task runs until it
 *      blocks (vTaskDelay) or returns.
 *    * NOT priority based. Priorities are stored and ignored; tasks run in
 *      creation order, one pass each per scheduler round.
 *    * NOT a real time kernel. There is no tick interrupt, no timer service,
 *      no context switch, no stack frame, no port layer, no idle hook, no
 *      malloc-from-heap guarantees, no SMP/ SMP-safe anything.
 *    * NOT FreeRTOSConfig.h. There is no configuration, no INCLUDE_* option,
 *      no hook, no assertion, no stats, no trace facility.
 *    * NOT tested against FreeRTOS. No FreeRTOS source was available when this
 *      was written, so nothing here was compiled against, diffed with, or run
 *      next to the real thing. A semantic difference between this file and
 *      real FreeRTOS is entirely possible and would be invisible to CI.
 *
 *  Replacing it with real FreeRTOS is the whole point of the seam: delete
 *  stub_freertos/ from the include path, put the real kernel and the target's
 *  port layer on the include path, define HML_PORT_HAVE_FREERTOS, and the
 *  library's real path in ports/freertos_lwip/hml_port_freertos.c takes over.
 *  See docs/renode.md section 7 for the exact list of inputs that requires.
 * ===========================================================================
 *
 * Tick source: the SysTick COUNTFLAG bit (SYST_CSR bit 16), which is real
 * Cortex-M hardware and is modelled by Renode's NVIC. Reading SYST_CSR clears
 * COUNTFLAG, so the tick is a COUNTFLAG-edge counter. The tick period is
 * whatever the platform's SysTick reload value implies; the demo only uses
 * vTaskDelay as a "let time pass" call and prints a warning if the tick never
 * advances, so a platform model that does not drive SysTick degrades to a
 * warning rather than a hang.
 */

#ifndef HML_STUB_FREERTOS_H
#define HML_STUB_FREERTOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- FreeRTOS-compatible spellings ------------------------------------- */
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  pdTRUE
#define pdFAIL  pdFALSE

/* 1 kHz, matching the tick-resolution caveat already documented in
 * ports/freertos_lwip/hml_port_freertos.c. */
#define portTICK_PERIOD_MS 1u

/* "Block forever": the stub never blocks forever, it idles with a bounded
 * SysTick wait. See freertos_stub.c. */
#define portMAX_DELAY 0xffffffffu

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void (*TaskFunction_t)(void *);
typedef uint32_t pdHandle_t;

typedef struct stub_task *TaskHandle_t;
typedef struct stub_queue *QueueHandle_t;

/* ---- STUB API (not FreeRTOS) -------------------------------------------- */
TickType_t xTaskGetTickCount(void);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, UBaseType_t stack_words, void *arg,
                      UBaseType_t prio, TaskHandle_t *out);
void vTaskStartScheduler(void); /* never returns, exactly like the real one */
void vTaskDelay(TickType_t ticks);
void taskYIELD(void);

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait);
void vQueueDelete(QueueHandle_t q);

/* Emulated-time progress since boot, in milliseconds. Derived from the same
 * COUNTFLAG counter as the tick, so it advances at tick resolution only. */
uint32_t hml_stub_elapsed_ms(void);

/* Non-zero once the stub has given up waiting for SysTick (a platform model
 * that does not drive SysTick). The demo turns this into a warning line. */
int hml_stub_systick_stalled(void);

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#endif /* HML_STUB_FREERTOS_H */
