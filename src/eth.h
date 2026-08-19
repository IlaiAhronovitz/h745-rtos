/* eth.h — Ethernet driver for the STM32H745, bare metal.
 *
 * The MAC is a Synopsys DesignWare EQOS core (NOT the older GMAC found on the
 * F4/F7 — the register map is different, so F4 example code will mislead you).
 * It drives an on-board Microchip LAN8742A PHY over RMII.
 *
 * Sources (local copies and page numbers: docs/refs/INDEX.md):
 *   RM0399 rev 4 §61        Ethernet (ETH): MAC with DMA controller
 *   RM0399 §61.6            PHY interfaces; clause-22 MDIO frame, PDF p.3007
 *   RM0399 §61.11           register descriptions, PDF p.3074
 *   DS12923 rev 3 Table 9   alternate function map, PDF p.86
 *   UM2408 rev 6            Nucleo-144 MB1363 board manual
 *   LAN8742A rev 1.1        management (MDIO) register map
 *
 * Beware RM0399 §57 "MDIOS" — that is a different peripheral (MDIO *slave*),
 * and it is what a naive search for "MDIO" finds first.
 *
 * Board wiring, taken from the nucleo_h745zi_q device tree (verified, not
 * assumed) — every pin is AF11:
 *   MDIO   PA2    MDC   PC1
 *   REF_CLK PA1   CRS_DV PA7   RXD0 PC4   RXD1 PC5
 *   TX_EN  PG11   TXD0  PG13   TXD1 PB13
 *
 * DMA note for later milestones: the ETH DMA cannot reach DTCM, and the D-cache
 * will hand it stale descriptors. Rings and buffers must live in D2 SRAM3
 * (0x30040000) behind an MPU region marked non-cacheable.
 */
#pragma once
#include <stdint.h>

/* The LAN8742A straps to MDIO address 0 on this board (DTS: ethernet-phy@0). */
#define ETH_PHY_ADDR 0u

/* IEEE 802.3 clause 22 standard registers (same on every conforming PHY). */
#define PHY_REG_BCR   0u  /* basic control  */
#define PHY_REG_BSR   1u  /* basic status   */
#define PHY_REG_ID1   2u  /* OUI bits 3..18 */
#define PHY_REG_ID2   3u  /* OUI 19..24, model, revision */

/* Bits inside PHY_REG_BSR (IEEE 802.3 cl.22 register 1). */
#define PHY_BSR_LINK      (1u << 2)  /* 1 = link is up right now      */
#define PHY_BSR_ANEG_DONE (1u << 5)  /* 1 = auto-negotiation finished */

/* LAN8742A-specific: PHY Special Control/Status, register 31 (datasheet
 * PDF p.89). Bits 4:2 = HCDSPEED, the result of auto-negotiation:
 *   001 = 10 half   101 = 10 full   010 = 100 half   110 = 100 full
 * so within that 3-bit field, bit 1 means 100 Mbit and bit 2 means full. */
#define PHY_REG_SCSR      31u
#define PHY_SCSR_HCD_SHIFT 2u
#define PHY_SCSR_HCD_MASK  0x7u
#define PHY_HCD_100        0x2u
#define PHY_HCD_FULL       0x4u

/* ---- Milestone 1: management interface only ------------------------------
 * No RMII data path, no DMA, no interrupts. Just enough to prove the PHY is
 * powered, clocked and answering.
 */
void     eth_mdio_init(void);
uint16_t eth_mdio_read(uint8_t phy_addr, uint8_t reg);

/* ---- Milestone 2, step E: the RMII data path pins ------------------------
 * MUST be called BEFORE eth_mdio_init(): EPIS is only latched while the MAC
 * clock is still off, so selecting RMII after enabling ETH1MACEN silently
 * leaves the MAC in MII mode. */
void     eth_pins_init(void);

/* ---- Milestone 2, step G: memory the DMA can actually reach --------------
 * Call once, before any descriptor is touched. Marks the SRAM3 region
 * non-cacheable so the CPU and the ETH DMA agree about what is in memory.
 *
 * Put DMA-visible objects in that region with:
 *     __attribute__((section(".eth_ram"), aligned(32)))
 */
#define ETH_RAM_BASE 0x30040000u   /* D2 SRAM3, RM0399 PDF p.136 */
#define ETH_RAM_SIZE (32u * 1024u)

void     eth_mem_init(void);

/* ---- Milestone 2, step H: descriptors ------------------------------------
 * A descriptor is 4 words whose layout the HARDWARE defines (RM0399 §61.10,
 * PDF pp.3046-3069). This is d07 with real stakes: if the compiler inserted
 * one byte of padding, every field after it would land in the wrong place and
 * the DMA would read garbage. Four uint32_t need no padding, so the struct is
 * naturally exact -- and the _Static_assert below proves it at compile time
 * rather than hoping.
 *
 * `volatile` because the DMA writes these behind the CPU's back, exactly like
 * a hardware register.
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
#define ETH_BUF_SIZE      1536u   /* 1522 max tagged frame, rounded up to 32 */

/* RDES3, read format (RM0399 Table 594, PDF p.3064). */
#define RDES3_OWN    (1u << 31)  /* 1 = DMA owns it, 0 = you own it */
#define RDES3_IOC    (1u << 30)  /* interrupt when this one is filled */
#define RDES3_BUF2V  (1u << 25)
#define RDES3_BUF1V  (1u << 24)  /* "the address in des0 is real" */

/* RDES3, WRITE-BACK format -- what the DMA leaves behind (Table 598, p.3069).
 * Same word, different meaning, once OWN has been cleared. */
#define RDES3_PL_MASK  0x7FFFu   /* bits 14:0, packet length in bytes */
#define RDES3_ES       (1u << 15) /* error summary: frame arrived bad  */

/* TDES2/TDES3, read format (RM0399 Tables 581/582, PDF pp.3050-3051). */
#define TDES2_IOC     (1u << 31)
#define TDES2_B1L_MASK  0x3FFFu  /* buffer 1 length, bits 13:0 */
#define TDES3_OWN     (1u << 31)
#define TDES3_FD      (1u << 29) /* first descriptor of the packet */
#define TDES3_LD      (1u << 28) /* last descriptor of the packet  */
#define TDES3_FL_MASK  0x7FFFu   /* bits 14:0, total frame length (p.3049) */

/* ---- Milestone 2, step J: transmit ---------------------------------------
 * Copies len bytes into the next free TX buffer and hands the descriptor to
 * the DMA. Returns 0 on success, -1 if the ring is full (the DMA still owns
 * the next descriptor) or the frame is too big for one buffer.
 *
 * The MAC appends the CRC itself, and pads anything under 60 bytes, so you
 * pass only the real frame content.
 */
int      eth_tx_send(const uint8_t *data, uint16_t len);

void     eth_rings_init(void);

/* ---- Milestone 2, step I: switch the receiver on -------------------------
 * eth_start() does the fixed configuration sequence -- MTL FIFOs, MAC speed
 * and duplex, DMA ring addresses -- and then enables RX and TX.
 * Call it AFTER the link is up, because speed and duplex come from the PHY.
 *
 * eth_rx_poll() is the interesting half: check whether the DMA has handed a
 * frame back, and if so report its length.
 *
 * Returns the frame length in bytes, or 0 if nothing has arrived.
 */
extern unsigned eth_swr_timed_out;   /* set if the DMA reset never cleared */
void     eth_start(unsigned speed100, unsigned full_duplex);
/* Copies the frame into dst (at most dst_size bytes) and returns its true
 * length, or 0 if nothing has arrived. The copy is not optional: the
 * descriptor is handed back to the DMA before this returns, so a pointer into
 * eth_rx_buf would be pointing at memory the hardware may overwrite. */
uint16_t eth_rx_poll(uint8_t *dst, uint16_t dst_size);

/* Exposed so the app can print them and prove placement. */
extern eth_desc_t eth_rx_desc[ETH_RX_DESC_COUNT];
extern eth_desc_t eth_tx_desc[ETH_TX_DESC_COUNT];
extern uint8_t    eth_rx_buf[ETH_RX_DESC_COUNT][ETH_BUF_SIZE];
extern uint8_t    eth_tx_buf[ETH_TX_DESC_COUNT][ETH_BUF_SIZE];
