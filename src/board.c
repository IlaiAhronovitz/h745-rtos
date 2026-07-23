#include "stm32h745.h"
#include "board.h"

typedef struct { gpio_t *port; unsigned pin; } led_t;

static const led_t leds[] = {
    { GPIOB, 0 },   /* LD1 green  */
    { GPIOE, 1 },   /* LD2 yellow */
    { GPIOB, 14 },  /* LD3 red    */
};

void board_init(void)
{
    RCC_AHB4ENR |= AHB4ENR_GPIOBEN | AHB4ENR_GPIOCEN | AHB4ENR_GPIOEEN;
    (void)RCC_AHB4ENR;

    for (unsigned i = 0; i < 3; i++) {
        gpio_t *g = leds[i].port;
        unsigned p = leds[i].pin;
        g->MODER = (g->MODER & ~(3u << (2u * p))) | (1u << (2u * p));
    }

    /* PC13 input (H7 GPIO resets to analog mode — must clear it) */
    GPIOC->MODER &= ~(3u << 26);
}

void board_led(unsigned led, int on)
{
    gpio_t *g = leds[led].port;
    g->BSRR = on ? (1u << leds[led].pin) : (1u << (leds[led].pin + 16u));
}

void board_led_toggle(unsigned led)
{
    leds[led].port->ODR ^= (1u << leds[led].pin);
}

void board_button_init(uint8_t nvic_prio)
{
    RCC_APB4ENR |= APB4ENR_SYSCFGEN;
    (void)RCC_APB4ENR;

    /* EXTI line 13 <- port C (SYSCFG_EXTICR4 field [7:4] = 0b0010) */
    SYSCFG_EXTICR(3) = (SYSCFG_EXTICR(3) & ~(0xFu << 4)) | (2u << 4);
    EXTI_RTSR1  |= (1u << 13);          /* rising edge = button press  */
    EXTI_C1IMR1 |= (1u << 13);          /* unmask toward this core     */

    NVIC_IPR8(IRQ_EXTI15_10) = nvic_prio;
    NVIC_ISER(1) = (1u << (IRQ_EXTI15_10 - 32));
}

int board_button_pressed(void)
{
    return (GPIOC->IDR >> 13) & 1u;
}

void board_button_clear_irq(void)
{
    EXTI_C1PR1 = (1u << 13);            /* write-1-to-clear */
}
