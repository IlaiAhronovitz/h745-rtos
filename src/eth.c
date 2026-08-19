/* eth.c — STM32H745 Ethernet driver.  MILESTONE 1: MDIO only.
 *
 * Four small steps, in order. Each one is a few lines and each one is visible
 * on the serial console before you move to the next -- so you are never
 * debugging two unknowns at once.
 *
 *   STEP A  turn the MAC clock on          (~3 lines)   <-- start here
 *   STEP B  route two pins to the MAC      (~4 lines)
 *   STEP C  pick the MDC divider           (~2 lines + arithmetic)
 *   STEP D  one MDIO read transaction      (~12 lines)
 *
 * Register addresses and field positions are already in stm32h745.h. What is
 * NOT there, on purpose: which GOC encoding means "read", and which CR encoding
 * is legal at 64 MHz. Those two are the actual thinking, and both answers are
 * in RM0399 §61.11, PDF pp.3150-3152. Local copy: docs/refs/, see INDEX.md.
 */
#include "stm32h745.h"
#include "eth.h"
#include "libmin.h"

/* ---- The rings themselves ------------------------------------------------
 * ETH_RAM puts these in the non-cacheable SRAM3 region you set up in step G.
 * aligned(32) matches the Cortex-M7 cache line and keeps each ring on a clean
 * boundary -- the DMA requires the ring base to be at least word aligned, and
 * 32 costs nothing here.
 */
#define ETH_RAM __attribute__((section(".eth_ram"), aligned(32)))

ETH_RAM eth_desc_t eth_rx_desc[ETH_RX_DESC_COUNT];
ETH_RAM eth_desc_t eth_tx_desc[ETH_TX_DESC_COUNT];
ETH_RAM uint8_t    eth_rx_buf[ETH_RX_DESC_COUNT][ETH_BUF_SIZE];
ETH_RAM uint8_t    eth_tx_buf[ETH_TX_DESC_COUNT][ETH_BUF_SIZE];

/* ==========================================================================
 * STEP H -- hand the RX ring to the DMA, keep the TX ring for yourself.
 *
 * A ring is an array of descriptors. Each descriptor points at one buffer.
 * The OWN bit in des3 says whose turn it is -- exactly like MB in milestone 1,
 * except here there is one per descriptor:
 *
 *     OWN = 1  ->  the DMA owns it. Hands off.
 *     OWN = 0  ->  you own it. Safe to read or refill.
 *
 * At startup you give every RX descriptor to the DMA (so it has somewhere to
 * put arriving frames), and keep every TX descriptor yourself (nothing to
 * send yet).
 * ========================================================================== */
void eth_rings_init(void)
{
    /* ---- STEP H1: fill the RX ring ---------------------------------------
     * Loop over all ETH_RX_DESC_COUNT descriptors. For each one, i:
     *
     *   des0 = the address of eth_rx_buf[i]
     *          -- an array name IS its address (d05), but the field is a
     *             uint32_t and the array is a pointer, so you must cast:
     *             (uint32_t)(uintptr_t)eth_rx_buf[i]
     *   des1 = 0
     *   des2 = 0            (we are not using a second buffer)
     *   des3 = RDES3_OWN | RDES3_BUF1V
     *
     * ORDER MATTERS. des3 must be written LAST. The instant OWN goes to 1 the
     * DMA may look at that descriptor, and it has to find a valid address
     * already sitting in des0.
     *
     * The loop is d06 all over again: for (unsigned i = 0; i < N; i++).
     */
    for(unsigned i = 0; i < ETH_RX_DESC_COUNT; i++) {
        eth_rx_desc[i].des0 = (uint32_t)(uintptr_t)eth_rx_buf[i];
        eth_rx_desc[i].des1 = 0u;
        eth_rx_desc[i].des2 = 0u;
        eth_rx_desc[i].des3 = RDES3_OWN | RDES3_BUF1V;
    }

    /* ---- STEP H2: clear the TX ring --------------------------------------
     * Same loop shape, but every field is 0 -- including des3, which means
     * OWN = 0, which means you own all of them. Nothing is queued to send.
     */
    for(unsigned i = 0; i < ETH_TX_DESC_COUNT; i++) {
        eth_tx_desc[i].des0 = 0u;
        eth_tx_desc[i].des1 = 0u;
        eth_tx_desc[i].des2 = 0u;
        eth_tx_desc[i].des3 = 0u;
}
}

/* ==========================================================================
 * STEP G -- make SRAM3 safe for DMA.
 *
 * THE PROBLEM, in one paragraph. The Cortex-M7 has a data cache. When you
 * write a descriptor, the write may sit in cache and never reach RAM -- so the
 * DMA reads a stale descriptor. When the DMA writes a received frame straight
 * into RAM, the CPU may still hold an old cached copy -- so you read stale
 * data. Two masters, one memory, no automatic agreement.
 *
 * THE FIX used here: tell the MPU that this 32 KB region is Normal,
 * non-cacheable memory. Every access then goes to real RAM. Slower than
 * cached, and completely correct -- which is the right trade for a few
 * kilobytes of descriptors.
 * ========================================================================== */
void eth_mem_init(void)
{
    /* ---- STEP G1: pick the region and point it at SRAM3 ------------------
     * MPU_RNR selects WHICH of the 16 regions you are editing. Use region 0 --
     * nothing else in this project uses the MPU yet.
     * MPU_RBAR takes the base address: ETH_RAM_BASE.
     *
     * The base MUST be aligned to the region size. 0x30040000 is a multiple
     * of 32 KB, so you are fine -- but that is why this works, not luck.
     */
    MPU_RNR = 0u;
    MPU_RBAR = ETH_RAM_BASE;


    /* ---- STEP G2: describe the region in MPU_RASR -----------------------
     * Build ONE word out of these pieces (PM0253 Table 89, PDF p.227):
     *
     *   TEX = 0b001, C = 0, B = 0, S = 1   -> Normal, shareable, NON-CACHEABLE
     *                                         (Table 91, PDF p.228)
     *   AP  = 0b011                        -> full read/write access
     *   XN  = 1                            -> never execute code from here
     *   SIZE                               -> region bytes = 2^(SIZE+1).
     *                                         32 KB, so work out SIZE yourself.
     *   ENABLE = 1
     *
     * Note C and B must be ZERO, so you simply do not OR in MPU_RASR_C or
     * MPU_RASR_B at all. Leaving a bit clear is a decision, not an omission --
     * say so in a comment.
     *
     * This is the step C field-write pattern again, four fields instead of one.
     */
    uint32_t rasr = 0u;
    rasr |= 1u << MPU_RASR_TEX_SHIFT;  /* TEX = 0b001 */
    rasr |= MPU_RASR_S;                 /* S = 1 */
    rasr |= 3u << MPU_RASR_AP_SHIFT;    /* AP = 0b011 */
    rasr |= MPU_RASR_XN;                /* XN = 1 */
    rasr |= 14u << MPU_RASR_SIZE_SHIFT; /* SIZE = 0b1110 -> 2^(14+1) = 32 KB */
    rasr |= MPU_RASR_ENABLE;            /* ENABLE = 1 */
    MPU_RASR = rasr;
    /* ---- STEP G3: switch the MPU on --------------------------------------
     * MPU_CTRL needs MPU_CTRL_ENABLE and MPU_CTRL_PRIVDEFENA together.
     *
     * PRIVDEFENA is not optional: without it, the ONLY accessible memory
     * becomes the regions you defined -- so flash, DTCM and every peripheral
     * vanish, and the CPU faults on the next instruction fetch. With it, the
     * default memory map stays in force everywhere your region does not cover.
     *
     * Finish with barrier_dsb() then barrier_isb(). The MPU change must be
     * complete and the pipeline refilled before the next access is allowed to
     * use the new rules.
     */
    MPU_CTRL = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;
    barrier_dsb();
    barrier_isb();

}

void eth_pins_init(void)
{
    /* ---- STEP E1: clocks for the ports you are about to touch ------------
     * The 7 RMII pins live on ports A, B, C and G. board_init() already
     * enabled B, C and E; step B added A. So only GPIOG is missing --
     * AHB4ENR_GPIOGEN. You also need the SYSCFG clock for step E2:
     * APB4ENR_SYSCFGEN, exactly as board_button_init() does it.
     */
    RCC_APB4ENR |= APB4ENR_SYSCFGEN;
    (void)RCC_APB4ENR;
    RCC_AHB4ENR |= AHB4ENR_GPIOAEN | AHB4ENR_GPIOGEN;
    /* ---- STEP E2: tell the chip this board is RMII, not MII --------------
     * SYSCFG_PMCR, field EPIS[2:0] = 100 (RM0399 §13.3.1, PDF p.586).
     * This is the field write from step C: clear with PMCR_EPIS_MASK, then
     * OR in your value shifted by PMCR_EPIS_SHIFT.
     *
     * Order matters and it is the classic trap on this chip: EPIS must be set
     * while the MAC clock is still OFF. That is why this function runs before
     * eth_mdio_init(). Set it afterwards and the MAC stays in MII mode with
     * no error of any kind -- the pins simply never carry traffic.
     */
    SYSCFG_PMCR &= ~PMCR_EPIS_MASK;
    SYSCFG_PMCR |= (4u << PMCR_EPIS_SHIFT);
    /* ---- STEP E3: hand 7 more pins to the MAC, all AF11 ------------------
     * Same gpio_af(port, pin, af) you used in step B. From the board DTS:
     *
     *   PA1  REF_CLK      PA7  CRS_DV
     *   PC4  RXD0         PC5  RXD1
     *   PG11 TX_EN        PG13 TXD0        PB13 TXD1
     *
     * Seven calls. Repetitive on purpose -- typing them is the point.
     */
    gpio_af(GPIOA, 1, 11);
    gpio_af(GPIOA, 7, 11);
    gpio_af(GPIOC, 4, 11);
    gpio_af(GPIOC, 5, 11);
    gpio_af(GPIOG, 11, 11);
    gpio_af(GPIOG, 13, 11);
    gpio_af(GPIOB, 13, 11);
}

void eth_mdio_init(void)
{
    /* ---- STEP A: clocks ---------------------------------------------------
     * Enable the ETH MAC clock in RCC_AHB1ENR (already declared for you as
     * AHB1ENR_ETH1MACEN / _ETH1TXEN / _ETH1RXEN), then read the register back
     * -- board.c does the same after every RCC enable, because the AHB write
     * is buffered and the very next peripheral access can otherwise land
     * before the clock is actually running.
     *
     * Decide: does the management interface need TX/RX clocks, or only MAC?
     * Either answer is defensible for milestone 1; say which and why.
     *
     * DONE WHEN: app_eth prints AHB1ENR with your bit(s) set. That is the
     * whole of step A. Flash it and look before writing step B.
     */
     RCC_AHB1ENR |= AHB1ENR_ETH1MACEN;
     (void)RCC_AHB1ENR;
    /* ---- STEP B: pins -----------------------------------------------------
     * PA2 = MDIO, PC1 = MDC, both alternate function 11.
     * Use the gpio_af(port, pin, af) helper now in stm32h745.h -- it handles
     * the AFR[0]/AFR[1] split, so this is two calls plus the GPIO clocks.
     *
     * Careful: board_init() enables GPIOB/C/E only. GPIOA is NOT on yet.
     * Confirm AF11 yourself in DS12923 Table 9, PDF p.86.
     *
     * DONE WHEN: it builds and still prints. No visible change yet -- pins
     * alone prove nothing. That is expected.
     */
     RCC_AHB4ENR |= AHB4ENR_GPIOAEN;
     gpio_af(GPIOA, 2, 11);
     gpio_af(GPIOC, 1, 11);
    /* ---- STEP C: the MDC divider -----------------------------------------
     * MDC is HCLK divided by the CR field of ETH_MACMDIOAR, and it must come
     * out at or below 2.5 MHz (IEEE 802.3 clause 22). This kernel runs
     * HCLK = SYS_CLK_HZ = 64 MHz.
     *
     * The CSR clock range table is RM0399 §61.11, PDF p.3151. It is indexed by
     * HCLK *range*, not by "whatever divides below 2.5 MHz" -- more than one
     * encoding satisfies the ceiling, but only one range contains 64 MHz.
     * Write the arithmetic in a comment: 64 MHz / <divisor> = <MDC> MHz.
     *
     * THIS IS THE ONE THAT BITES. A wrong CR does not fault, does not set an
     * error bit, and does not hang -- the PHY simply fails to sample and you
     * read 0xFFFF, which is indistinguishable from unplugged hardware.
     *
     * Set CR here, in init, once. Step D must preserve it on every call.
     *
     * DONE WHEN: app_eth prints MACMDIOAR with your CR field in place.
     */
     ETH_MACMDIOAR &= ~MACMDIOAR_CR_MASK;
     ETH_MACMDIOAR |= (0u << MACMDIOAR_CR_SHIFT);   /* 0000: 64 MHz / 42 = 1.52 MHz */
}

uint16_t eth_mdio_read(uint8_t phy_addr, uint8_t reg)
{
    /* ---- STEP D: one clause-22 read --------------------------------------
     * You are not bit-banging the protocol. The MAC has a hardware state
     * machine: you describe the transaction, set the busy bit, and it clocks
     * all 64 bit-times out on MDC by itself.
     *
     *   1. build MACMDIOAR: PA = phy_addr, RDA = reg, GOC = the read encoding,
     *      CR preserved from step C, then MB set to launch it;
     *   2. poll until the MAC clears MB;
     *   3. return the low 16 bits of MACMDIODR.
     *
     * Two questions worth answering in comments:
     *   - what happens if you set MB while MB is already set?
     *   - step 2 hangs forever if the PHY never answers. Bound the poll with a
     *     counter and return 0xFFFF on timeout. A bench tool that reports "no
     *     answer" beats one that locks up, and 0xFFFF is exactly what a
     *     floating MDIO bus reads anyway.
     *
     * DONE WHEN: ID1=0007, ID2=c13x, green LED.
     */
    uint32_t v = ETH_MACMDIOAR & MACMDIOAR_CR_MASK;
    v |= (uint32_t)phy_addr << MACMDIOAR_PA_SHIFT;
    v |= (uint32_t)reg << MACMDIOAR_RDA_SHIFT; 
    v |= 3u << MACMDIOAR_GOC_SHIFT;

    v |= MACMDIOAR_MB;
    ETH_MACMDIOAR = v;

    uint32_t guard = 100000u;
    while((ETH_MACMDIOAR & MACMDIOAR_MB) && guard){
        guard--;
    }

    if(guard == 0u){
        return 0xFFFFu;
    }

    return (uint16_t)(ETH_MACMDIODR & MACMDIODR_GD_MASK);


}

/* ==========================================================================
 * STEP I -- configure MAC + DMA and switch the receiver on.
 *
 * I wrote this half. It is a fixed initialisation sequence transcribed from
 * RM0399 §61.9 -- long, order-sensitive, and it teaches very little beyond
 * "do these writes in this order". Read it, do not memorise it.
 *
 * The half worth writing yourself is eth_rx_poll() below.
 * ========================================================================== */
unsigned eth_swr_timed_out;   /* diagnostic: did the DMA reset finish? */

void eth_start(unsigned speed100, unsigned full_duplex)
{
    /* I1 -- the data path needs clocks the management interface did not.
     * In step A you enabled ETH1MACEN only, and that was the right call for
     * MDIO. Moving frames needs the TX and RX clocks as well. */
    RCC_AHB1ENR |= AHB1ENR_ETH1TXEN | AHB1ENR_ETH1RXEN;
    (void)RCC_AHB1ENR;

    /* I2 -- reset the DMA and wait for it to finish. SWR clears itself.
     * Everything below must happen after this, or the reset wipes it. */
    ETH_DMAMR |= DMAMR_SWR;
    {
        /* Bounded, for the same reason your MDIO poll is bounded: an
         * unbounded wait on hardware turns a bug into a silent lockup. */
        uint32_t guard = 1000000u;
        while ((ETH_DMAMR & DMAMR_SWR) && guard) {
            guard--;
        }
        eth_swr_timed_out = (guard == 0u);
    }

    /* I3 -- MTL, the FIFO layer between MAC and DMA.
     * Store-and-forward on both sides: the FIFO holds a WHOLE frame before
     * releasing it. Simpler and safer than cut-through at this stage, because
     * a frame never starts moving until it is known to be complete. */
    ETH_MTLTXQOMR = MTLTXQOMR_TSF
                  | (1u << MTLTXQOMR_TXQEN_SHIFT)   /* enable tx queue 0 */
                  | (7u << MTLTXQOMR_TQS_SHIFT);    /* give it all the FIFO */
    ETH_MTLRXQOMR = MTLRXQOMR_RSF
                  | MTLRXQOMR_FEP                   /* forward error packets */
                  | MTLRXQOMR_FUP
                  | (7u << MTLRXQOMR_RQS_SHIFT);

    /* I4 -- tell the DMA where the rings are and how big everything is.
     * RDRL/TDRL are "length minus one" -- the hardware counts from zero.
     * RBSZ sits at bit 1, not bit 0, hence the shift. */
    ETH_DMACTXDLAR = (uint32_t)(uintptr_t)eth_tx_desc;
    ETH_DMACRXDLAR = (uint32_t)(uintptr_t)eth_rx_desc;
    ETH_DMACTXRLR  = ETH_TX_DESC_COUNT - 1u;
    ETH_DMACRXRLR  = ETH_RX_DESC_COUNT - 1u;
    ETH_DMACRXCR   = (ETH_BUF_SIZE << DMACRXCR_RBSZ_SHIFT)
                   | (32u << DMACRXCR_RXPBL_SHIFT);
    ETH_DMACTXCR   = (32u << DMACTXCR_TXPBL_SHIFT);

    /* The tail pointer marks "one past the last descriptor you have given
     * away". Pointing it at the end of the ring means: all of them. */
    ETH_DMACRXDTPR = (uint32_t)(uintptr_t)&eth_rx_desc[ETH_RX_DESC_COUNT - 1u];
    ETH_DMACTXDTPR = (uint32_t)(uintptr_t)eth_tx_desc;

    /* I5 -- MAC speed and duplex must match what the PHY negotiated.
     * Promiscuous mode accepts every frame on the wire, not just ones
     * addressed to us -- exactly what you want for a first bring-up. */
    uint32_t cr = 0u;
    if (speed100)    cr |= MACCR_FES;
    if (full_duplex) cr |= MACCR_DM;
    /* CST: strip the received FCS. Without it every frame arrives 4 bytes
     * longer than it really is, and any length-sensitive work above --
     * a checksum, a payload copy -- is quietly wrong. */
    ETH_MACCR  = cr | MACCR_RE | MACCR_TE | MACCR_CST;
    ETH_MACPFR = MACPFR_PR;

    /* I6 -- start the DMA engines. From here the hardware is live. */
    ETH_DMACRXCR |= DMACRXCR_SR;
    ETH_DMACTXCR |= DMACTXCR_ST;

    barrier_dsb();
}

/* ==========================================================================
 * YOUR PART. Has a frame arrived?
 * ========================================================================== */
uint16_t eth_rx_poll(uint8_t *dst, uint16_t dst_size)
{
    /* rx_index remembers which descriptor we are waiting on. `static` means
     * it keeps its value between calls -- the ring position must survive, and
     * a plain local would reset to 0 every time. */
    static unsigned rx_index = 0u;

    /* ---- STEP I7: has the DMA handed this descriptor back? ---------------
     * Read eth_rx_desc[rx_index].des3 into a uint32_t.
     *
     * If RDES3_OWN is STILL SET, the DMA still owns it -- nothing has
     * arrived. Return 0 immediately.
     *
     * Remember: OWN set means theirs, OWN clear means yours. This is the
     * same test shape as the MB poll in milestone 1, one bit with &.
     */
    uint32_t des3 = eth_rx_desc[rx_index].des3;
    if (des3 & RDES3_OWN) {
        return 0u;
    }

    /* ---- STEP I8: a frame arrived -- how big is it? ----------------------
     * The DMA has rewritten des3 in write-back format. The packet length is
     * field PL, bits 14:0 (RM0399 Table 598, PDF p.3069).
     *
     * Mask it out into a uint16_t. This is d01: select bits with &, and the
     * field starts at bit 0 so nothing needs shifting.
     */
    uint16_t len = (uint16_t)(des3 & RDES3_PL_MASK);

    /* ---- STEP I9: re-arm this descriptor and move on ---------------------
     * The DMA destroyed your setup when it wrote status back, so rebuild it
     * exactly as eth_rings_init() did:
     *
     *   des0 = address of eth_rx_buf[rx_index]
     *   des3 = RDES3_OWN | RDES3_BUF1V      <-- LAST, same rule as step H
     *
     * Then advance rx_index to the next slot, wrapping back to 0 at the end.
     * The wrap is what makes it a ring:
     *
     *   rx_index = (rx_index + 1u) % ETH_RX_DESC_COUNT;
     *
     * Finally return the length you found in I8.
     */
    /* Copy the frame out BEFORE re-arming -- the next line after the re-arm
     * gives this buffer back to the DMA. */
    {
        uint16_t n = (len < dst_size) ? len : dst_size;
        memcpy(dst, eth_rx_buf[rx_index], n);
    }

    eth_rx_desc[rx_index].des0 = (uint32_t)(uintptr_t)eth_rx_buf[rx_index];
    eth_rx_desc[rx_index].des3 = RDES3_OWN | RDES3_BUF1V;
    rx_index = (rx_index + 1u) % ETH_RX_DESC_COUNT;
    ETH_DMACSR = DMACSR_RBU;  /* clear the "RX buffer unavailable" bit, if it was set */
    ETH_DMACRXDTPR = (uint32_t)(uintptr_t)&eth_rx_desc[rx_index];  /* advance the tail pointer */

    return len;
}

/* ==========================================================================
 * STEP J -- transmit. The mirror of eth_rx_poll(), with the roles swapped:
 * on receive you waited for the DMA to hand a descriptor BACK; on transmit
 * you fill one in and hand it OVER.
 * ========================================================================== */
int eth_tx_send(const uint8_t *data, uint16_t len)
{
    static unsigned tx_index = 0u;

    if (len > ETH_BUF_SIZE) {
        return -1;                      /* will not fit in one buffer */
    }

    /* ---- STEP J1: is this descriptor free? -------------------------------
     * Read eth_tx_desc[tx_index].des3.
     *
     * If TDES3_OWN is STILL SET, the DMA has not finished sending the last
     * frame that used this slot -- the ring is full. Return -1.
     *
     * Same test as I7, opposite meaning: there OWN set meant "nothing has
     * arrived yet", here it means "no room to send".
     */
    uint32_t des3 = eth_tx_desc[tx_index].des3;
    if (des3 & TDES3_OWN) {
        return -1;                      /* DMA still owns it */
    }

    /* ---- STEP J2: copy the frame into the buffer -------------------------
     * memcpy(eth_tx_buf[tx_index], data, len);
     *
     * The copy is needed because the DMA can only read from the non-cacheable
     * SRAM3 region you set up in step G. The caller's data usually lives in
     * DTCM, which the DMA cannot reach at all.
     */
    memcpy(eth_tx_buf[tx_index], data, len);

    /* ---- STEP J3: describe the frame and hand it over --------------------
     *   des0 = address of eth_tx_buf[tx_index]        (cast like step H)
     *   des1 = 0
     *   des2 = len & TDES2_B1L_MASK                   (how much is in buf 1)
     *   des3 = TDES3_OWN | TDES3_FD | TDES3_LD | (len & TDES3_FL_MASK)
     *
     * FD and LD together say "this one descriptor is the whole packet" --
     * first segment and last segment at once. A frame longer than one buffer
     * would span several descriptors, with FD on the first and LD on the last.
     *
     * des3 LAST again, for the same reason as always.
     *
     * Then advance tx_index with the modulo wrap, and ring the TX doorbell:
     *
     *     ETH_DMACTXDTPR = (uint32_t)(uintptr_t)&eth_tx_desc[tx_index];
     *
     * Return 0.
     */
    eth_tx_desc[tx_index].des0 = (uint32_t)(uintptr_t)eth_tx_buf[tx_index];
    eth_tx_desc[tx_index].des1 = 0u;
    eth_tx_desc[tx_index].des2 = (uint32_t)(len & TDES2_B1L_MASK);
    eth_tx_desc[tx_index].des3 = TDES3_OWN | TDES3_FD | TDES3_LD | (len & TDES3_FL_MASK);
    tx_index = (tx_index + 1u) % ETH_TX_DESC_COUNT;
    /* Descriptors are Normal memory; the tail pointer is a Device register.
     * The core may reorder writes between the two, so the doorbell could
     * reach the MAC before OWN is visible. */
    barrier_dsb();
    ETH_DMACTXDTPR = (uint32_t)(uintptr_t)&eth_tx_desc[tx_index];


    return 0;
}
