#pragma once
#include <stdint.h>

/* USART3 on PD8/PD9 (AF7) — routed to the ST-Link VCP on the Nucleo-144. */
void uart_init(uint32_t baud);
void uart_putc(char c);
void uart_write(const char *s, uint32_t n);
void uart_flush(void);
