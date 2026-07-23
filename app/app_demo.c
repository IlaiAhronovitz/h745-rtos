/* Milestone 0 bring-up: LEDs + UART + cycle counter. No kernel yet —
 * this file becomes the RTOS demo once the kernel lands. */
#include "stm32h745.h"
#include "uart.h"
#include "board.h"
#include "kprintf.h"

static void delay_ms(uint32_t ms)
{
    uint32_t t0 = DWT_CYCCNT;
    uint32_t cycles = ms * (SYS_CLK_HZ / 1000u);
    while (DWT_CYCCNT - t0 < cycles) {}
}

int main(void)
{
    board_init();
    uart_init(115200);

    kprintf("\n=== h745-rtos ===\n");
    kprintf("bring-up: CM7 @ %u MHz (HSI), stacks in DTCM, CM4 parked\n",
            SYS_CLK_HZ / 1000000u);

    uint32_t n = 0;
    for (;;) {
        board_led_toggle(LED_GREEN);
        if (++n % 4u == 0) board_led_toggle(LED_YELLOW);
        kprintf("blink %lu  cyccnt=%lu  btn=%d\n", n, DWT_CYCCNT,
                board_button_pressed());
        delay_ms(250);
    }
}
