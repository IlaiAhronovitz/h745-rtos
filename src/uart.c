/* USART3 @ PD8 (TX) / PD9 (RX), AF7 — the Nucleo-144 ST-Link VCP pair (UM2408).
 * Polled TX only; the kernel's log path stays dead simple.
 * Kernel clock: USART234578 defaults to rcc_pclk1 = 64 MHz (RM0399 §9.7.21).
 */
#include "stm32h745.h"
#include "uart.h"

void uart_init(uint32_t baud)
{
    RCC_AHB4ENR  |= AHB4ENR_GPIODEN;
    RCC_APB1LENR |= APB1LENR_USART3EN;
    (void)RCC_APB1LENR;                 /* read back: clock really on before use */

    /* PD8/PD9 -> alternate function 7 */
    GPIOD->MODER  = (GPIOD->MODER & ~((3u << 16) | (3u << 18)))
                                  |  ((2u << 16) | (2u << 18));
    GPIOD->AFR[1] = (GPIOD->AFR[1] & ~((0xFu << 0) | (0xFu << 4)))
                                   |  ((7u  << 0) | (7u  << 4));

    /* USART3 is on D2 APB1, which is NOT the CPU clock once the
     * prescalers stop being /1. */
    USART3->BRR = (g_apb1_hz + baud / 2u) / baud;
    USART3->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

void uart_putc(char c)
{
    while (!(USART3->ISR & USART_ISR_TXE)) {}
    USART3->TDR = (uint8_t)c;
}

void uart_write(const char *s, uint32_t n)
{
    while (n--) uart_putc(*s++);
}

void uart_flush(void)
{
    while (!(USART3->ISR & USART_ISR_TC)) {}
}
