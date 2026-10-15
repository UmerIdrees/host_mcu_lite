/*
 * queue.h -- THIS IS NOT FreeRTOS. See ../FreeRTOS.h for the full statement of
 * what this stub is and, more importantly, what it is not. The queue here is a
 * fixed-size ring of fixed-size items: no block time, no priority
 * inheritance, no ISR-safety, no notifications, no counting semaphore, no
 * mutexes. It exists because ports/freertos_lwip/hml_port_freertos.c bridges
 * lwIP's rx callback into the polling hml_sock_recv() model through a queue,
 * and that code path must at least be compilable and shape-correct.
 */
#ifndef HML_STUB_QUEUE_H
#define HML_STUB_QUEUE_H

#include "FreeRTOS.h" /* the stub, not the kernel */

#endif /* HML_STUB_QUEUE_H */
