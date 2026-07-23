#pragma once
#include <stdint.h>

/* Nucleo-H745ZI-Q (UM2408): LD1 green PB0, LD2 yellow PE1, LD3 red PB14,
 * user button B1 on PC13 (external pull-down, reads high while pressed). */
enum { LED_GREEN = 0, LED_YELLOW = 1, LED_RED = 2 };

void board_init(void);
void board_led(unsigned led, int on);
void board_led_toggle(unsigned led);

/* B1 -> EXTI13, rising edge, EXTI15_10 IRQ at the given NVIC priority.
 * The ISR itself (EXTI15_10_IRQHandler) belongs to the app. */
void board_button_init(uint8_t nvic_prio);
int  board_button_pressed(void);
void board_button_clear_irq(void);
