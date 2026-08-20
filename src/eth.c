/* eth.c — Ethernet driver for the STM32H745, bare metal.
 *
 * The MAC is a Synopsys DesignWare EQOS core driving an on-board Microchip
 * LAN8742A PHY over RMII. No HAL, no CMSIS, no vendor stack: every register
 * used here is hand-declared in stm32h745.h and cited to RM0399.
 *
 * Bring-up order matters, and the call sequence below is not arbitrary:
 *
 *     eth_mem_init()     MPU marks the DMA region non-cacheable
 *     eth_pins_init()    RMII pin routing + SYSCFG mode select
 *     eth_mdio_init()    MAC clock and MDC divider
 *     eth_rings_init()   descriptor rings
 *     ... wait for link, read speed/duplex from the PHY ...
 *     eth_start(speed, duplex)
 *
 * eth_pins_init() must precede eth_mdio_init() because the RMII/MII selection
 * only latches while the MAC clock is off. eth_start() must come last because
 * the MAC's speed and duplex have to match what the PHY negotiated.
 *
 * Reference documents and page numbers: docs/refs/INDEX.md.
 */
#include "stm32h745.h"
#include "eth.h"
#include "libmin.h"

/* Descriptor rings and packet buffers.
 *
 * These live in D2 SRAM3 rather than DTCM for a hard reason: the Ethernet DMA
 * cannot reach DTCM at all. aligned(32) matches the Cortex-M7 cache line, which
 * keeps each ring on a clean boundary; the DMA itself only requires word
 * alignment, but 32 costs nothing and avoids sharing a line with anything else.
 */
#define ETH_RAM __attribute__((section(".eth_ram"), aligned(32)))

ETH_RAM eth_desc_t eth_rx_desc[ETH_RX_DESC_COUNT];
ETH_RAM eth_desc_t eth_tx_desc[ETH_TX_DESC_COUNT];
ETH_RAM uint8_t    eth_rx_buf[ETH_RX_DESC_COUNT][ETH_BUF_SIZE];
ETH_RAM uint8_t    eth_tx_buf[ETH_TX_DESC_COUNT][ETH_BUF_SIZE];

/* Set if the DMA software reset never completed. Exposed rather than hidden
 * because it is the single most useful diagnostic during bring-up: a stuck SWR
 * means the MAC is not receiving its RMII reference clock, and every register
 * written afterwards lands in a peripheral still held in reset. */
unsigned eth_swr_timed_out;

/* ==========================================================================
 * Memory: make SRAM3 safe for DMA
 *
 * The Cortex-M7 has a data cache, and that becomes a problem the moment a
 * second bus master touches the same memory. A descriptor written by the CPU
 * can sit in cache and never reach RAM, so the DMA reads a stale one. A frame
 * written by the DMA straight into RAM can be shadowed by an older cached
 * copy, so the CPU reads stale data. Two masters, one memory, no automatic
 * agreement.
 *
 * The fix here is the blunt one: an MPU region marking these 32 KB as Normal,
 * shareable, non-cacheable. Every access then goes to real RAM. Slower than
 * cached, and unconditionally correct -- the right trade for a few kilobytes
 * of descriptors. The alternative, cache maintenance around every descriptor
 * access, is faster and considerably easier to get subtly wrong.
 * ========================================================================== */
void eth_mem_init(void)
{
    /* Region 0; nothing else in this project uses the MPU.
     * The base address must be aligned to the region size, and 0x30040000 is
     * a multiple of 32 KB -- by design, not by luck. */
    MPU_RNR  = 0u;
    MPU_RBAR = ETH_RAM_BASE;

    /* Attributes, PM0253 Table 89 (p.227) and Table 91 (p.228).
     * TEX=001 with C=0 and B=0 is the encoding for "Normal, non-cacheable";
     * C and B are deliberately left clear rather than written as zero. */
    uint32_t rasr = 0u;
    rasr |= 1u  << MPU_RASR_TEX_SHIFT;   /* TEX = 0b001, with C=B=0           */
    rasr |= MPU_RASR_S;                  /* shareable                         */
    rasr |= 3u  << MPU_RASR_AP_SHIFT;    /* AP  = 0b011, full read/write      */
    rasr |= MPU_RASR_XN;                 /* never fetch instructions here     */
    rasr |= 14u << MPU_RASR_SIZE_SHIFT;  /* 2^(14+1) = 32 KB                  */
    rasr |= MPU_RASR_ENABLE;
    MPU_RASR = rasr;

    /* PRIVDEFENA is not optional. Without it the only accessible memory is the
     * regions defined above, so flash, DTCM and every peripheral disappear and
     * the core faults on its next instruction fetch. With it, the default
     * memory map stays in force everywhere this region does not cover.
     *
     * The barriers ensure the MPU change has taken effect and the pipeline has
     * been refilled before any access is allowed to use the new rules. */
    MPU_CTRL = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;
    barrier_dsb();
    barrier_isb();
}

/* ==========================================================================
 * Pins: route the RMII data path and select RMII mode
 *
 * Nine pins in total, all alternate function 11 (DS12923 Table 9, p.86). Two
 * of them, PA2 and PC1, are the MDIO management pair and are configured in
 * eth_mdio_init(); the seven here are the data path.
 * ========================================================================== */
void eth_pins_init(void)
{
    /* board_init() enables GPIOB, GPIOC and GPIOE. GPIOA and GPIOG are not,
     * and writes to an unclocked GPIO are silently discarded -- so the clocks
     * must come first, in this function, before any pin is touched. */
    RCC_APB4ENR |= APB4ENR_SYSCFGEN;
    (void)RCC_APB4ENR;
    RCC_AHB4ENR |= AHB4ENR_GPIOAEN | AHB4ENR_GPIOGEN;

    /* EPIS = 100 selects RMII (RM0399 section 13.3.1, p.586).
     *
     * This must happen while the MAC clock is still off, which is why this
     * function runs before eth_mdio_init(). Set it afterwards and the MAC
     * stays in MII mode with no error reported anywhere -- the pins simply
     * never carry traffic. */
    SYSCFG_PMCR &= ~PMCR_EPIS_MASK;
    SYSCFG_PMCR |= (4u << PMCR_EPIS_SHIFT);

    /* Board wiring, from the nucleo_h745zi_q device tree:
     *   PA1  REF_CLK    PA7  CRS_DV
     *   PC4  RXD0       PC5  RXD1
     *   PG11 TX_EN      PG13 TXD0      PB13 TXD1                              */
    gpio_af(GPIOA, 1, 11);
    gpio_af(GPIOA, 7, 11);
    gpio_af(GPIOC, 4, 11);
    gpio_af(GPIOC, 5, 11);
    gpio_af(GPIOG, 11, 11);
    gpio_af(GPIOG, 13, 11);
    gpio_af(GPIOB, 13, 11);
}

/* ==========================================================================
 * MDIO: the management interface
 *
 * Two wires, entirely separate from the data path, used to configure and
 * interrogate the PHY. Slow by design -- MDC runs at a couple of MHz.
 * ========================================================================== */
void eth_mdio_init(void)
{
    /* Only the MAC clock is needed here. The TX and RX clocks belong to the
     * data path and are enabled later, in eth_start(). The read-back forces
     * the buffered AHB write to land before the next peripheral access. */
    RCC_AHB1ENR |= AHB1ENR_ETH1MACEN;
    (void)RCC_AHB1ENR;

    /* PA2 = MDIO, PC1 = MDC. GPIOA's clock came from eth_pins_init(). */
    RCC_AHB4ENR |= AHB4ENR_GPIOAEN;
    gpio_af(GPIOA, 2, 11);
    gpio_af(GPIOC, 1, 11);

    /* MDC is HCLK divided by the CR field, and IEEE 802.3 clause 22 caps it at
     * 2.5 MHz. At HCLK = 64 MHz, CR=0000 divides by 42 for 1.52 MHz, which sits
     * mid-range rather than against either edge of the 1.0-2.5 MHz window
     * (RM0399 section 61.11, p.3151).
     *
     * CR=0000 also happens to be the reset value, so this write is a no-op
     * today. It is written explicitly anyway: it documents the intent, and it
     * is the line that must change if HCLK is ever raised. At 400 MHz the same
     * encoding would give 9.5 MHz -- four times over the limit, and the PHY
     * would simply stop answering, with no error reported anywhere. */
    ETH_MACMDIOAR &= ~MACMDIOAR_CR_MASK;
    ETH_MACMDIOAR |= (0u << MACMDIOAR_CR_SHIFT);
}

/* Read one PHY register over MDIO (IEEE 802.3 clause 22).
 *
 * The MAC has a hardware state machine for the protocol: this function
 * describes the transaction, sets the busy bit, and the MAC clocks all 64
 * bit-times out on MDC by itself.
 *
 * Returns 0xFFFF on timeout, which is also what an idle MDIO bus floats to --
 * so "no answer" and "nobody there" read alike, deliberately. */
uint16_t eth_mdio_read(uint8_t phy_addr, uint8_t reg)
{
    /* Preserve CR, replace everything else. GOC=11 is a read (p.3151). */
    uint32_t v = ETH_MACMDIOAR & MACMDIOAR_CR_MASK;
    v |= (uint32_t)phy_addr << MACMDIOAR_PA_SHIFT;
    v |= (uint32_t)reg      << MACMDIOAR_RDA_SHIFT;
    v |= 3u << MACMDIOAR_GOC_SHIFT;
    v |= MACMDIOAR_MB;

    ETH_MACMDIOAR = v;

    /* MB has two jobs: writing 1 starts the transaction, and the MAC clears it
     * when the PHY has answered. The poll is bounded because an unbounded wait
     * on hardware turns a configuration mistake into a silent lockup. */
    uint32_t guard = 100000u;
    while ((ETH_MACMDIOAR & MACMDIOAR_MB) && guard) {
        guard--;
    }

    if (guard == 0u) {
        return 0xFFFFu;
    }

    return (uint16_t)(ETH_MACMDIODR & MACMDIODR_GD_MASK);
}

/* ==========================================================================
 * Descriptor rings
 *
 * A ring is an array of descriptors, each pointing at one buffer. The OWN bit
 * in des3 is the entire handshake between CPU and DMA:
 *
 *     OWN = 1   the DMA owns this descriptor
 *     OWN = 0   the CPU owns it
 *
 * No locks and no interrupts are needed, because the two sides never hold the
 * same descriptor at once.
 *
 * At startup every RX descriptor is handed to the DMA so it has somewhere to
 * put arriving frames, and every TX descriptor is kept by the CPU because
 * there is nothing queued to send.
 * ========================================================================== */
void eth_rings_init(void)
{
    for (unsigned i = 0; i < ETH_RX_DESC_COUNT; i++) {
        eth_rx_desc[i].des0 = (uint32_t)(uintptr_t)eth_rx_buf[i];
        eth_rx_desc[i].des1 = 0u;
        eth_rx_desc[i].des2 = 0u;   /* no second buffer */

        /* des3 last, always. The instant OWN goes to 1 the DMA may act on this
         * descriptor, so the buffer address must already be valid. */
        eth_rx_desc[i].des3 = RDES3_OWN | RDES3_BUF1V | RDES3_IOC;
    }

    for (unsigned i = 0; i < ETH_TX_DESC_COUNT; i++) {
        eth_tx_desc[i].des0 = 0u;
        eth_tx_desc[i].des1 = 0u;
        eth_tx_desc[i].des2 = 0u;
        eth_tx_desc[i].des3 = 0u;   /* OWN = 0: the CPU owns all of them */
    }
}

/* ==========================================================================
 * Start the MAC and DMA
 *
 * A fixed initialisation sequence from RM0399 section 61.9. Order-sensitive:
 * the DMA software reset clears the MTL and DMA register banks, so nothing may
 * be configured before it completes.
 *
 * speed100 and full_duplex come from the PHY's negotiated result, so this must
 * not be called until the link is up.
 * ========================================================================== */
void eth_start(unsigned speed100, unsigned full_duplex)
{
    /* The data path needs the TX and RX clocks that MDIO did not. */
    RCC_AHB1ENR |= AHB1ENR_ETH1TXEN | AHB1ENR_ETH1RXEN;
    (void)RCC_AHB1ENR;

    /* SWR clears itself when the reset completes. It will never clear if the
     * MAC is not receiving its RMII reference clock, so the wait is bounded
     * and the outcome recorded rather than hung on. */
    ETH_DMAMR |= DMAMR_SWR;
    {
        uint32_t guard = 1000000u;
        while ((ETH_DMAMR & DMAMR_SWR) && guard) {
            guard--;
        }
        eth_swr_timed_out = (guard == 0u);
    }

    /* MTL is the FIFO layer between MAC and DMA. Store-and-forward on both
     * sides: a frame is held complete in the FIFO before it moves. Slightly
     * higher latency than cut-through, and it means a frame never starts
     * moving until it is known to be whole. */
    ETH_MTLTXQOMR = MTLTXQOMR_TSF
                  | (1u << MTLTXQOMR_TXQEN_SHIFT)   /* enable TX queue 0     */
                  | (7u << MTLTXQOMR_TQS_SHIFT);    /* all of the FIFO       */
    ETH_MTLRXQOMR = MTLRXQOMR_RSF
                  | MTLRXQOMR_FEP                   /* forward error packets */
                  | MTLRXQOMR_FUP
                  | (7u << MTLRXQOMR_RQS_SHIFT);

    /* Ring geometry. RDRL/TDRL are length-minus-one; RBSZ sits at bit 1. */
    ETH_DMACTXDLAR = (uint32_t)(uintptr_t)eth_tx_desc;
    ETH_DMACRXDLAR = (uint32_t)(uintptr_t)eth_rx_desc;
    ETH_DMACTXRLR  = ETH_TX_DESC_COUNT - 1u;
    ETH_DMACRXRLR  = ETH_RX_DESC_COUNT - 1u;
    ETH_DMACRXCR   = (ETH_BUF_SIZE << DMACRXCR_RBSZ_SHIFT)
                   | (32u << DMACRXCR_RXPBL_SHIFT);
    ETH_DMACTXCR   = (32u << DMACTXCR_TXPBL_SHIFT);

    /* The tail pointer marks one past the last descriptor handed over. */
    ETH_DMACRXDTPR = (uint32_t)(uintptr_t)&eth_rx_desc[ETH_RX_DESC_COUNT - 1u];
    ETH_DMACTXDTPR = (uint32_t)(uintptr_t)eth_tx_desc;

    /* Speed and duplex must match the negotiated link.
     *
     * CST strips the 4-byte FCS from received frames. Without it every frame
     * arrives four bytes longer than it really is, and anything length-
     * sensitive above -- a checksum, a payload copy -- is quietly wrong.
     *
     * Promiscuous mode accepts every frame on the wire, not only those
     * addressed to this MAC. Useful for observing traffic; tighten it if this
     * driver is ever put somewhere busy. */
    uint32_t cr = 0u;
    if (speed100)    cr |= MACCR_FES;
    if (full_duplex) cr |= MACCR_DM;
    ETH_MACCR  = cr | MACCR_RE | MACCR_TE | MACCR_CST;
    ETH_MACPFR = MACPFR_PR;

    ETH_DMACRXCR |= DMACRXCR_SR;
    ETH_DMACTXCR |= DMACTXCR_ST;

    barrier_dsb();
}

/* ==========================================================================
 * Receive
 *
 * Copies at most dst_size bytes of the next frame into dst and returns the
 * frame's true length, or 0 if nothing has arrived.
 *
 * The copy is not optional. The descriptor is handed back to the DMA before
 * this function returns, so a pointer into eth_rx_buf would point at memory
 * the hardware is free to overwrite.
 * ========================================================================== */
uint16_t eth_rx_poll(uint8_t *dst, uint16_t dst_size)
{
    /* static: the ring position must survive between calls. */
    static unsigned rx_index = 0u;

    uint32_t des3 = eth_rx_desc[rx_index].des3;

    if (des3 & RDES3_OWN) {
        return 0u;              /* the DMA still owns it: nothing has arrived */
    }

    /* des3 now holds the write-back format: PL is the packet length in bits
     * 14:0 (RM0399 Table 598, p.3069). The same word means different things
     * depending on which side wrote it last. */
    uint16_t len = (uint16_t)(des3 & RDES3_PL_MASK);

    {
        uint16_t n = (len < dst_size) ? len : dst_size;
        memcpy(dst, eth_rx_buf[rx_index], n);
    }

    /* Re-arm. The DMA overwrote des0 and des3 with status, so both are rebuilt
     * exactly as eth_rings_init() did, des3 last. */
    eth_rx_desc[rx_index].des0 = (uint32_t)(uintptr_t)eth_rx_buf[rx_index];
    eth_rx_desc[rx_index].des3 = RDES3_OWN | RDES3_BUF1V | RDES3_IOC;

    rx_index = (rx_index + 1u) % ETH_RX_DESC_COUNT;

    /* Re-arming is not enough on its own. If the DMA previously ran out of
     * descriptors it latched RBU and suspended, and it does not re-check by
     * itself -- the tail pointer write is what wakes it. Without this the
     * receiver delivers exactly as many frames as the ring is deep, then goes
     * permanently quiet. */
    ETH_DMACSR     = DMACSR_RBU;
    ETH_DMACRXDTPR = (uint32_t)(uintptr_t)&eth_rx_desc[rx_index];

    return len;
}

/* ==========================================================================
 * Transmit
 *
 * The mirror of eth_rx_poll(): there the DMA hands a descriptor back, here the
 * CPU fills one in and hands it over.
 *
 * Returns 0 on success, -1 if the frame is too large or the ring is full.
 * The MAC appends the CRC and pads short frames to the 60-byte minimum, so
 * callers pass only real frame content.
 * ========================================================================== */
int eth_tx_send(const uint8_t *data, uint16_t len)
{
    static unsigned tx_index = 0u;

    if (len > ETH_BUF_SIZE) {
        return -1;              /* will not fit in one buffer */
    }

    uint32_t des3 = eth_tx_desc[tx_index].des3;

    if (des3 & TDES3_OWN) {
        return -1;              /* DMA has not finished with this slot */
    }

    /* The DMA can only read from the non-cacheable SRAM3 region; the caller's
     * data usually lives in DTCM, which it cannot reach at all. */
    memcpy(eth_tx_buf[tx_index], data, len);

    eth_tx_desc[tx_index].des0 = (uint32_t)(uintptr_t)eth_tx_buf[tx_index];
    eth_tx_desc[tx_index].des1 = 0u;
    eth_tx_desc[tx_index].des2 = (uint32_t)(len & TDES2_B1L_MASK);

    /* FD and LD together mean this single descriptor is the whole packet. A
     * frame spanning several buffers would set FD on the first and LD on the
     * last. des2 carries the length of THIS buffer, des3 the length of the
     * whole frame; with one descriptor they are equal. */
    eth_tx_desc[tx_index].des3 = TDES3_OWN | TDES3_FD | TDES3_LD
                               | (len & TDES3_FL_MASK);

    tx_index = (tx_index + 1u) % ETH_TX_DESC_COUNT;

    /* Descriptors are Normal memory; the tail pointer is a Device register.
     * The core is free to reorder writes between those two classes, so without
     * this barrier the doorbell can reach the MAC before OWN is visible. The
     * DMA then finds nothing to send and waits for the next doorbell -- which
     * shows up not as a failure but as latency, whenever something else
     * happens to ring it. */
    barrier_dsb();
    ETH_DMACTXDTPR = (uint32_t)(uintptr_t)&eth_tx_desc[tx_index];

    return 0;
}

/* ==========================================================================
 * Receive interrupt
 *
 * Replaces polling: the DMA raises an interrupt when it has closed a
 * descriptor, instead of the CPU asking repeatedly whether anything arrived.
 * ========================================================================== */
volatile uint32_t eth_rx_irq_count;

/* NULL until the application installs one -- see eth.h. */
void (*eth_rx_notify)(void);

void eth_irq_enable(uint8_t nvic_prio)
{
    /* RIE alone is not enough. NIE is a summary gate: with it clear, receive
     * interrupts are recorded in DMACSR but never reach the NVIC. */
    ETH_DMACIER |= DMACIER_NIE | DMACIER_RIE;

    NVIC_IPR8(IRQ_ETH) = nvic_prio;
    NVIC_ISER(1)       = 1u << (IRQ_ETH - 32u);
}

void ETH_IRQHandler(void)
{
    uint32_t status = ETH_DMACSR;

    if (status & DMACSR_RI) {
        eth_rx_irq_count++;

        /* NULL until the application installs one. Calling through an
         * uninstalled function pointer from an ISR is a hard fault with a
         * singularly unhelpful traceback. */
        if (eth_rx_notify) {
            eth_rx_notify();
        }
    }

    /* Unconditional, and both bits. These are write-1-to-clear, so writing a
     * 1 clears rather than sets -- the same convention as EXTI_C1PR1.
     *
     * The interrupt line stays asserted while the flag is set, so returning
     * without clearing means the NVIC re-enters this handler immediately and
     * forever: no task ever runs again and the board looks dead. NIS is the
     * summary bit that actually drives the line, so clearing only RI is not
     * enough. */
    ETH_DMACSR = DMACSR_RI | DMACSR_NIS;
}
