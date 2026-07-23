/* Boot-time chip setup. Clocks stay at reset defaults on purpose:
 * HSI 64 MHz, every prescaler /1 (RM0399 §8.4.3). Raising to 480 MHz needs the
 * H7 PWR sequence (VOS + SMPS-vs-LDO on the -Q board) — a documented stretch
 * goal, not part of the minimal kernel.
 */
#include "stm32h745.h"

extern uint32_t g_vectors[];

void system_init(void)
{
    SCB_VTOR = (uint32_t)g_vectors;   /* explicit — don't rely on boot mapping */

    /* Free-running cycle counter: all timing measurements hang off this. */
    DEMCR |= DEMCR_TRCENA;
    DWT_CYCCNT = 0;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;
}
