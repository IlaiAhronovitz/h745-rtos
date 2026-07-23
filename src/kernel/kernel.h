/* kernel.h — minimal preemptive RTOS kernel for Cortex-M7.
 *
 * Model:
 *  - Static allocation only: caller owns every TCB and stack.
 *  - Fixed priorities 0 (highest) .. 7 (idle, reserved), FIFO round-robin
 *    within a priority level, O(1) highest-ready lookup via bitmap + CTZ.
 *  - Tasks run in privileged thread mode on PSP; exceptions run on MSP.
 *  - Context switch = PendSV at the lowest exception priority.
 *  - One kernel lock: BASEPRI = K_BASEPRI_MASK masks every kernel-aware
 *    interrupt (SysTick, PendSV, app ISRs that call k_sem_give).
 */
#pragma once
#include <stdint.h>

/* ---- configuration ------------------------------------------------------ */
#define K_CPU_HZ          64000000u
#define K_TICK_HZ         1000u
#define K_MAX_PRIOS       8u
#define K_IDLE_PRIO       (K_MAX_PRIOS - 1u)
#define K_TIMESLICE_TICKS 10u          /* round-robin quantum, in ticks      */
#define K_BASEPRI_MASK    0xE0u        /* keep in sync with port.s           */
#define K_STACK_CANARY    0xC0DE57ACu  /* lowest word of every task stack    */
#define K_STACK_FILL      0xA5A5A5A5u  /* high-water fill pattern            */

typedef enum {
    K_INVALID = 0, K_READY, K_RUNNING, K_SLEEPING, K_BLOCKED, K_ZOMBIE
} k_state_t;

typedef struct k_task {
    uint32_t *sp;               /* saved PSP — MUST stay first (port.s)      */
    struct k_task *next;        /* ready-queue / semaphore-waitlist link     */
    k_state_t state;
    uint8_t   prio;
    uint32_t  wake_tick;
    uint32_t *stack_base;       /* lowest word — holds the canary            */
    uint32_t  stack_words;
    const char *name;
    uint32_t  n_switched_in;
    struct k_task *all_next;    /* registry: sleep scan + stats              */
} k_task_t;

/* ---- public API ---------------------------------------------------------- */
void k_task_create(k_task_t *t, const char *name, void (*fn)(void *), void *arg,
                   uint8_t prio, uint32_t *stack, uint32_t stack_words);
void k_start(void) __attribute__((noreturn));
void k_yield(void);
void k_sleep_ms(uint32_t ms);
uint32_t k_now(void);                     /* ticks (ms) since k_start        */
uint32_t k_crit_enter(void);              /* returns previous BASEPRI        */
void k_crit_exit(uint32_t old);
void k_panic(const char *why, const k_task_t *t) __attribute__((noreturn));

const k_task_t *k_task_registry(void);    /* iterate via ->all_next          */
uint32_t k_stack_high_water(const k_task_t *t);   /* peak words ever used    */

/* ---- kernel-internal (shared with port.s / sem.c) ------------------------ */
extern k_task_t * volatile k_curr;
extern uint32_t k_switch_count;
void k_ready_locked(k_task_t *t);         /* callers hold the kernel lock    */
void k_pend_switch(void);
uint32_t k_in_isr(void);
void k_switch_context(void);              /* PendSV only                     */
void k_fault_dump(uint32_t *frame, uint32_t exc_return) __attribute__((noreturn));
