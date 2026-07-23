/* Counting semaphore with priority-ordered blocking.
 *
 * give() with waiters hands the token DIRECTLY to the highest-priority waiter
 * (count is not incremented) — no thundering herd, no re-check loop in take().
 * give() is safe from ISRs running at or below the kernel BASEPRI level.
 */
#pragma once
#include "kernel.h"

typedef struct {
    volatile int32_t count;
    k_task_t *waiters;          /* priority-sorted, FIFO within equal prio */
} k_sem_t;

void k_sem_init(k_sem_t *s, int32_t count);
void k_sem_take(k_sem_t *s);    /* blocks; never call from ISR or idle */
int  k_sem_try_take(k_sem_t *s);
void k_sem_give(k_sem_t *s);    /* task or ISR context */
