/* port.s — Cortex-M7 context-switch core (PM0253 exception model).
 *
 * Task stack layout, must match k_task_create():
 *
 *   task->sp -> [ R4..R11, EXC_RETURN ]            software frame (port.s)
 *               [ R0 R1 R2 R3 R12 LR PC xPSR ]     hardware frame (CPU)
 *               [ S0..S15 FPSCR ... ]              only if FP was active
 *
 * FPU: lazy stacking is enabled out of reset (FPCCR.ASPEN/LSPEN). If the
 * switched-out task had an active FP context, EXC_RETURN bit 4 reads 0 and the
 * CPU stacked S0-S15 itself — S16-S31 are ours to save on top.
 */
    .syntax unified
    .cpu cortex-m7
    .thumb

    .equ KERNEL_BASEPRI, 0xE0       /* == K_BASEPRI_MASK in kernel.h */

    .text

/* SVC — runs exactly once, from k_start(): launch the first task. */
    .thumb_func
    .global SVC_Handler
SVC_Handler:
    ldr   r0, =k_curr
    ldr   r0, [r0]
    ldr   r0, [r0]                  /* first TCB field = saved PSP        */
    ldmia r0!, {r4-r11, lr}         /* software frame; EXC_RETURN into lr */
    msr   psp, r0
    ldr   r0, =_estack              /* boot MSP frame is dead — reclaim   */
    msr   msp, r0
    isb
    bx    lr                        /* unstacks the fabricated hw frame   */

/* PendSV — THE context switch. Configured as the lowest exception priority,
 * so it runs only when every other exception has unwound: it always
 * interrupts pure thread state, never a half-serviced ISR. */
    .thumb_func
    .global PendSV_Handler
PendSV_Handler:
    mrs   r0, psp
    isb
    tst   lr, #0x10                 /* FP context active? (bit 4 == 0)    */
    it    eq
    vstmdbeq r0!, {s16-s31}
    stmdb r0!, {r4-r11, lr}

    ldr   r1, =k_curr
    ldr   r2, [r1]
    str   r0, [r2]                  /* k_curr->sp = saved frame           */

    movs  r0, #KERNEL_BASEPRI       /* ready queues are shared with       */
    msr   basepri, r0               /* SysTick + kernel-aware ISRs        */
    dsb
    isb
    bl    k_switch_context          /* picks the new k_curr               */
    movs  r0, #0
    msr   basepri, r0

    ldr   r1, =k_curr
    ldr   r2, [r1]
    ldr   r0, [r2]
    ldmia r0!, {r4-r11, lr}
    tst   lr, #0x10
    it    eq
    vldmiaeq r0!, {s16-s31}
    msr   psp, r0
    isb
    bx    lr

/* HardFault — hand the faulting frame to C for a register dump. */
    .thumb_func
    .global HardFault_Handler
HardFault_Handler:
    tst   lr, #4                    /* which stack holds the frame?       */
    ite   eq
    mrseq r0, msp
    mrsne r0, psp
    mov   r1, lr
    b     k_fault_dump
