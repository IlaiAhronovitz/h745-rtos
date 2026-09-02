/* Host-simulation shim for stm32h745.h — same symbols, sim-backed storage.
 * Compiled INSTEAD of src/stm32h745.h (include-path order); src/usbh.c is
 * compiled unmodified. Every REG32 access routes through sim_reg32(), which
 * steps the hardware model before returning the cell — so the driver's
 * polling loops (which always touch DWT_CYCCNT or a status register) advance
 * simulated time deterministically, with no threads.
 */
#pragma once
#include <stdint.h>

volatile uint32_t *sim_reg32(uint32_t addr);

#define REG32(addr) (*sim_reg32(addr))

#define SYS_CLK_HZ 64000000u

/* ---- DWT / core ---- */
#define DEMCR           REG32(0xE000EDFCu)
#define   DEMCR_TRCENA    (1u << 24)
#define DWT_CTRL        REG32(0xE0001000u)
#define   DWT_CTRL_CYCCNTENA (1u << 0)
#define DWT_CYCCNT      REG32(0xE0001004u)

/* ---- RCC ---- */
#define RCC_BASE        0x58024400u
#define RCC_CR          REG32(RCC_BASE + 0x000u)
#define   RCC_CR_HSEON      (1u << 16)
#define   RCC_CR_HSERDY     (1u << 17)
#define   RCC_CR_HSEBYP     (1u << 18)
#define   RCC_CR_PLL3ON     (1u << 28)
#define   RCC_CR_PLL3RDY    (1u << 29)
#define RCC_PLLCKSELR   REG32(RCC_BASE + 0x028u)
#define   PLLCKSELR_PLLSRC_MASK  3u
#define   PLLCKSELR_PLLSRC_HSE   2u
#define   PLLCKSELR_DIVM3_SHIFT  20u
#define   PLLCKSELR_DIVM3_MASK   (0x3Fu << 20)
#define RCC_PLLCFGR     REG32(RCC_BASE + 0x02Cu)
#define   PLLCFGR_PLL3FRACEN (1u << 8)
#define   PLLCFGR_PLL3VCOSEL (1u << 9)
#define   PLLCFGR_PLL3RGE_MASK (3u << 10)
#define   PLLCFGR_DIVQ3EN    (1u << 23)
#define RCC_PLL3DIVR    REG32(RCC_BASE + 0x040u)
#define RCC_D2CCIP2R    REG32(RCC_BASE + 0x054u)
#define   D2CCIP2R_USBSEL_MASK  (3u << 20)
#define   D2CCIP2R_USBSEL_PLL3Q (2u << 20)
#define RCC_AHB1ENR     REG32(RCC_BASE + 0x0D8u)
#define   AHB1ENR_ETH1MACEN (1u << 15)
#define   AHB1ENR_ETH1TXEN  (1u << 16)
#define   AHB1ENR_ETH1RXEN  (1u << 17)
#define   AHB1ENR_USB2OTGEN (1u << 27)
#define RCC_AHB4ENR     REG32(RCC_BASE + 0x0E0u)
#define   AHB4ENR_GPIOAEN (1u << 0)
#define   AHB4ENR_GPIOBEN (1u << 1)
#define   AHB4ENR_GPIOCEN (1u << 2)
#define   AHB4ENR_GPIODEN (1u << 3)

/* ---- DBGMCU / PWR ---- */
#define DBGMCU_CR       REG32(0x5C001004u)
#define   DBGMCU_CR_DBGSLPD1  (1u << 0)
#define   DBGMCU_CR_DBGSTPD1  (1u << 1)
#define   DBGMCU_CR_D1DBGCKEN (1u << 21)
#define   DBGMCU_CR_D3DBGCKEN (1u << 22)
#define PWR_CR3         REG32(0x58024800u + 0x00Cu)
#define   PWR_CR3_USB33DEN  (1u << 24)
#define   PWR_CR3_USBREGEN  (1u << 25)
#define   PWR_CR3_USB33RDY  (1u << 26)

/* ---- GPIO: plain sim structs, storage only ---- */
typedef struct {
    volatile uint32_t MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR, BSRR, LCKR;
    volatile uint32_t AFR[2];
} gpio_t;

extern gpio_t sim_gpioa, sim_gpiod;
#define GPIOA (&sim_gpioa)
#define GPIOD (&sim_gpiod)

static inline void gpio_af(gpio_t *g, unsigned p, unsigned af)
{
    g->MODER   = (g->MODER   & ~(3u << (2u * p)))  | (2u << (2u * p));
    g->OSPEEDR = (g->OSPEEDR & ~(3u << (2u * p)))  | (3u << (2u * p));
    g->AFR[p >> 3] = (g->AFR[p >> 3] & ~(0xFu << (4u * (p & 7u))))
                   | (af << (4u * (p & 7u)));
}

#define IRQ_OTG_FS 101

static inline void barrier_dsb(void) {}
static inline void barrier_isb(void) {}
static inline void cpu_wfi(void)     {}

void system_init(void);
