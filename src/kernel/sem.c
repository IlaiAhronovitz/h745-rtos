#include "sem.h"

void k_sem_init(k_sem_t *s, int32_t count)
{
    s->count = count;
    s->waiters = 0;
}

void k_sem_take(k_sem_t *s)
{
    uint32_t st = k_crit_enter();

    if (s->count > 0) {
        s->count--;
        k_crit_exit(st);
        return;
    }

    /* Block: park on the waitlist, priority-sorted (FIFO within a level).
     * The 'next' link is free — a RUNNING task is never on a ready queue. */
    k_task_t *cur = k_curr;
    cur->state = K_BLOCKED;
    k_task_t **pp = &s->waiters;
    while (*pp && (*pp)->prio <= cur->prio)
        pp = &(*pp)->next;
    cur->next = *pp;
    *pp = cur;

    k_pend_switch();
    k_crit_exit(st);
    /* Running again ⇒ some give() handed us the token directly. */
}

int k_sem_try_take(k_sem_t *s)
{
    uint32_t st = k_crit_enter();
    int ok = s->count > 0;
    if (ok) s->count--;
    k_crit_exit(st);
    return ok;
}

void k_sem_give(k_sem_t *s)
{
    uint32_t st = k_crit_enter();

    k_task_t *w = s->waiters;
    if (w) {
        s->waiters = w->next;            /* head = highest-priority waiter */
        w->next = 0;
        k_ready_locked(w);
        /* Preempt if the woken task outranks whoever is running now. From an
         * ISR, k_curr is the interrupted task — same comparison applies. */
        if (!k_curr || w->prio < k_curr->prio)
            k_pend_switch();
    } else {
        s->count++;
    }

    k_crit_exit(st);
}
