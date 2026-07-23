#include "kernel.h"
#include "stm32h745.h"
#include "kprintf.h"
#include "uart.h"
#include "board.h"

k_task_t * volatile k_curr;
uint32_t k_switch_count;

static k_task_t *rq_head[K_MAX_PRIOS];
static k_task_t *rq_tail[K_MAX_PRIOS];
static uint32_t  rq_bitmap;              /* bit n set <=> ready queue n nonempty */
static k_task_t *registry;
static volatile uint32_t ticks;
static uint32_t slice;

static k_task_t idle_tcb;
static uint32_t idle_stack[128] __attribute__((aligned(8)));

/* ---- kernel lock: BASEPRI masks SysTick/PendSV/kernel-aware ISRs --------- */
uint32_t k_crit_enter(void)
{
    uint32_t old, val = K_BASEPRI_MASK;
    __asm volatile("mrs %0, basepri" : "=r"(old));
    __asm volatile("msr basepri, %0" :: "r"(val) : "memory");
    __asm volatile("isb");
    return old;
}

void k_crit_exit(uint32_t old)
{
    __asm volatile("msr basepri, %0" :: "r"(old) : "memory");
}

uint32_t k_in_isr(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    return ipsr;
}

void k_pend_switch(void)
{
    SCB_ICSR = ICSR_PENDSVSET;           /* taken once BASEPRI unmasks it */
}

/* ---- ready queues: FIFO per priority, bitmap for O(1) highest-ready ------ */
static void rq_push(k_task_t *t)
{
    t->next = 0;
    if (rq_tail[t->prio]) rq_tail[t->prio]->next = t;
    else                  rq_head[t->prio] = t;
    rq_tail[t->prio] = t;
    rq_bitmap |= (1u << t->prio);
}

static k_task_t *rq_pop(uint32_t prio)
{
    k_task_t *t = rq_head[prio];
    rq_head[prio] = t->next;
    if (!rq_head[prio]) {
        rq_tail[prio] = 0;
        rq_bitmap &= ~(1u << prio);
    }
    t->next = 0;
    return t;
}

void k_ready_locked(k_task_t *t)
{
    t->state = K_READY;
    rq_push(t);
}

/* Called from PendSV with the kernel lock held: outgoing context is already
 * saved to k_curr->sp; pick who runs next. */
void k_switch_context(void)
{
    k_task_t *cur = k_curr;

    if (cur->stack_base[0] != K_STACK_CANARY)
        k_panic("stack overflow", cur);

    if (cur->state == K_RUNNING)         /* still runnable (not blocked/asleep/ */
        k_ready_locked(cur);             /* zombie) -> back of its own queue    */

    /* idle is always runnable, so the bitmap is never empty here */
    uint32_t prio = (uint32_t)__builtin_ctz(rq_bitmap);
    k_task_t *next = rq_pop(prio);
    next->state = K_RUNNING;
    next->n_switched_in++;
    k_switch_count++;
    k_curr = next;
}

/* ---- task lifecycle ------------------------------------------------------ */
static void task_exit_trap(void)
{
    uint32_t st = k_crit_enter();
    k_curr->state = K_ZOMBIE;            /* never requeued again */
    k_pend_switch();
    k_crit_exit(st);
    for (;;) cpu_wfi();                  /* until PendSV takes us out */
}

void k_task_create(k_task_t *t, const char *name, void (*fn)(void *), void *arg,
                   uint8_t prio, uint32_t *stack, uint32_t stack_words)
{
    for (uint32_t i = 0; i < stack_words; i++) stack[i] = K_STACK_FILL;
    stack[0] = K_STACK_CANARY;

    uint32_t *top = (uint32_t *)((uint32_t)(stack + stack_words) & ~7u);

    /* Fabricated hardware exception frame (PM0253 §2.3.7) — consumed by the
     * exception return that first launches this task: */
    *--top = 0x01000000u;                /* xPSR: Thumb bit                  */
    *--top = (uint32_t)fn & ~1u;         /* PC: task entry                   */
    *--top = (uint32_t)task_exit_trap;   /* LR: catches a task that returns  */
    *--top = 0;                          /* R12                              */
    *--top = 0;                          /* R3                               */
    *--top = 0;                          /* R2                               */
    *--top = 0;                          /* R1                               */
    *--top = (uint32_t)arg;              /* R0: task argument                */
    /* Software frame, popped by port.s as ldmia {r4-r11, lr}: */
    *--top = 0xFFFFFFFDu;                /* EXC_RETURN: thread, PSP, no FP   */
    for (int i = 0; i < 8; i++) *--top = 0;   /* R11..R4                     */

    t->sp = top;
    t->name = name;
    t->prio = prio;
    t->stack_base = stack;
    t->stack_words = stack_words;
    t->wake_tick = 0;
    t->n_switched_in = 0;

    uint32_t st = k_crit_enter();
    t->all_next = registry;
    registry = t;
    k_ready_locked(t);
    k_crit_exit(st);
}

static void idle_body(void *arg)
{
    (void)arg;
    for (;;) cpu_wfi();
}

void k_start(void)
{
    k_task_create(&idle_tcb, "idle", idle_body, 0, K_IDLE_PRIO,
                  idle_stack, sizeof idle_stack / 4u);

    /* PendSV must be the LOWEST priority in the system so a switch only ever
     * happens as the last exception out; SysTick sits at the kernel level.
     * (PM0253: SHPR3[23:16] = PendSV, [31:24] = SysTick.) */
    SCB_SHPR3 = (0xF0u << 16) | ((uint32_t)K_BASEPRI_MASK << 24);

    uint32_t prio = (uint32_t)__builtin_ctz(rq_bitmap);
    k_curr = rq_pop(prio);
    k_curr->state = K_RUNNING;
    k_curr->n_switched_in = 1;

    SYST_RVR = K_CPU_HZ / K_TICK_HZ - 1u;
    SYST_CVR = 0;
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE;

    __asm volatile("svc 0");             /* port.s launches k_curr on PSP */
    for (;;) {}                          /* unreachable */
}

/* ---- tick ---------------------------------------------------------------- */
void SysTick_Handler(void)
{
    ticks++;

    int resched = 0;
    for (k_task_t *t = registry; t; t = t->all_next)
        if (t->state == K_SLEEPING && (int32_t)(ticks - t->wake_tick) >= 0) {
            k_ready_locked(t);
            resched = 1;
        }

    if (++slice >= K_TIMESLICE_TICKS) {  /* round-robin quantum expired */
        slice = 0;
        resched = 1;
    }

    if (resched) k_pend_switch();
}

uint32_t k_now(void) { return ticks; }

void k_sleep_ms(uint32_t ms)
{
    if (!ms) { k_yield(); return; }
    uint32_t st = k_crit_enter();
    k_curr->state = K_SLEEPING;
    k_curr->wake_tick = ticks + ms;      /* 1 tick == 1 ms */
    k_pend_switch();
    k_crit_exit(st);                     /* PendSV fires right here */
}

void k_yield(void)
{
    uint32_t st = k_crit_enter();
    k_pend_switch();
    k_crit_exit(st);
}

/* ---- introspection ------------------------------------------------------- */
const k_task_t *k_task_registry(void) { return registry; }

uint32_t k_stack_high_water(const k_task_t *t)
{
    uint32_t i = 1;                      /* word 0 is the canary */
    while (i < t->stack_words && t->stack_base[i] == K_STACK_FILL) i++;
    return t->stack_words - i;
}

void k_panic(const char *why, const k_task_t *t)
{
    __asm volatile("cpsid i");
    kprintf("\n*** KERNEL PANIC: %s", why);
    if (t)
        kprintf("  task=%s sp=%08lx base=%08lx", t->name,
                (uint32_t)t->sp, (uint32_t)t->stack_base);
    kprintf(" ***\n");
    uart_flush();
    board_led(LED_RED, 1);
    for (;;) {}
}
