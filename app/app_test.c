/* On-target validation suite (build with: make APP=test).
 *
 * Runs sequentially on real hardware and prints PASS/FAIL plus measured
 * numbers. The captured transcript is the "hardware-validated" evidence.
 * The LAST test deliberately overflows a task stack — the run is SUPPOSED
 * to end in "KERNEL PANIC: stack overflow  task=victm".
 */
#include "stm32h745.h"
#include "uart.h"
#include "board.h"
#include "kprintf.h"
#include "kernel.h"
#include "sem.h"

#define US(cyc) ((cyc) / (K_CPU_HZ / 1000000u))

static k_task_t t_run;
static uint32_t s_run[512] __attribute__((aligned(8)));

static int pass_count, fail_count;

static void verdict(const char *name, int ok, const char *detail)
{
    if (ok) pass_count++; else fail_count++;
    kprintf("[%s] %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
}

/* ---- T1: context-switch cost (yield round trip) -------------------------- */
static void t1_switch_cost(void)
{
    for (int i = 0; i < 10; i++) k_yield();          /* warm up */

    uint32_t lo = ~0u, hi = 0, sum = 0;
    const int N = 1000;
    for (int i = 0; i < N; i++) {
        uint32_t t0 = DWT_CYCCNT;
        k_yield();                                   /* full PendSV round trip */
        uint32_t dt = DWT_CYCCNT - t0;
        if (dt < lo) lo = dt;
        if (dt > hi) hi = dt;
        sum += dt;
    }
    uint32_t avg = sum / (uint32_t)N;
    kprintf("  T1 yield round trip: min=%lu avg=%lu max=%lu cycles"
            " (avg %lu us @64 MHz)\n", lo, avg, hi, US(avg));
    verdict("T1 context-switch cost", avg < 2000, "avg under 2000 cycles");
}

/* ---- T2: semaphore handoff / preemption latency -------------------------- */
static k_sem_t hs, hs_done;
static volatile uint32_t t2_wake_cyc;
static k_task_t t_hi;
static uint32_t s_hi[512] __attribute__((aligned(8)));

static void hi_body(void *arg)
{
    (void)arg;
    for (;;) {
        k_sem_take(&hs);
        t2_wake_cyc = DWT_CYCCNT;        /* first thing after waking */
        k_sem_give(&hs_done);
    }
}

static void t2_preemption(void)
{
    k_sem_init(&hs, 0);
    k_sem_init(&hs_done, 0);
    k_task_create(&t_hi, "hi   ", hi_body, 0, 0, s_hi, 512);   /* outranks runner */
    k_sleep_ms(2);                       /* let it park on the semaphore */

    uint32_t lo = ~0u, hi_c = 0, sum = 0;
    const int N = 1000;
    for (int i = 0; i < N; i++) {
        uint32_t t0 = DWT_CYCCNT;
        k_sem_give(&hs);                 /* wakes prio-0 -> preempts inside give */
        k_sem_take(&hs_done);
        uint32_t dt = t2_wake_cyc - t0;
        if (dt < lo) lo = dt;
        if (dt > hi_c) hi_c = dt;
        sum += dt;
    }
    uint32_t avg = sum / (uint32_t)N;
    kprintf("  T2 give->preempt->wake: min=%lu avg=%lu max=%lu cycles"
            " (avg %lu us)\n", lo, avg, hi_c, US(avg));
    verdict("T2 preemption latency", avg < 3000, "avg under 3000 cycles");
}

/* ---- T3: round-robin fairness -------------------------------------------- */
static volatile uint32_t rr_count[2];
static volatile int rr_stop;
static k_task_t t_rr[2];
static uint32_t s_rr[2][512] __attribute__((aligned(8)));

static void rr_body(void *arg)
{
    uint32_t id = (uint32_t)arg;
    while (!rr_stop) rr_count[id]++;
}                                        /* returning exercises the exit trap */

static void t3_round_robin(void)
{
    rr_count[0] = rr_count[1] = 0;
    rr_stop = 0;
    k_task_create(&t_rr[0], "rr-a ", rr_body, (void *)0u, 4, s_rr[0], 512);
    k_task_create(&t_rr[1], "rr-b ", rr_body, (void *)1u, 4, s_rr[1], 512);

    k_sleep_ms(1000);                    /* they busy-spin below us */
    rr_stop = 1;
    k_sleep_ms(30);                      /* let both notice and exit */

    uint32_t a = rr_count[0], b = rr_count[1];
    uint32_t max = a > b ? a : b, min = a < b ? a : b;
    uint32_t skew_pct = max ? (100u * (max - min)) / max : 100u;
    kprintf("  T3 equal-prio busy tasks, 1 s: a=%lu b=%lu skew=%lu%%\n",
            a, b, skew_pct);
    verdict("T3 round-robin fairness", a && b && skew_pct <= 10,
            "both ran, skew <= 10%");
}

/* ---- T4: semaphore integrity under handoff churn ------------------------- */
#define T4_TOKENS 20000u
static k_sem_t work, t4_done;
static volatile uint32_t consumed;
static k_task_t t_prod, t_cons;
static uint32_t s_prod[512] __attribute__((aligned(8)));
static uint32_t s_cons[512] __attribute__((aligned(8)));

static void producer_body(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < T4_TOKENS; i++) {
        k_sem_give(&work);               /* consumer outranks us: direct handoff */
        if ((i & 63u) == 0) k_yield();
    }
}

static void consumer_body(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < T4_TOKENS; i++) {
        k_sem_take(&work);
        consumed++;
    }
    k_sem_give(&t4_done);
}

static void t4_semaphores(void)
{
    k_sem_init(&work, 0);
    k_sem_init(&t4_done, 0);
    consumed = 0;
    uint32_t t0 = k_now();
    k_task_create(&t_cons, "cons ", consumer_body, 0, 2, s_cons, 512);
    k_task_create(&t_prod, "prod ", producer_body, 0, 3, s_prod, 512);
    k_sem_take(&t4_done);
    uint32_t dt = k_now() - t0;

    kprintf("  T4 %u tokens producer->consumer in %lu ms, count=%ld\n",
            (unsigned)T4_TOKENS, dt, (long)work.count);
    verdict("T4 semaphore integrity",
            consumed == T4_TOKENS && work.count == 0,
            "all tokens arrived, none invented");
}

/* ---- T5: sleep accuracy --------------------------------------------------- */
static void t5_sleep_accuracy(void)
{
    uint32_t lo = ~0u, hi = 0;
    int ok = 1;
    for (int i = 0; i < 20; i++) {
        uint32_t t0 = DWT_CYCCNT;
        k_sleep_ms(50);
        uint32_t ms = (DWT_CYCCNT - t0) / (K_CPU_HZ / 1000u);
        if (ms < lo) lo = ms;
        if (ms > hi) hi = ms;
        if (ms < 49 || ms > 53) ok = 0;
    }
    kprintf("  T5 k_sleep_ms(50) x20: actual %lu..%lu ms\n", lo, hi);
    verdict("T5 sleep accuracy", ok, "every sample in [49,53] ms");
}

/* ---- T6: FPU context preservation ----------------------------------------- */
static volatile uint32_t f_rounds[2], f_fails[2];
static k_task_t t_f[2];
static uint32_t s_f[2][512] __attribute__((aligned(8)));

static void f_body(void *arg)
{
    uint32_t id = (uint32_t)arg;
    float x0 = id ? 2.718282f : 1.618034f;
    float golden = 0.0f;
    for (int r = 0; r < 40; r++) {
        float a = x0, b = 2*x0, c = 3*x0, d = 4*x0,
              e = 5*x0, f = 6*x0, g = 7*x0, h = 8*x0;
        for (int n = 0; n < 20000; n++) {
            a = a * 1.0000010f + 0.5f;
            b = b * 0.9999990f + 0.25f;
            c += a * 0.0010f;
            d += b * 0.0020f;
            e += c * 0.0001f;
            f += d * 0.0002f;
            g = g * 1.0000005f + e * 0.00001f;
            h = h * 0.9999995f + f * 0.00002f;
        }
        float sum = a + b + c + d + e + f + g + h;
        if (r == 0) golden = sum;
        else if (sum != golden) f_fails[id]++;
        f_rounds[id]++;
    }
}

static void t6_fpu(void)
{
    f_rounds[0] = f_rounds[1] = f_fails[0] = f_fails[1] = 0;
    k_task_create(&t_f[0], "fpu-a", f_body, (void *)0u, 4, s_f[0], 512);
    k_task_create(&t_f[1], "fpu-b", f_body, (void *)1u, 4, s_f[1], 512);
    while (f_rounds[0] < 40 || f_rounds[1] < 40)
        k_sleep_ms(50);                  /* they interleave via timeslice RR */

    kprintf("  T6 two timesliced FP tasks, 40 deterministic rounds each: "
            "fails a=%lu b=%lu\n", f_fails[0], f_fails[1]);
    verdict("T6 FPU lazy-stacking context", !f_fails[0] && !f_fails[1],
            "S16-S31 identical across preemptions");
}

/* ---- T7: stack-overflow canary (ends in a deliberate panic) --------------- */
static k_task_t t_victim;
/* 128 guard words sit BELOW the victim stack by construction, so the ~50-word
 * overflow plus the PendSV save frame land in memory this test owns —
 * nothing else is corrupted */
static uint32_t victim_mem[128 + 256] __attribute__((aligned(8)));

static void victim_body(void *arg)
{
    (void)arg;
    volatile uint8_t big[1200];          /* ~300 words > what's left of 256 */
    for (unsigned i = 0; i < sizeof big; i++) big[i] = 0xEE;
    (void)big;
    k_yield();                           /* switch out -> canary check fires */
    for (;;) k_sleep_ms(1000);
}

static void t7_stack_canary(void)
{
    kprintf("  T7 spawning a task that overflows its 256-word stack.\n"
            "  EXPECTED FINAL OUTPUT: KERNEL PANIC: stack overflow task=victm\n");
    uart_flush();
    k_task_create(&t_victim, "victm", victim_body, 0, 3,
                  victim_mem + 128, 256);
    k_sleep_ms(1000);
    verdict("T7 stack canary", 0, "panic did NOT fire");   /* unreachable if OK */
}

/* ---- runner --------------------------------------------------------------- */
static void runner_body(void *arg)
{
    (void)arg;
    kprintf("\n--- h745-rtos on-target validation ---\n\n");
    t1_switch_cost();
    t2_preemption();
    t3_round_robin();
    t4_semaphores();
    t5_sleep_accuracy();
    t6_fpu();
    kprintf("\n%d passed, %d failed (of 6 measurable tests)\n\n",
            pass_count, fail_count);
    t7_stack_canary();
    for (;;) k_sleep_ms(1000);
}

int main(void)
{
    board_init();
    uart_init(115200);
    kprintf("\n=== h745-rtos: APP=test build ===\n");

    k_task_create(&t_run, "run  ", runner_body, 0, 1, s_run, 512);
    k_start();
}
