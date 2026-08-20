/* eth.h — Ethernet driver for the STM32H745, bare metal.
 *
 * The MAC is a Synopsys DesignWare EQOS core, NOT the older GMAC found on the
 * F4/F7 series. The register maps differ, so F4 example code will mislead you.
 * It drives an on-board Microchip LAN8742A PHY over RMII.
 *
 * Sources (local copies and page numbers: docs/refs/INDEX.md):
 *   RM0399 rev 4 section 61      Ethernet (ETH): MAC with DMA controller
 *   RM0399 section 61.6          PHY interfaces; clause-22 MDIO frame, p.3007
 *   RM0399 section 61.11         register descriptions, p.3074
 *   DS12923 rev 3 Table 9        alternate function map, p.86
 *   UM2408 rev 6                 Nucleo-144 MB1363 board manual
 *   LAN8742A rev 1.1             management (MDIO) register map
 *
 * Beware RM0399 section 57, "MDIOS": that is a different peripheral entirely,
 * an MDIO *slave*, and it is what a naive search for "MDIO" finds first.
 *
 * Board wiring, from the nucleo_h745zi_q device tree rather than assumed.
 * Every pin is AF11:
 *   MDIO    PA2    MDC    PC1
 *   REF_CLK PA1    CRS_DV PA7    RXD0 PC4    RXD1 PC5
 *   TX_EN   PG11   TXD0   PG13   TXD1 PB13
 */
#pragma once
#include <stdint.h>

/* The LAN8742A straps to MDIO address 0 on this board (DTS: ethernet-phy@0). */
#define ETH_PHY_ADDR 0u

/* ---- PHY registers -------------------------------------------------------*/

/* IEEE 802.3 clause 22 standard set, identical on every conforming PHY. */
#define PHY_REG_BCR   0u  /* basic control                */
#define PHY_REG_BSR   1u  /* basic status                 */
#define PHY_REG_ID1   2u  /* OUI bits 3..18               */
#define PHY_REG_ID2   3u  /* OUI 19..24, model, revision  */

#define PHY_BSR_LINK      (1u << 2)  /* link is up right now          */
#define PHY_BSR_ANEG_DONE (1u << 5)  /* auto-negotiation has finished */

/* LAN8742A-specific: PHY Special Control/Status, register 31 (datasheet p.89).
 * Bits 4:2 are HCDSPEED, the negotiated result:
 *   001 = 10 half   101 = 10 full   010 = 100 half   110 = 100 full
 * so within that 3-bit field, bit 1 means 100 Mbit and bit 2 means full. */
#define PHY_REG_SCSR       31u
#define PHY_SCSR_HCD_SHIFT  2u
#define PHY_SCSR_HCD_MASK   0x7u
#define PHY_HCD_100         0x2u
#define PHY_HCD_FULL        0x4u

/* ---- DMA-visible memory --------------------------------------------------
 * The ETH DMA cannot reach DTCM at all, and the D-cache would otherwise hand
 * it stale descriptors, so rings and buffers live in D2 SRAM3 behind an MPU
 * region marked non-cacheable.
 *
 * Place DMA-visible objects there with:
 *     __attribute__((section(".eth_ram"), aligned(32)))
 */
#define ETH_RAM_BASE 0x30040000u        /* D2 SRAM3, RM0399 p.136 */
#define ETH_RAM_SIZE (32u * 1024u)

/* ---- Descriptors ---------------------------------------------------------
 * Four words whose layout the hardware defines (RM0399 section 61.10,
 * pp.3046-3069). Struct padding would be fatal: a single inserted byte would
 * shift every field after it and the DMA would read garbage. Four uint32_t
 * need no padding, so the layout is naturally exact -- and the _Static_assert
 * proves that at compile time rather than assuming it.
 *
 * volatile because the DMA rewrites these behind the CPU's back, exactly as a
 * hardware register would.
 */
typedef struct {
    volatile uint32_t des0;
    volatile uint32_t des1;
    volatile uint32_t des2;
    volatile uint32_t des3;
} eth_desc_t;

_Static_assert(sizeof(eth_desc_t) == 16, "descriptor must be exactly 4 words");

#define ETH_RX_DESC_COUNT 4u
#define ETH_TX_DESC_COUNT 4u
#define ETH_BUF_SIZE      1536u   /* 1522-byte max tagged frame, rounded to 32 */

/* RDES3 as the CPU writes it -- "read format" (RM0399 Table 594, p.3064). */
#define RDES3_OWN    (1u << 31)  /* 1 = DMA owns it, 0 = the CPU does   */
#define RDES3_IOC    (1u << 30)  /* interrupt once this one is filled    */
#define RDES3_BUF2V  (1u << 25)
#define RDES3_BUF1V  (1u << 24)  /* the address in des0 is valid         */

/* RDES3 as the DMA leaves it -- "write-back format" (Table 598, p.3069).
 * The same word, repurposed, once OWN has been cleared. */
#define RDES3_PL_MASK  0x7FFFu    /* bits 14:0, packet length in bytes   */
#define RDES3_ES       (1u << 15) /* error summary: the frame arrived bad */

/* TDES2 and TDES3, read format (RM0399 Tables 581/582, pp.3050-3051). */
#define TDES2_IOC       (1u << 31)
#define TDES2_B1L_MASK  0x3FFFu   /* buffer 1 length, bits 13:0          */
#define TDES3_OWN       (1u << 31)
#define TDES3_FD        (1u << 29) /* first descriptor of the packet      */
#define TDES3_LD        (1u << 28) /* last descriptor of the packet       */
#define TDES3_FL_MASK   0x7FFFu   /* bits 14:0, whole frame length (p.3049) */

/* ---- Interface -----------------------------------------------------------
 * Initialisation order is fixed and explained at the top of eth.c:
 *
 *     eth_mem_init(); eth_rings_init(); eth_pins_init(); eth_mdio_init();
 *     ... wait for link, read speed and duplex from the PHY ...
 *     eth_start(speed100, full_duplex);
 */

/* MPU region for the DMA memory. Call once, before any descriptor is touched. */
void     eth_mem_init(void);

/* Descriptor rings: RX handed to the DMA, TX retained by the CPU. */
void     eth_rings_init(void);

/* The seven RMII data-path pins plus the RMII/MII mode select.
 * MUST precede eth_mdio_init(): the mode select only latches while the MAC
 * clock is off, and setting it afterwards leaves the MAC in MII mode with no
 * error reported anywhere. */
void     eth_pins_init(void);

/* MAC clock, the MDIO pin pair, and the MDC divider. */
void     eth_mdio_init(void);

/* Read one PHY register (IEEE 802.3 clause 22).
 * Returns 0xFFFF on timeout, which is also what an idle MDIO bus floats to. */
uint16_t eth_mdio_read(uint8_t phy_addr, uint8_t reg);

/* Configure MTL, MAC and DMA, then enable receive and transmit.
 * Call only once the link is up: speed and duplex must match what the PHY
 * negotiated. */
void     eth_start(unsigned speed100, unsigned full_duplex);

/* Set if the DMA software reset never completed, which means the MAC never
 * received its RMII reference clock. The most useful single diagnostic during
 * bring-up, because everything configured afterwards is then meaningless. */
extern unsigned eth_swr_timed_out;

/* Copy the next received frame into dst, at most dst_size bytes, and return
 * its true length -- or 0 if nothing has arrived.
 *
 * The copy is not optional: the descriptor is handed back to the DMA before
 * this returns, so a pointer into eth_rx_buf would point at memory the
 * hardware is free to overwrite. */
uint16_t eth_rx_poll(uint8_t *dst, uint16_t dst_size);

/* Queue one frame for transmission. Returns 0 on success, -1 if the frame is
 * too large for a single buffer or the ring is full.
 *
 * The MAC appends the CRC and pads anything under 60 bytes, so callers pass
 * only real frame content. */
int      eth_tx_send(const uint8_t *data, uint16_t len);

/* Enable the Ethernet receive interrupt and route it to the NVIC.
 *
 * nvic_prio must be numerically >= the kernel's BASEPRI mask for the handler
 * to be kernel-aware -- i.e. maskable by the kernel lock, and therefore
 * allowed to call k_sem_give(). Pass K_BASEPRI_MASK, as board_button_init()
 * does. A more urgent (numerically lower) priority would run underneath the
 * kernel and must not touch kernel state at all. */
void eth_irq_enable(uint8_t nvic_prio);

/* Called from the receive ISR when a frame has arrived. NULL by default.
 *
 * This hook is how the driver stays independent of the kernel. eth.c includes
 * no kernel header and knows nothing about tasks or semaphores; the
 * application installs a function here that does whatever waking it needs.
 * Anything else -- a bare polling loop, a different RTOS -- can use this
 * driver unchanged.
 *
 * Runs in interrupt context at the priority passed to eth_irq_enable(), so
 * whatever is installed must be safe to call from an ISR. */
extern void (*eth_rx_notify)(void);

/* Incremented by the ISR on every receive interrupt. */
extern volatile uint32_t eth_rx_irq_count;

/* Rings and buffers, in .eth_ram. Exposed for diagnostics. */
extern eth_desc_t eth_rx_desc[ETH_RX_DESC_COUNT];
extern eth_desc_t eth_tx_desc[ETH_TX_DESC_COUNT];
extern uint8_t    eth_rx_buf[ETH_RX_DESC_COUNT][ETH_BUF_SIZE];
extern uint8_t    eth_tx_buf[ETH_TX_DESC_COUNT][ETH_BUF_SIZE];
