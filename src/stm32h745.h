/* stm32h745.h — hand-written register definitions, only what this project uses.
 * Sources: RM0399 rev 4 (STM32H745/755 reference manual), PM0253 (Cortex-M7
 * programming manual). No vendor headers — every address is cited.
 */
#pragma once
#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

/* System clock: reset defaults on purpose — HSI 64 MHz, all prescalers /1
 * (RM0399 §8.4.3). Raising clocks means the PWR/VOS/SMPS dance; out of scope. */
#define SYS_CLK_HZ 64000000u

/* ------------------------- Cortex-M7 core (PM0253) ------------------------ */
#define SCB_ICSR        REG32(0xE000ED04u)
#define   ICSR_PENDSVSET  (1u << 28)
#define SCB_VTOR        REG32(0xE000ED08u)
#define SCB_SHPR3       REG32(0xE000ED20u)  /* [23:16] PendSV, [31:24] SysTick */
#define SCB_SHCSR       REG32(0xE000ED24u)
#define SCB_CFSR        REG32(0xE000ED28u)  /* UsageFault|BusFault|MemManage   */
#define SCB_HFSR        REG32(0xE000ED2Cu)
#define SCB_MMFAR       REG32(0xE000ED34u)
#define SCB_BFAR        REG32(0xE000ED38u)
#define SCB_CPACR       REG32(0xE000ED88u)

#define SYST_CSR        REG32(0xE000E010u)
#define   SYST_CSR_ENABLE    (1u << 0)
#define   SYST_CSR_TICKINT   (1u << 1)
#define   SYST_CSR_CLKSOURCE (1u << 2)      /* 1 = CPU clock */
#define SYST_RVR        REG32(0xE000E014u)
#define SYST_CVR        REG32(0xE000E018u)

#define NVIC_ISER(n)    REG32(0xE000E100u + 4u * (n))
#define NVIC_IPR8(irq)  (*(volatile uint8_t *)(0xE000E400u + (irq)))

#define DEMCR           REG32(0xE000EDFCu)
#define   DEMCR_TRCENA    (1u << 24)
#define DWT_CTRL        REG32(0xE0001000u)
#define   DWT_CTRL_CYCCNTENA (1u << 0)
#define DWT_CYCCNT      REG32(0xE0001004u)  /* free-running cycle counter */

/* ------------------------------ RCC (RM0399 §8) --------------------------- */
#define RCC_BASE        0x58024400u
#define RCC_AHB4ENR     REG32(RCC_BASE + 0x0E0u)
#define   AHB4ENR_GPIOAEN (1u << 0)
#define   AHB4ENR_GPIOBEN (1u << 1)
#define   AHB4ENR_GPIOCEN (1u << 2)
#define   AHB4ENR_GPIODEN (1u << 3)
#define   AHB4ENR_GPIOEEN (1u << 4)
#define RCC_APB1LENR    REG32(RCC_BASE + 0x0E8u)
#define   APB1LENR_USART3EN (1u << 18)
#define RCC_APB4ENR     REG32(RCC_BASE + 0x0F4u)
#define   APB4ENR_SYSCFGEN  (1u << 1)

/* ----------------------------- GPIO (RM0399 §12) -------------------------- */
typedef struct {
    volatile uint32_t MODER;    /* 00: 00 in, 01 out, 10 AF, 11 analog */
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;     /* [15:0] set, [31:16] reset — atomic  */
    volatile uint32_t LCKR;
    volatile uint32_t AFR[2];   /* 4 bits per pin */
} gpio_t;

#define GPIOA ((gpio_t *)0x58020000u)
#define GPIOB ((gpio_t *)0x58020400u)
#define GPIOC ((gpio_t *)0x58020800u)
#define GPIOD ((gpio_t *)0x58020C00u)
#define GPIOE ((gpio_t *)0x58021000u)

/* ---------------------------- USART (RM0399 §51) -------------------------- */
typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t CR3;
    volatile uint32_t BRR;      /* = usart_ker_ck / baud (OVER8=0) */
    volatile uint32_t GTPR;
    volatile uint32_t RTOR;
    volatile uint32_t RQR;
    volatile uint32_t ISR;
    volatile uint32_t ICR;
    volatile uint32_t RDR;
    volatile uint32_t TDR;
    volatile uint32_t PRESC;
} usart_t;

#define USART3 ((usart_t *)0x40004800u)     /* APB1; kernel clock = rcc_pclk1 by default */
#define   USART_CR1_UE   (1u << 0)
#define   USART_CR1_RE   (1u << 2)
#define   USART_CR1_TE   (1u << 3)
#define   USART_CR1_RXNEIE (1u << 5)
#define   USART_ISR_RXNE (1u << 5)
#define   USART_ISR_TC   (1u << 6)
#define   USART_ISR_TXE  (1u << 7)

/* ------------------- EXTI + SYSCFG (RM0399 §20, §11) ---------------------- */
#define EXTI_BASE       0x58000000u
#define EXTI_RTSR1      REG32(EXTI_BASE + 0x00u)
#define EXTI_FTSR1      REG32(EXTI_BASE + 0x04u)
#define EXTI_C1IMR1     REG32(EXTI_BASE + 0x80u)  /* CM7 interrupt mask     */
#define EXTI_C1PR1      REG32(EXTI_BASE + 0x88u)  /* CM7 pending, w1c       */

#define SYSCFG_BASE     0x58000400u
#define SYSCFG_EXTICR(n) REG32(SYSCFG_BASE + 0x08u + 4u * (n))  /* n = 0..3 */

/* ------------------------------ IRQ numbers ------------------------------- */
#define IRQ_USART3      39
#define IRQ_EXTI15_10   40

/* ------------------------------- intrinsics ------------------------------- */
static inline void barrier_dsb(void) { __asm volatile("dsb 0xF" ::: "memory"); }
static inline void barrier_isb(void) { __asm volatile("isb 0xF" ::: "memory"); }
static inline void cpu_wfi(void)     { __asm volatile("wfi"); }

void system_init(void);
