/* HardFault register dump — the difference between "it froze" and a fix.
 * port.s passes the active stack frame; escalated Bus/Usage/MemManage faults
 * all land here too (they stay disabled in SHCSR, so they escalate).
 */
#include "kernel.h"
#include "stm32h745.h"
#include "kprintf.h"
#include "uart.h"
#include "board.h"

void k_fault_dump(uint32_t *frame, uint32_t exc_return)
{
    __asm volatile("cpsid i");

    kprintf("\n*** HARD FAULT ***\n");
    kprintf("  r0=%08lx  r1=%08lx  r2=%08lx   r3=%08lx\n",
            frame[0], frame[1], frame[2], frame[3]);
    kprintf(" r12=%08lx  lr=%08lx  pc=%08lx  psr=%08lx\n",
            frame[4], frame[5], frame[6], frame[7]);
    kprintf("HFSR=%08lx CFSR=%08lx BFAR=%08lx MMFAR=%08lx\n",
            SCB_HFSR, SCB_CFSR, SCB_BFAR, SCB_MMFAR);
    kprintf(" EXC_RETURN=%08lx  task=%s\n",
            exc_return, k_curr ? k_curr->name : "(boot)");
    uart_flush();

    board_led(LED_RED, 1);
    for (;;) {}
}
