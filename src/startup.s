/* startup.s — STM32H745 Cortex-M7 boot: vector table, reset handler, C runtime init.
 * References: RM0399 (vector table, Table 143), PM0253 (core, exception model).
 * The CM7 fetches initial MSP + PC from 0x08000000 (BOOT_CM7_ADD0 default).
 */
    .syntax unified
    .cpu cortex-m7
    .thumb

/* ---------------------------------------------------------------- vectors */
    .section .vectors, "a", %progbits
    .align 2
    .global g_vectors
g_vectors:
    .word _estack               /*  0: initial MSP — top of DTCM        */
    .word Reset_Handler         /*  1: reset                            */
    .word NMI_Handler           /*  2                                   */
    .word HardFault_Handler     /*  3                                   */
    .word MemManage_Handler     /*  4                                   */
    .word BusFault_Handler      /*  5                                   */
    .word UsageFault_Handler    /*  6                                   */
    .word 0                     /*  7-10: reserved                      */
    .word 0
    .word 0
    .word 0
    .word SVC_Handler           /* 11: kernel start                     */
    .word DebugMon_Handler      /* 12                                   */
    .word 0                     /* 13: reserved                         */
    .word PendSV_Handler        /* 14: context switch                   */
    .word SysTick_Handler       /* 15: kernel tick                      */

    /* External interrupts, RM0399 Table 143. Positions matter; only the ones
     * this project uses get real names. */
    .word Default_Handler       /*  0: WWDG            */
    .word Default_Handler       /*  1: PVD_AVD         */
    .word Default_Handler       /*  2: TAMP_STAMP      */
    .word Default_Handler       /*  3: RTC_WKUP        */
    .word Default_Handler       /*  4: FLASH           */
    .word Default_Handler       /*  5: RCC             */
    .word Default_Handler       /*  6: EXTI0           */
    .word Default_Handler       /*  7: EXTI1           */
    .word Default_Handler       /*  8: EXTI2           */
    .word Default_Handler       /*  9: EXTI3           */
    .word Default_Handler       /* 10: EXTI4           */
    .word Default_Handler       /* 11: DMA1_Stream0    */
    .word Default_Handler       /* 12: DMA1_Stream1    */
    .word Default_Handler       /* 13: DMA1_Stream2    */
    .word Default_Handler       /* 14: DMA1_Stream3    */
    .word Default_Handler       /* 15: DMA1_Stream4    */
    .word Default_Handler       /* 16: DMA1_Stream5    */
    .word Default_Handler       /* 17: DMA1_Stream6    */
    .word Default_Handler       /* 18: ADC1_2          */
    .word Default_Handler       /* 19: FDCAN1_IT0      */
    .word Default_Handler       /* 20: FDCAN2_IT0      */
    .word Default_Handler       /* 21: FDCAN1_IT1      */
    .word Default_Handler       /* 22: FDCAN2_IT1      */
    .word Default_Handler       /* 23: EXTI9_5         */
    .word Default_Handler       /* 24: TIM1_BRK        */
    .word Default_Handler       /* 25: TIM1_UP         */
    .word Default_Handler       /* 26: TIM1_TRG_COM    */
    .word Default_Handler       /* 27: TIM1_CC         */
    .word Default_Handler       /* 28: TIM2            */
    .word Default_Handler       /* 29: TIM3            */
    .word Default_Handler       /* 30: TIM4            */
    .word Default_Handler       /* 31: I2C1_EV         */
    .word Default_Handler       /* 32: I2C1_ER         */
    .word Default_Handler       /* 33: I2C2_EV         */
    .word Default_Handler       /* 34: I2C2_ER         */
    .word Default_Handler       /* 35: SPI1            */
    .word Default_Handler       /* 36: SPI2            */
    .word Default_Handler       /* 37: USART1          */
    .word Default_Handler       /* 38: USART2          */
    .word USART3_IRQHandler     /* 39: USART3 (VCP)    */
    .word EXTI15_10_IRQHandler  /* 40: EXTI15_10 (user button PC13) */
    .rept 109                   /* 41..149: unused     */
    .word Default_Handler
    .endr

/* ------------------------------------------------------------------ reset */
    .text
    .thumb_func
    .global Reset_Handler
Reset_Handler:
    ldr   r0, =_estack          /* soft resets can arrive with a stale MSP  */
    msr   msp, r0

    /* FPU on before any C code runs — we compile hard-float (PM0253 §4.6):
     * CPACR CP10/CP11 = full access. */
    ldr   r0, =0xE000ED88
    ldr   r1, [r0]
    orr   r1, r1, #(0xF << 20)
    str   r1, [r0]
    dsb
    isb

    /* .data: flash image -> DTCM */
    ldr   r0, =_sdata
    ldr   r1, =_edata
    ldr   r2, =_sidata
1:  cmp   r0, r1
    ittt  lo
    ldrlo r3, [r2], #4
    strlo r3, [r0], #4
    blo   1b

    /* .bss: zero */
    ldr   r0, =_sbss
    ldr   r1, =_ebss
    movs  r2, #0
2:  cmp   r0, r1
    itt   lo
    strlo r2, [r0], #4
    blo   2b

    bl    system_init
    bl    main
3:  b     3b                    /* main never returns */

    .thumb_func
Default_Handler:
    b     Default_Handler

/* Weak defaults — the kernel and app override the ones they own. */
    .weak NMI_Handler
    .thumb_set NMI_Handler, Default_Handler
    .weak HardFault_Handler
    .thumb_set HardFault_Handler, Default_Handler
    .weak MemManage_Handler
    .thumb_set MemManage_Handler, Default_Handler
    .weak BusFault_Handler
    .thumb_set BusFault_Handler, Default_Handler
    .weak UsageFault_Handler
    .thumb_set UsageFault_Handler, Default_Handler
    .weak SVC_Handler
    .thumb_set SVC_Handler, Default_Handler
    .weak DebugMon_Handler
    .thumb_set DebugMon_Handler, Default_Handler
    .weak PendSV_Handler
    .thumb_set PendSV_Handler, Default_Handler
    .weak SysTick_Handler
    .thumb_set SysTick_Handler, Default_Handler
    .weak USART3_IRQHandler
    .thumb_set USART3_IRQHandler, Default_Handler
    .weak EXTI15_10_IRQHandler
    .thumb_set EXTI15_10_IRQHandler, Default_Handler
