/* Demo application — six tasks + one ISR exercising every kernel feature.
 *
 *   prio 1  btn      blocks on a semaphore; the button ISR gives it and the
 *                    ISR→task wake latency is printed in cycles/µs
 *   prio 2  green    LD1 heartbeat, 2 Hz          (k_sleep_ms)
 *   prio 2  yellow   LD2, 1 Hz                    (k_sleep_ms)
 *   prio 3  stats    every 5 s: per-task state, stack high-water, switches
 *                    (outranks the fpu load — a lower-prio observer starves)
 *   prio 4  fpu-a/b  continuous float chains that verify S16-S31 survive
 *                    preemption (lazy FP stacking) — busy, timesliced RR
 *   prio 7  idle     wfi
 */
#include "stm32h745.h"
#include "uart.h"
#include "board.h"
#include "kprintf.h"
#include "kernel.h"
#include "sem.h"
#include <stdarg.h>

static k_sem_t print_lock;               /* binary: one UART line at a time */
static k_sem_t btn_sem;
static volatile uint32_t btn_t0;         /* CYCCNT at ISR entry */
static uint32_t btn_last;                /* debounce (ISR-only) */

#define STACK_WORDS 512u
static k_task_t t_btn, t_green, t_yellow, t_fpu_a, t_fpu_b, t_stats;
static uint32_t s_btn[STACK_WORDS]    __attribute__((aligned(8)));
static uint32_t s_green[STACK_WORDS]  __attribute__((aligned(8)));
static uint32_t s_yellow[STACK_WORDS] __attribute__((aligned(8)));
static uint32_t s_fpu_a[STACK_WORDS]  __attribute__((aligned(8)));
static uint32_t s_fpu_b[STACK_WORDS]  __attribute__((aligned(8)));
static uint32_t s_stats[STACK_WORDS]  __attribute__((aligned(8)));

__attribute__((format(printf, 1, 2)))
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    k_sem_take(&print_lock);
    kvprintf(fmt, ap);
    k_sem_give(&print_lock);
    va_end(ap);
}

/* ---- button: ISR -> semaphore -> high-prio task -------------------------- */
void EXTI15_10_IRQHandler(void)
{
    board_button_clear_irq();
    uint32_t now = DWT_CYCCNT;
    if (now - btn_last < 20u * (g_cpu_hz / 1000u)) return;   /* 20 ms debounce */
    btn_last = now;
    btn_t0 = now;
    k_sem_give(&btn_sem);                /* wakes 'btn' (prio 1) -> preempts */
}

static void btn_body(void *arg)
{
    (void)arg;
    uint32_t n = 0;
    for (;;) {
        k_sem_take(&btn_sem);
        uint32_t lat = DWT_CYCCNT - btn_t0;
        board_led_toggle(LED_RED);
        say("[btn] press %lu: ISR->task wake in %lu cycles = %lu us\n",
            ++n, lat, lat / (g_cpu_hz / 1000000u));
    }
}

/* ---- heartbeat ----------------------------------------------------------- */
static void green_body(void *arg)
{
    (void)arg;
    for (;;) { board_led_toggle(LED_GREEN); k_sleep_ms(250); }
}

static void yellow_body(void *arg)
{
    (void)arg;
    for (;;) { board_led_toggle(LED_YELLOW); k_sleep_ms(500); }
}

/* ---- FPU context proof ---------------------------------------------------
 * Both tasks run the same deterministic float chain from different seeds and
 * compare against their own first result. The eight accumulators stay live
 * across many timeslice preemptions (and across the other FPU task) — if
 * PendSV mishandled S16-S31 or lazy stacking, the sums would diverge. */
static volatile uint32_t fpu_rounds[2], fpu_fails[2];

static void fpu_body(void *arg)
{
    uint32_t id = (uint32_t)arg;
    float x0 = id ? 2.718282f : 1.618034f;
    float golden = 0.0f;

    for (;;) {
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

        if (fpu_rounds[id] == 0) golden = sum;
        else if (sum != golden) fpu_fails[id]++;
        fpu_rounds[id]++;
        k_sleep_ms(1);                   /* let the peer interleave */
    }
}

/* ---- stats --------------------------------------------------------------- */
static const char *const state_names[] = {
    "inval", "ready", "run  ", "sleep", "block", "zombi"
};

static void stats_body(void *arg)
{
    (void)arg;
    for (;;) {
        k_sleep_ms(5000);
        k_sem_take(&print_lock);
        kprintf("\n-- t=%lu ms, %lu context switches --\n",
                k_now(), k_switch_count);
        kprintf("task     prio state  stack-peak  switched-in\n");
        for (const k_task_t *t = k_task_registry(); t; t = t->all_next)
            kprintf("%s  %u   %s  %lu/%lu   %lu\n",
                    t->name, (unsigned)t->prio, state_names[t->state],
                    k_stack_high_water(t), t->stack_words, t->n_switched_in);
        kprintf("fpu: a=%lu rounds/%lu fail   b=%lu rounds/%lu fail\n\n",
                fpu_rounds[0], fpu_fails[0], fpu_rounds[1], fpu_fails[1]);
        k_sem_give(&print_lock);
    }
}

/* ---- main ---------------------------------------------------------------- */
int main(void)
{
    board_init();
    uart_init(115200);

    kprintf("\n=== h745-rtos ===\n");
    kprintf("preemptive kernel on CM7 @ %u MHz | tick %u Hz | press B1 for the semaphore demo\n",
            (unsigned)(SYS_CLK_HZ / 1000000u), (unsigned)K_TICK_HZ);

    k_sem_init(&print_lock, 1);
    k_sem_init(&btn_sem, 0);
    board_button_init(K_BASEPRI_MASK);   /* button ISR may call k_sem_give */

    k_task_create(&t_btn,    "btn  ", btn_body,    0,           1, s_btn,    STACK_WORDS);
    k_task_create(&t_green,  "green", green_body,  0,           2, s_green,  STACK_WORDS);
    k_task_create(&t_yellow, "yello", yellow_body, 0,           2, s_yellow, STACK_WORDS);
    k_task_create(&t_fpu_a,  "fpu-a", fpu_body,    (void *)0u,  4, s_fpu_a,  STACK_WORDS);
    k_task_create(&t_fpu_b,  "fpu-b", fpu_body,    (void *)1u,  4, s_fpu_b,  STACK_WORDS);
    /* stats sits ABOVE the fpu pair. Each fpu task sleeps only 1 ms per
     * ~15 ms round, so a lower-priority observer runs only when both happen
     * to sleep at once -- measured on hardware: two characters in 20 s. */
    k_task_create(&t_stats,  "stats", stats_body,  0,           3, s_stats,  STACK_WORDS);

    k_start();                           /* never returns */
}
