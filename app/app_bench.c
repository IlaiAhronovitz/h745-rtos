/* app_bench.c — cross-OS benchmark, h745-rtos flavor.  make APP=bench flash
 *
 * Emits the exact output format that os-comparison/run.py parses. The same
 * two measurements exist as FreeRTOS, ThreadX and Zephyr apps in
 * a separate os-comparison project — same board bring-up, same clock as that
 * comparison used (64 MHz HSI) unless clock_init() has since raised it, same
 * N, so the numbers are directly comparable.
 *
 *   yield-rt : measurer and partner at the same priority, both calling
 *              k_yield(). One iteration = two full context switches.
 *   semwake  : a higher-priority task blocks on S; the measurer gives S and
 *              the wake timestamp is taken as the first instruction the
 *              high task executes. Give -> preempt -> restore -> stamp.
 */
#include <stdint.h>
#include "stm32h745.h"
#include "uart.h"
#include "board.h"
#include "kprintf.h"
#include "kernel.h"
#include "sem.h"

#define N 1000u

static k_sem_t sem_s, sem_d;
static volatile uint32_t wake_cyc;
static volatile int partner_run = 1;

#define STACK_WORDS 512u
static k_task_t t_meas, t_part, t_hi;
static uint32_t s_meas[STACK_WORDS] __attribute__((aligned(8)));
static uint32_t s_part[STACK_WORDS] __attribute__((aligned(8)));
static uint32_t s_hi[STACK_WORDS]   __attribute__((aligned(8)));

typedef struct { uint32_t lo, hi, sum; } stat_t;

static void stat_init(stat_t *s) { s->lo = ~0u; s->hi = 0u; s->sum = 0u; }

static void stat_add(stat_t *s, uint32_t dt)
{
    if (dt < s->lo) s->lo = dt;
    if (dt > s->hi) s->hi = dt;
    s->sum += dt;
}

static void stat_print(const char *name, const stat_t *s)
{
    kprintf("%s min=%lu avg=%lu max=%lu cycles (N=%u)\n",
            name, (unsigned long)s->lo, (unsigned long)(s->sum / N),
            (unsigned long)s->hi, (unsigned)N);
}

static void partner_body(void *arg)
{
    (void)arg;
    while (partner_run) k_yield();
    for (;;) k_sleep_ms(1000);
}

static void hi_body(void *arg)
{
    (void)arg;
    for (;;) {
        k_sem_take(&sem_s);
        wake_cyc = DWT_CYCCNT;           /* first thing after waking */
        k_sem_give(&sem_d);
    }
}

static void meas_body(void *arg)
{
    (void)arg;
    stat_t st;

    kprintf("OSBENCH h745-rtos\n");

    /* -- yield round trip ------------------------------------------------ */
    for (int i = 0; i < 10; i++) k_yield();          /* warm up */
    stat_init(&st);
    for (uint32_t i = 0; i < N; i++) {
        uint32_t t0 = DWT_CYCCNT;
        k_yield();                       /* -> partner -> back here */
        stat_add(&st, DWT_CYCCNT - t0);
    }
    partner_run = 0;
    k_yield();                           /* let the partner park itself */
    stat_print("yield-rt:", &st);

    /* -- semaphore give -> preempt -> wake ------------------------------- */
    k_sleep_ms(2);                       /* let hi park on the semaphore */
    stat_init(&st);
    for (uint32_t i = 0; i < N; i++) {
        uint32_t t0 = DWT_CYCCNT;
        k_sem_give(&sem_s);              /* wakes prio-1 -> preempts here */
        k_sem_take(&sem_d);
        stat_add(&st, wake_cyc - t0);
    }
    stat_print("semwake: ", &st);

    kprintf("OSBENCH done\n");
    uart_flush();
    for (;;) k_sleep_ms(1000);
}

int main(void)
{
    board_init();
    uart_init(115200);
    kprintf("\n=== h745-rtos: APP=bench build ===\n");

    k_sem_init(&sem_s, 0);
    k_sem_init(&sem_d, 0);

    k_task_create(&t_hi,   "hi   ", hi_body,      0, 1, s_hi,   STACK_WORDS);
    k_task_create(&t_meas, "meas ", meas_body,    0, 3, s_meas, STACK_WORDS);
    k_task_create(&t_part, "part ", partner_body, 0, 3, s_part, STACK_WORDS);

    k_start();                           /* never returns */
}
