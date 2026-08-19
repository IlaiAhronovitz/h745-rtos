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

/* ---- MPU (PM0253 §4.6, PDF pp.222-229) ----------------------------------
 * Needed to mark the ETH DMA memory non-cacheable. Field positions are from
 * Table 89 (PDF p.227); the TEX/C/B/S encodings are Table 91 (PDF p.228).
 * What each field should CONTAIN is milestone 2 step G, not defined here. */
#define MPU_TYPE        REG32(0xE000ED90u)
#define MPU_CTRL        REG32(0xE000ED94u)
#define   MPU_CTRL_ENABLE     (1u << 0)
#define   MPU_CTRL_HFNMIENA   (1u << 1)
#define   MPU_CTRL_PRIVDEFENA (1u << 2)  /* keep the default map for everything
                                          * NOT covered by a region -- without
                                          * this, enabling the MPU makes flash
                                          * and DTCM unreachable instantly */
#define MPU_RNR         REG32(0xE000ED98u)  /* which region you are editing */
#define MPU_RBAR        REG32(0xE000ED9Cu)  /* base address, size-aligned    */
#define MPU_RASR        REG32(0xE000EDA0u)  /* attributes + size + enable    */
#define   MPU_RASR_ENABLE   (1u << 0)
#define   MPU_RASR_SIZE_SHIFT  1u   /* region bytes = 2^(SIZE+1)            */
#define   MPU_RASR_B        (1u << 16)
#define   MPU_RASR_C        (1u << 17)
#define   MPU_RASR_S        (1u << 18)
#define   MPU_RASR_TEX_SHIFT  19u
#define   MPU_RASR_AP_SHIFT   24u
#define   MPU_RASR_XN       (1u << 28)

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
#define RCC_AHB1ENR     REG32(RCC_BASE + 0x0D8u)  /* RM0399 PDF p.487 */
#define   AHB1ENR_ETH1MACEN (1u << 15)
#define   AHB1ENR_ETH1TXEN  (1u << 16)
#define   AHB1ENR_ETH1RXEN  (1u << 17)
#define RCC_AHB4ENR     REG32(RCC_BASE + 0x0E0u)
#define   AHB4ENR_GPIOAEN (1u << 0)
#define   AHB4ENR_GPIOBEN (1u << 1)
#define   AHB4ENR_GPIOCEN (1u << 2)
#define   AHB4ENR_GPIODEN (1u << 3)
#define   AHB4ENR_GPIOEEN (1u << 4)
#define   AHB4ENR_GPIOGEN (1u << 6)
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
#define GPIOG ((gpio_t *)0x58021800u)     /* ETH TX_EN/TXD0 live here */

/* Set pin p of port g to alternate function af. Handles the AFR[0]/AFR[1]
 * split (pins 0-7 vs 8-15) so call sites do not have to. */
static inline void gpio_af(gpio_t *g, unsigned p, unsigned af)
{
    g->MODER   = (g->MODER   & ~(3u << (2u * p)))  | (2u << (2u * p));
    g->OSPEEDR = (g->OSPEEDR & ~(3u << (2u * p)))  | (3u << (2u * p));
    g->AFR[p >> 3] = (g->AFR[p >> 3] & ~(0xFu << (4u * (p & 7u))))
                   | (af << (4u * (p & 7u)));
}

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

/* Peripheral mode config: EPIS[2:0] picks the Ethernet PHY interface.
 * RM0399 §13.3.1, PDF p.586. 000 = MII, 100 = RMII, all else reserved. */
#define SYSCFG_PMCR     REG32(SYSCFG_BASE + 0x04u)
#define   PMCR_EPIS_SHIFT   21u
#define   PMCR_EPIS_MASK    (7u << 21)

/* ---------------------- Ethernet MAC (RM0399 §61) ------------------------- */
/* Synopsys DesignWare EQOS core. Base 0x40028000 (board DTS: ethernet@40028000).
 * Only the MDIO management pair is declared here; the DMA/descriptor registers
 * arrive with milestone 2. Field positions below are transcribed from RM0399
 * §61.11, PDF pp.3150-3152 -- positions only. Which GOC encoding means "read",
 * and which CR encoding is legal at 64 MHz, are in the same pages and are
 * deliberately NOT defined here. */
#define ETH_BASE        0x40028000u
#define ETH_MACMDIOAR   REG32(ETH_BASE + 0x200u)  /* MDIO address  (p.3150) */
#define ETH_MACMDIODR   REG32(ETH_BASE + 0x204u)  /* MDIO data     (p.3152) */

#define   MACMDIOAR_MB      (1u << 0)     /* MII busy: set to start, MAC clears */
#define   MACMDIOAR_C45E    (1u << 1)     /* clause 45 enable (leave 0)         */
#define   MACMDIOAR_GOC_SHIFT   2u        /* [3:2]  operation command           */
#define   MACMDIOAR_GOC_MASK    (3u << 2)
#define   MACMDIOAR_SKAP    (1u << 4)     /* skip address packet                */
#define   MACMDIOAR_CR_SHIFT    8u        /* [11:8] CSR clock range -> MDC      */
#define   MACMDIOAR_CR_MASK     (0xFu << 8)
#define   MACMDIOAR_NTC_SHIFT   12u       /* [14:12] number of training clocks  */
#define   MACMDIOAR_RDA_SHIFT   16u       /* [20:16] register/device address    */
#define   MACMDIOAR_RDA_MASK    (0x1Fu << 16)
#define   MACMDIOAR_PA_SHIFT    21u       /* [25:21] physical layer address     */
#define   MACMDIOAR_PA_MASK     (0x1Fu << 21)

#define   MACMDIODR_GD_MASK     0xFFFFu   /* [15:0] the 16 data bits            */

/* ---- MAC / MTL / DMA, milestone 2 step I --------------------------------
 * Every offset and bit position below was read out of RM0399 §61.11; the
 * page number is on each line. "MTL" is the FIFO layer that sits between the
 * MAC and the DMA. Channel 0 is the only one this board uses. */
#define ETH_MACCR       REG32(ETH_BASE + 0x0000u)  /* p.3112 */
#define   MACCR_RE          (1u << 0)              /* receiver enable    */
#define   MACCR_TE          (1u << 1)              /* transmitter enable */
#define   MACCR_CST         (1u << 21)             /* strip the 4-byte FCS
                                                  * from received Ether-
                                                  * type frames (p.3112) */
#define   MACCR_DM          (1u << 13)             /* 1 = full duplex    */
#define   MACCR_FES         (1u << 14)             /* 1 = 100 Mbit/s     */
#define ETH_MACPFR      REG32(ETH_BASE + 0x0008u)  /* p.3118 */
#define   MACPFR_PR         (1u << 0)              /* promiscuous mode   */

#define ETH_MTLTXQOMR   REG32(ETH_BASE + 0x0D00u)  /* p.3101 */
#define   MTLTXQOMR_FTQ     (1u << 0)              /* flush tx queue     */
#define   MTLTXQOMR_TSF     (1u << 1)              /* store and forward  */
#define   MTLTXQOMR_TXQEN_SHIFT  2u                /* [3:2]              */
#define   MTLTXQOMR_TQS_SHIFT    16u               /* [18:16] queue size */
#define ETH_MTLRXQOMR   REG32(ETH_BASE + 0x0D30u)  /* p.3106 */
#define   MTLRXQOMR_FUP     (1u << 3)
#define   MTLRXQOMR_FEP     (1u << 4)
#define   MTLRXQOMR_RSF     (1u << 5)              /* store and forward  */
#define   MTLRXQOMR_RQS_SHIFT    20u               /* [22:20] queue size */

#define ETH_DMAMR       REG32(ETH_BASE + 0x1000u)  /* p.3074 */
#define   DMAMR_SWR         (1u << 0)              /* software reset     */
#define ETH_DMASBMR     REG32(ETH_BASE + 0x1004u)  /* p.3076 */
#define ETH_DMACCR      REG32(ETH_BASE + 0x1100u)  /* p.3078 */
#define ETH_DMACTXCR    REG32(ETH_BASE + 0x1104u)  /* p.3079 */
#define   DMACTXCR_ST       (1u << 0)              /* start transmission */
#define   DMACTXCR_TXPBL_SHIFT   16u               /* [21:16] burst len  */
#define ETH_DMACRXCR    REG32(ETH_BASE + 0x1108u)  /* p.3081 */
#define   DMACRXCR_SR       (1u << 0)              /* start reception    */
#define   DMACRXCR_RBSZ_SHIFT     1u               /* [14:1] buffer size */
#define   DMACRXCR_RXPBL_SHIFT   16u               /* [21:16] burst len  */
#define ETH_DMACTXDLAR  REG32(ETH_BASE + 0x1114u)  /* p.3083 tx ring base */
#define ETH_DMACRXDLAR  REG32(ETH_BASE + 0x111Cu)  /* p.3084 rx ring base */
#define ETH_DMACTXDTPR  REG32(ETH_BASE + 0x1120u)  /* p.3085 tx tail ptr  */
#define ETH_DMACRXDTPR  REG32(ETH_BASE + 0x1128u)  /* p.3086 rx tail ptr  */
#define ETH_DMACTXRLR   REG32(ETH_BASE + 0x112Cu)  /* p.3086 [9:0] TDRL   */
#define ETH_DMACRXRLR   REG32(ETH_BASE + 0x1130u)  /* p.3087 [9:0] RDRL   */
#define ETH_DMACSR      REG32(ETH_BASE + 0x1160u)  /* p.3093 */
#define   DMACSR_RI         (1u << 6)              /* receive interrupt   */
#define   DMACSR_RBU        (1u << 7)              /* rx buffer unavail   */
#define   DMACSR_FBE        (1u << 12)             /* fatal bus error     */
#define   DMACSR_AIS        (1u << 14)
#define   DMACSR_NIS        (1u << 15)

/* ------------------------------ IRQ numbers ------------------------------- */
#define IRQ_USART3      39
#define IRQ_EXTI15_10   40

/* ------------------------------- intrinsics ------------------------------- */
static inline void barrier_dsb(void) { __asm volatile("dsb 0xF" ::: "memory"); }
static inline void barrier_isb(void) { __asm volatile("isb 0xF" ::: "memory"); }
static inline void cpu_wfi(void)     { __asm volatile("wfi"); }

void system_init(void);
