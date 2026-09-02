/* Boot-time chip setup. Clocks stay at reset defaults on purpose:
 * HSI 64 MHz, every prescaler /1 (RM0399 §8.4.3). Raising to 480 MHz needs the
 * H7 PWR sequence (VOS + SMPS-vs-LDO on the -Q board) — a documented stretch
 * goal, not part of the minimal kernel.
 */
#include "stm32h745.h"

extern uint32_t g_vectors[];

/* Live clock rates. Start at what reset gives us so that every consumer is
 * correct even if clock_init() declines to switch. */
uint32_t g_cpu_hz  = HSI_CLK_HZ;
uint32_t g_apb1_hz = HSI_CLK_HZ;     /* reset: every prescaler is /1 */
uint32_t g_idcode;                   /* device + revision, read at boot */
/* Why the clock ended up where it did. Bit 0 rev V, 1 scale-1 ready,
 * 2 overdrive ready, 3 the 480 PLL locked, 4 the 480 switch took. */
uint32_t g_clk_diag;
uint32_t g_pwr_csr1, g_pwr_d3cr, g_pwr_cr3;  /* raw, for diagnosis */

/* Bounded wait for a bit to reach a value. No timebase exists yet, so this
 * counts loop iterations -- generous, and only ever used during boot. */
static int wait_bit(volatile uint32_t *reg, uint32_t mask, uint32_t want)
{
    for (uint32_t i = 0; i < 2000000u; i++)
        if ((*reg & mask) == want) return 0;
    return -1;
}

/* HSE 8 MHz -> PLL1 -> sys_ck 200 MHz.
 *
 * Order matters and the failure path matters more:
 *
 *  - Flash wait states go UP BEFORE the clock does. Raising sys_ck first would
 *    execute the very next instruction out of under-waited flash.
 *  - Bus prescalers are set BEFORE the switch, so AHB and APB never see a
 *    moment at 200 MHz. At reset they are all /1, which at 200 MHz would be
 *    far past their limits.
 *  - PLLSRC is shared by all three PLLs and is only writable while every PLL
 *    is off. usbh.c's PLL3 setup relies on that and says so; because this runs
 *    first and selects HSE, usbh.c's later write is a no-op for that field and
 *    its DIVM3 write still lands (PLL3 is off at the time).
 *  - Every wait is bounded. If HSE or the PLL never comes up we return -1
 *    having changed nothing that matters, the chip stays on HSI, and g_cpu_hz
 *    keeps saying 64 MHz so the console still works to say so.
 *
 * 200 MHz is the ceiling at the reset voltage scale. Going higher means VOS1
 * and the PWR supply dance, which on the -Q board involves the SMPS -- a
 * deliberate stopping point, not an oversight. */
/* Raise VCORE as far as the silicon allows.
 * Scale 1 is a register write. Above it, 480 MHz needs the SYSCFG overdrive
 * (VOS0), which exists only on revision V. Returns 1 if the overdrive is on. */
static int voltage_max(int rev_v)
{
    /* The regulator will not move until the SUPPLY configuration is settled.
     * PWR_CR3 keeps its reset value here (SMPS feeding the LDO), which is
     * valid on both board variants -- but ACTVOSRDY still has to be waited
     * for before a VOS write means anything. Skipping this is why the first
     * attempt at Scale 1 silently did nothing. */
    /* The supply topology latches on the first write to PWR_CR3 after reset,
     * and ACTVOSRDY stays low until it has. Write back what is ALREADY there
     * rather than choosing a topology: the reset value is valid on both the
     * SMPS and LDO board variants, and picking the wrong one browns out the
     * core. This only commits the existing configuration. */
    g_pwr_cr3 = PWR_CR3;
    if (!(PWR_CSR1 & PWR_CSR1_ACTVOSRDY)) {
        /* Rewriting the same value does not latch it; the supply has to be
         * selected EXPLICITLY. This board is a NUCLEO-H745ZI-Q -- the -Q
         * suffix is the SMPS variant, and ST's own generated code for it
         * uses direct SMPS. Wrong here would brown the core out, so it is
         * gated on ACTVOSRDY being genuinely absent and everything below
         * still falls back to the Scale-3 200 MHz path that already works. */
        PWR_CR3 = (PWR_CR3 & ~PWR_CR3_SUPPLY_MASK) | PWR_CR3_SDEN;
        (void)wait_bit(&PWR_CSR1, PWR_CSR1_ACTVOSRDY, PWR_CSR1_ACTVOSRDY);
    }
    g_pwr_csr1 = PWR_CSR1;
    if (PWR_CSR1 & PWR_CSR1_ACTVOSRDY) g_clk_diag |= 32u;

    PWR_D3CR = (PWR_D3CR & ~PWR_D3CR_VOS_MASK) | PWR_D3CR_VOS_SCALE1;
    int r = wait_bit(&PWR_D3CR, PWR_D3CR_VOSRDY, PWR_D3CR_VOSRDY);
    g_pwr_d3cr = PWR_D3CR;
    if (r != 0) return 0;
    g_clk_diag |= 2u;
    if (!rev_v) return 0;

    RCC_APB4ENR |= APB4ENR_SYSCFGEN;
    (void)RCC_APB4ENR;                      /* let the enable land */
    SYSCFG_PWRCR |= SYSCFG_PWRCR_ODEN;
    (void)SYSCFG_PWRCR;
    if (wait_bit(&PWR_D3CR, PWR_D3CR_VOSRDY, PWR_D3CR_VOSRDY) != 0) return 0;
    g_clk_diag |= 4u;
    return 1;
}

/* Configure and start PLL1, then switch sys_ck to it. divn/divp are the real
 * ratios; the register stores them biased by one. Leaves PLL1 off and the
 * clock untouched if anything fails to come ready. */
static int pll1_switch(uint32_t divm, uint32_t divn, uint32_t divp,
                       uint32_t rge, uint32_t hpre, uint32_t ppre)
{
    RCC_CR &= ~RCC_CR_PLL1ON;
    if (wait_bit(&RCC_CR, RCC_CR_PLL1RDY, 0) != 0) return -1;

    RCC_PLLCKSELR = (RCC_PLLCKSELR & ~(PLLCKSELR_PLLSRC_MASK | PLLCKSELR_DIVM1_MASK))
                  | PLLCKSELR_PLLSRC_HSE
                  | (divm << PLLCKSELR_DIVM1_SHIFT);
    RCC_PLLCFGR = (RCC_PLLCFGR & ~(PLLCFGR_PLL1RGE_MASK | PLLCFGR_PLL1VCOSEL
                                   | PLLCFGR_PLL1FRACEN))
                | rge                       /* wide VCO: VCOSEL stays 0 */
                | PLLCFGR_DIVP1EN;
    RCC_PLL1DIVR = (RCC_PLL1DIVR & ~0xFFFFu) | (divn - 1u) | ((divp - 1u) << 9);

    RCC_CR |= RCC_CR_PLL1ON;
    if (wait_bit(&RCC_CR, RCC_CR_PLL1RDY, RCC_CR_PLL1RDY) != 0) return -1;

    /* prescalers BEFORE the switch, so no bus ever sees the full rate */
    RCC_D1CFGR = D1CFGR_D1CPRE_DIV1 | hpre | (ppre << 4);
    RCC_D2CFGR = (ppre << 4) | (ppre << 8);
    RCC_D3CFGR = (ppre << 4);

    RCC_CFGR = (RCC_CFGR & ~CFGR_SW_MASK) | CFGR_SW_PLL1;
    return wait_bit(&RCC_CFGR, CFGR_SWS_MASK,
                    CFGR_SW_PLL1 << CFGR_SWS_SHIFT);
}

static int clock_init(void)
{
    int rev_v = ((DBGMCU_IDC >> 16) == 0x2003u);   /* revision V reaches 480 */
    if (rev_v) g_clk_diag |= 1u;

    /* Flash wait states go up BEFORE the clock does, and are set for the
     * FASTEST target we might reach. More wait states than needed is always
     * safe; fewer means the next instruction fetch is out of under-waited
     * flash, which is unrecoverable. */
    FLASH_ACR = (FLASH_ACR & ~(FLASH_ACR_LATENCY_MASK | FLASH_ACR_WRHIGHFREQ_MASK))
              | 4u | (2u << 4);
    if (wait_bit(&FLASH_ACR, FLASH_ACR_LATENCY_MASK, 4u) != 0) return -1;

    /* HSE from the ST-LINK's 8 MHz MCO. HSEBYP is writable only with HSE off,
     * so it goes first. Idempotent: usbh.c does the same for PLL3. */
    if (!(RCC_CR & RCC_CR_HSERDY)) {
        RCC_CR |= RCC_CR_HSEBYP;
        RCC_CR |= RCC_CR_HSEON;
        if (wait_bit(&RCC_CR, RCC_CR_HSERDY, RCC_CR_HSERDY) != 0) return -1;
    }

    int overdrive = voltage_max(rev_v);

    /* 480: ref 8/2 = 4 MHz (RGE 4-8), VCO 4 x 240 = 960, P /2.
     *      CPU 480, AHB /2 = 240, APB /4 = 120. Needs VOS0. */
    if (overdrive) {
        if (pll1_switch(2u, 240u, 2u, (2u << 2), D1CFGR_HPRE_DIV2, 4u) == 0)
            g_clk_diag |= 8u | 16u;
    }
    if (g_clk_diag & 16u) {
        g_cpu_hz  = 480000000u;
        g_apb1_hz = 120000000u;
        return 0;
    }

    /* Fall back to the configuration already proven on this board: ref 8 MHz
     * (RGE 8-16), VCO 800, P /4 -> 200 MHz, AHB 100, APB 50. Reached either
     * because the silicon is not revision V or because 480 would not lock. */
    if (pll1_switch(1u, 100u, 4u, PLLCFGR_PLL1RGE_8_16,
                    D1CFGR_HPRE_DIV2, 4u) == 0) {
        g_cpu_hz  = SYS_CLK_HZ_TARGET;
        g_apb1_hz = 50000000u;
        return 0;
    }
    return -1;      /* nothing switched: the chip stays on HSI and says so */
}

void system_init(void)
{
    SCB_VTOR = (uint32_t)g_vectors;   /* explicit — don't rely on boot mapping */

    /* Which silicon this is decides the top speed: revision V reaches
     * 480 MHz, earlier revisions stop at 400. Read it before anyone asks. */
    g_idcode = DBGMCU_IDC;

    /* Before anything measures time or talks to a peripheral. */
    (void)clock_init();

    /* Free-running cycle counter: all timing measurements hang off this. */
    DEMCR |= DEMCR_TRCENA;
    DWT_CYCCNT = 0;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;

    /* Debug stays reachable while running/sleeping: D1/D3 debug clocks on,
     * debug-in-sleep on. Without this a live attach (gdb / Cortex-Debug
     * "attach") times out on halt and only connect-under-reset works. */
    DBGMCU_CR |= DBGMCU_CR_DBGSLPD1 | DBGMCU_CR_DBGSTPD1
               | DBGMCU_CR_D1DBGCKEN | DBGMCU_CR_D3DBGCKEN;
}
