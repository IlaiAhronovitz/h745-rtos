/* usbh.c — bare-metal USB Full-Speed host, USB2 OTG instance, buffer-DMA.
 *
 * Register truth: RM0399 ch.60 via docs/refs/USB-OTG-NOTES.md (every offset
 * and bit position there is page-cited). Init ordering follows the proven
 * CherryUSB DWC2 host recipe (usb_hc_dwc2.c) with the H7-specific parts:
 * GCCFG here is the H7 layout (PWRDWN=enable, bit21=VBDEN) — NOT the F4
 * layout where bit21 disables sensing — and VBUS sensing stays off with the
 * GOTGCTL B-session overrides cleared.
 *
 * Three traps this file is built around:
 *  - HPRT carries four rc_w1 bits (PCDET/PENA/PENCHNG/POCCHNG). PENA reads
 *    as "port enabled" but WRITING 1 DISABLES THE PORT, so every read-
 *    modify-write goes through hprt_rmw() which masks all four.
 *  - The DMA reads descriptors/buffers over the D2 AHB matrix: it cannot
 *    see DTCM, so the bounce buffer lives in .usb_ram (SRAM3), and the
 *    doorbell rule from the Ethernet driver applies — barrier_dsb() between
 *    writing memory the core will read and the Device write that starts it.
 *  - HFIR may only be written after PENA is set (RM0399 p.2808), so the
 *    frame interval is programmed in the port-enable path, not in init.
 */
#include "stm32h745.h"
#include "libmin.h"
#include "kprintf.h"
#include "usbh.h"

/* ---- USB2 OTG register block (RM0399 Table 535/536, PDF pp.2769-2771) ---- */
#define OTG_BASE        0x40080000u
#define OTG(off)        REG32(OTG_BASE + (off))

#define OTG_GOTGCTL     OTG(0x000u)
#define   GOTGCTL_BVALOEN   (1u << 6)
#define   GOTGCTL_BVALOVAL  (1u << 7)
#define OTG_GAHBCFG     OTG(0x008u)
#define   GAHBCFG_GINT      (1u << 0)
#define   GAHBCFG_HBSTLEN_SHIFT 1u
#define   GAHBCFG_DMAEN     (1u << 5)
#define OTG_GUSBCFG     OTG(0x00Cu)
#define   GUSBCFG_TOCAL_MASK 7u
#define   GUSBCFG_PHYSEL    (1u << 6)
#define   GUSBCFG_TRDT_SHIFT 10u
#define   GUSBCFG_TRDT_MASK (0xFu << 10)
#define   GUSBCFG_FHMOD     (1u << 29)
#define   GUSBCFG_FDMOD     (1u << 30)
#define OTG_GRSTCTL     OTG(0x010u)
#define   GRSTCTL_CSRST     (1u << 0)
#define   GRSTCTL_RXFFLSH   (1u << 4)
#define   GRSTCTL_TXFFLSH   (1u << 5)
#define   GRSTCTL_TXFNUM_SHIFT 6u
#define   GRSTCTL_AHBIDL    (1u << 31)
#define OTG_GINTSTS     OTG(0x014u)
#define   GINTSTS_CMOD      (1u << 0)   /* 1 = host                          */
#define   GINTSTS_HPRTINT   (1u << 24)
#define   GINTSTS_DISCINT   (1u << 29)
#define OTG_GINTMSK     OTG(0x018u)
#define   GINTMSK_PRTIM     (1u << 24)
#define   GINTMSK_HCIM      (1u << 25)
#define OTG_GRXFSIZ     OTG(0x024u)
#define OTG_HNPTXFSIZ   OTG(0x028u)
#define OTG_GCCFG       OTG(0x038u)
#define   GCCFG_PWRDWN      (1u << 16)  /* H7: 1 = FS PHY ENABLED (p.2800)   */
#define OTG_CID         OTG(0x03Cu)
/* GSNPSID/GHWCFGx are Synopsys-standard offsets; RM0399 documents the core
 * as a DWC2 but omits them. They are read-only IDs, printed for ground
 * truth (GHWCFG2[4:3] = 2 means internal DMA — required by this driver). */
#define OTG_GSNPSID     OTG(0x040u)
#define OTG_GHWCFG1     OTG(0x044u)
#define OTG_GHWCFG2     OTG(0x048u)
#define OTG_GHWCFG3     OTG(0x04Cu)
#define OTG_GHWCFG4     OTG(0x050u)
#define OTG_HPTXFSIZ    OTG(0x100u)
#define OTG_HCFG        OTG(0x400u)
#define   HCFG_FSLSPCS_MASK 3u          /* 01 = 48 MHz, 10 = 6 MHz (LS)      */
#define OTG_HFIR        OTG(0x404u)
#define   HFIR_RLDCTRL      (1u << 16)  /* p.2809: "= 0 is not recommended"  */
#define OTG_HFNUM       OTG(0x408u)
#define OTG_HAINT       OTG(0x414u)
#define OTG_HPRT        OTG(0x440u)
#define   HPRT_PCSTS        (1u << 0)
#define   HPRT_PCDET        (1u << 1)   /* rc_w1 */
#define   HPRT_PENA         (1u << 2)   /* rc_w1 — writing 1 DISABLES        */
#define   HPRT_PENCHNG      (1u << 3)   /* rc_w1 */
#define   HPRT_POCCHNG      (1u << 5)   /* rc_w1 */
#define   HPRT_PRST         (1u << 8)
#define   HPRT_PPWR         (1u << 12)
#define   HPRT_PSPD_SHIFT   17u
#define   HPRT_PSPD_MASK    (3u << 17)  /* 01 = FS, 10 = LS                  */
#define   HPRT_W1C          (HPRT_PCDET | HPRT_PENA | HPRT_PENCHNG | HPRT_POCCHNG)

#define OTG_HCCHAR(ch)  OTG(0x500u + 0x20u * (ch))
#define   HCCHAR_MPSIZ_MASK 0x7FFu
#define   HCCHAR_EPNUM_SHIFT 11u
#define   HCCHAR_EPDIR_IN   (1u << 15)
#define   HCCHAR_LSDEV      (1u << 17)
#define   HCCHAR_EPTYP_SHIFT 18u        /* 00 = control                      */
#define   HCCHAR_MCNT_SHIFT 20u         /* must be >= 1 even non-periodic    */
#define   HCCHAR_DAD_SHIFT  22u
#define   HCCHAR_ODDFRM     (1u << 29)
#define   HCCHAR_CHDIS      (1u << 30)
#define   HCCHAR_CHENA      (1u << 31)
#define OTG_HCINT(ch)   OTG(0x508u + 0x20u * (ch))
#define   HCINT_XFRC        (1u << 0)
#define   HCINT_CHH         (1u << 1)
#define   HCINT_STALL       (1u << 3)
#define   HCINT_NAK         (1u << 4)
#define   HCINT_TXERR       (1u << 7)
#define   HCINT_BBERR       (1u << 8)
#define   HCINT_FRMOR       (1u << 9)
#define   HCINT_DTERR       (1u << 10)
#define   HCINT_ALL         0x7FFu
#define OTG_HCINTMSK(ch) OTG(0x50Cu + 0x20u * (ch))
#define OTG_HCTSIZ(ch)  OTG(0x510u + 0x20u * (ch))
#define   HCTSIZ_PKTCNT_SHIFT 19u
#define   HCTSIZ_XFRSIZ_MASK  0x7FFFFu
#define   HCTSIZ_DPID_SHIFT   29u
#define OTG_HCDMA(ch)   OTG(0x514u + 0x20u * (ch))
#define OTG_PCGCCTL     OTG(0xE00u)

#define PID_DATA0       0u
#define PID_DATA1       2u
#define PID_SETUP       3u

#define CH_OUT          0u              /* fixed channel assignment for EP0  */
#define CH_IN           1u

/* DMA bounce buffers: the OTG DMA cannot reach DTCM, so all transfer data
 * stages through SRAM3. dma_buf serves EP0 only; bulk pipes each own a slot
 * of pipe_buf (slot = channel - 2), so a bulk IN that sits armed can never
 * scribble over another transfer's staging bytes. */
static uint8_t dma_buf[512] __attribute__((section(".usb_ram"), aligned(4)));

#define USBH_BULK_BUF 1600u  /* >= 1514 B Ethernet frame + slack */
static uint8_t pipe_buf[4][USBH_BULK_BUF]
        __attribute__((section(".usb_ram"), aligned(4)));
static uint8_t pipe_next_ch;             /* reset per enumeration            */

/* ---- time: DWT-based bounded waits (no kernel dependency) ---------------- */
static void wait_ms(uint32_t ms)
{
    uint32_t t0 = DWT_CYCCNT;
    uint32_t n  = ms * (SYS_CLK_HZ / 1000u);
    while ((uint32_t)(DWT_CYCCNT - t0) < n) {}
}

/* Poll *reg & mask == want (want is mask or 0) with a millisecond bound. */
static int wait_reg(volatile uint32_t *reg, uint32_t mask, uint32_t want,
                    uint32_t timeout_ms)
{
    uint32_t t0 = DWT_CYCCNT;
    uint32_t n  = timeout_ms * (SYS_CLK_HZ / 1000u);
    while ((*reg & mask) != want) {
        if ((uint32_t)(DWT_CYCCNT - t0) >= n) return USBH_ERR_TIMEOUT;
    }
    return USBH_OK;
}

/* Every HPRT write goes through here: the four rc_w1 bits are masked out of
 * the read-back so a plain RMW cannot disable the port or eat a change flag. */
static void hprt_rmw(uint32_t set)
{
    OTG_HPRT = (OTG_HPRT & ~HPRT_W1C) | set;
}

/* ---- 48 MHz USB kernel clock: HSE 8 MHz bypass -> PLL3 -> pll3_q --------- */
/* DIVM3=5 -> 1.6 MHz ref (RGE=00, medium VCO mandatory at <= 2 MHz, p.373),
 * xN=150 -> VCO 240 MHz (legal 150-420), /Q=5 -> exactly 48.000 MHz. */
static int usb_clock_init(void)
{
    RCC_CR |= RCC_CR_HSEBYP;            /* before HSEON: ST-LINK MCO feed */
    RCC_CR |= RCC_CR_HSEON;
    if (wait_reg(&RCC_CR, RCC_CR_HSERDY, RCC_CR_HSERDY, 10))
        return USBH_ERR_INIT;

    /* PLL source is shared by PLL1..3 and only writable with all PLLs off.
     * This kernel never starts PLL1/2, so the field is free to take. */
    RCC_PLLCKSELR = (RCC_PLLCKSELR
                     & ~(PLLCKSELR_PLLSRC_MASK | PLLCKSELR_DIVM3_MASK))
                  | PLLCKSELR_PLLSRC_HSE
                  | (5u << PLLCKSELR_DIVM3_SHIFT);

    RCC_PLLCFGR = (RCC_PLLCFGR
                   & ~(PLLCFGR_PLL3FRACEN | PLLCFGR_PLL3RGE_MASK))
                | PLLCFGR_PLL3VCOSEL
                | PLLCFGR_DIVQ3EN;

    /* Stored values are N-1 / div-1 (pp.440-441): N=150, Q=/5. */
    RCC_PLL3DIVR = (4u << 16) | 149u;

    RCC_CR |= RCC_CR_PLL3ON;
    if (wait_reg(&RCC_CR, RCC_CR_PLL3RDY, RCC_CR_PLL3RDY, 10))
        return USBH_ERR_INIT;

    RCC_D2CCIP2R = (RCC_D2CCIP2R & ~D2CCIP2R_USBSEL_MASK)
                 | D2CCIP2R_USBSEL_PLL3Q;
    return USBH_OK;
}

/* ---- core soft reset (CherryUSB order: AHBIDL first, then CSRST) --------- */
static int core_reset(void)
{
    if (wait_reg(&OTG_GRSTCTL, GRSTCTL_AHBIDL, GRSTCTL_AHBIDL, 100))
        return USBH_ERR_INIT;
    OTG_GRSTCTL |= GRSTCTL_CSRST;
    if (wait_reg(&OTG_GRSTCTL, GRSTCTL_CSRST, 0, 100))
        return USBH_ERR_INIT;
    return wait_reg(&OTG_GRSTCTL, GRSTCTL_AHBIDL, GRSTCTL_AHBIDL, 100);
}

static int fifo_flush(void)
{
    if (wait_reg(&OTG_GRSTCTL, GRSTCTL_AHBIDL, GRSTCTL_AHBIDL, 100))
        return USBH_ERR_INIT;
    OTG_GRSTCTL = (0x10u << GRSTCTL_TXFNUM_SHIFT) | GRSTCTL_TXFFLSH;
    if (wait_reg(&OTG_GRSTCTL, GRSTCTL_TXFFLSH, 0, 100))
        return USBH_ERR_INIT;
    OTG_GRSTCTL = GRSTCTL_RXFFLSH;
    return wait_reg(&OTG_GRSTCTL, GRSTCTL_RXFFLSH, 0, 100);
}

int usbh_init(void)
{
    if (usb_clock_init() != USBH_OK) {
        kprintf("usbh: 48 MHz clock failed (HSE/PLL3)\n");
        return USBH_ERR_INIT;
    }

    /* VDD33USB level detector — mandatory before touching the PHY (p.285).
     * The rail itself is hard-wired on the Nucleo (SB19), no regulator. */
    PWR_CR3 |= PWR_CR3_USB33DEN;
    if (wait_reg(&PWR_CR3, PWR_CR3_USB33RDY, PWR_CR3_USB33RDY, 10)) {
        kprintf("usbh: USB33RDY never came\n");
        return USBH_ERR_INIT;
    }

    RCC_AHB1ENR |= AHB1ENR_USB2OTGEN;
    (void)RCC_AHB1ENR;                  /* read back: clock really on */

    /* Pins: PA11/PA12 -> AF10 (DS12923 Table 9 pp.86-87). PA9 (VBUS sense)
     * stays a plain input — it has no AF; sensing is off anyway. PD10 is the
     * STMPS2141's ACTIVE-LOW enable: output, HIGH = VBUS off for now. */
    RCC_AHB4ENR |= AHB4ENR_GPIOAEN | AHB4ENR_GPIODEN;
    (void)RCC_AHB4ENR;
    gpio_af(GPIOA, 11, 10);
    gpio_af(GPIOA, 12, 10);
    GPIOD->BSRR  = (1u << 10);
    GPIOD->MODER = (GPIOD->MODER & ~(3u << 20)) | (1u << 20);

    kprintf("usbh: CID=%08x GSNPSID=%08x\n",
            (unsigned)OTG_CID, (unsigned)OTG_GSNPSID);
    kprintf("usbh: GHWCFG1..4 = %08x %08x %08x %08x\n",
            (unsigned)OTG_GHWCFG1, (unsigned)OTG_GHWCFG2,
            (unsigned)OTG_GHWCFG3, (unsigned)OTG_GHWCFG4);

    /* GHWCFG2[4:3]: 2 = internal DMA. This driver is buffer-DMA only. */
    if (((OTG_GHWCFG2 >> 3) & 3u) != 2u) {
        kprintf("usbh: core reports no internal DMA -- driver unusable\n");
        return USBH_ERR_INIT;
    }

    OTG_GAHBCFG &= ~GAHBCFG_GINT;       /* polled bring-up: keep it off */

    /* H7 GCCFG: PWRDWN alone. No VBDEN — VBUS sensing off, and the
     * B-session override bits cleared below (CherryUSB H7 recipe). */
    OTG_GCCFG = GCCFG_PWRDWN;

    OTG_GUSBCFG |= GUSBCFG_PHYSEL;      /* FS serial PHY (mandatory on USB2) */
    if (core_reset() != USBH_OK) {      /* "reset after a PHY select"        */
        kprintf("usbh: core soft reset stuck\n");
        return USBH_ERR_INIT;
    }

    /* Force host mode; the switch is not instant — poll CMOD (<= 50 ms). */
    OTG_GUSBCFG = (OTG_GUSBCFG & ~GUSBCFG_FDMOD) | GUSBCFG_FHMOD;
    if (wait_reg(&OTG_GINTSTS, GINTSTS_CMOD, GINTSTS_CMOD, 100)) {
        kprintf("usbh: never entered host mode\n");
        return USBH_ERR_INIT;
    }

    OTG_GOTGCTL &= ~(GOTGCTL_BVALOEN | GOTGCTL_BVALOVAL);
    OTG_GUSBCFG |= GUSBCFG_TOCAL_MASK;
    OTG_GUSBCFG  = (OTG_GUSBCFG & ~GUSBCFG_TRDT_MASK)
                 | (9u << GUSBCFG_TRDT_SHIFT);   /* AHB > 30 MHz row, p.2783 */
    OTG_PCGCCTL = 0;                    /* un-gate the PHY clock */

    OTG_HCFG = (OTG_HCFG & ~HCFG_FSLSPCS_MASK) | 1u;    /* 48 MHz FS clock */
    OTG_HFIR |= HFIR_RLDCTRL;

    for (unsigned ch = 0; ch < 16; ch++) {
        OTG_HCINT(ch)    = HCINT_ALL;
        OTG_HCINTMSK(ch) = 0;
    }
    OTG_GINTMSK = 0;
    OTG_GINTSTS = 0xFFFFFFFFu;          /* w1c wipe of stale status */

    /* FIFO carve-up (words, of 1024 total): RX 568, non-periodic TX 128,
     * periodic TX 256 — the CherryUSB H7 host values. */
    OTG_GRXFSIZ   = 568u;
    OTG_HNPTXFSIZ = (128u << 16) | 568u;
    OTG_HPTXFSIZ  = (256u << 16) | 696u;
    if (fifo_flush() != USBH_OK) {
        kprintf("usbh: FIFO flush stuck\n");
        return USBH_ERR_INIT;
    }

    OTG_GAHBCFG |= (7u << GAHBCFG_HBSTLEN_SHIFT) | GAHBCFG_DMAEN;
    OTG_GINTMSK = GINTMSK_PRTIM | GINTMSK_HCIM | GINTSTS_DISCINT;

    /* VBUS on: PD10 low opens the STMPS2141, PPWR tells the core. */
    GPIOD->BSRR = (1u << 26);
    hprt_rmw(HPRT_PPWR);
    wait_ms(200);

    kprintf("usbh: host mode, VBUS on, port powered\n");
    return USBH_OK;
}

int usbh_connected(void)
{
    return (OTG_HPRT & HPRT_PCSTS) ? 1 : 0;
}

int usbh_wait_connect(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (!usbh_connected()) {
        if (waited >= timeout_ms) return USBH_ERR_TIMEOUT;
        wait_ms(10);
        waited += 10;
    }
    OTG_HPRT = (OTG_HPRT & ~HPRT_W1C) | HPRT_PCDET;     /* ack the detect */
    return USBH_OK;
}

/* Port reset per the proven timing: PRST 100 ms, release, then the core
 * runs its enable sequence — poll PENA (and program HFIR only after it). */
static int port_reset_enable(usbh_device_t *dev)
{
    hprt_rmw(HPRT_PRST);
    wait_ms(100);
    OTG_HPRT &= ~(HPRT_W1C | HPRT_PRST);
    wait_ms(10);

    if (wait_reg(&OTG_HPRT, HPRT_PENA, HPRT_PENA, 1000)) {
        kprintf("usbh: port never enabled after reset\n");
        return USBH_ERR_TIMEOUT;
    }
    OTG_HPRT = (OTG_HPRT & ~HPRT_W1C) | HPRT_PENCHNG;   /* ack */

    dev->speed = (uint8_t)((OTG_HPRT & HPRT_PSPD_MASK) >> HPRT_PSPD_SHIFT);

    if (dev->speed == 2u) {             /* low speed: 6 MHz PHY clock */
        OTG_HCFG = (OTG_HCFG & ~HCFG_FSLSPCS_MASK) | 2u;
        OTG_HFIR = (OTG_HFIR & ~0xFFFFu) | 5999u;
    } else {
        OTG_HCFG = (OTG_HCFG & ~HCFG_FSLSPCS_MASK) | 1u;
        OTG_HFIR = (OTG_HFIR & ~0xFFFFu) | 47999u;
    }

    wait_ms(100);                       /* post-reset settle before EP0 */
    return USBH_OK;
}

/* ch_halt() is defined below, next to the bulk path. Without this
 * declaration stage() could not call it at all — and did not, which left a
 * timed-out EP0 channel armed with its DMA still aimed at dma_buf while the
 * other EP0 channel reused the same buffer for the retry. */
static void ch_halt(uint32_t ch);

/* ---- one channel transaction stage (buffer DMA, polled) ------------------
 * Programs a channel, rings CHENA, waits for the core to halt the channel,
 * and classifies the outcome from HCINT. IN transfers must program a whole
 * multiple of MPS; the device's short packet ends the transfer early and
 * the residue is read back from HCTSIZ.
 * Returns bytes transferred (IN) / requested len (OUT), or USBH_ERR_*. */
static int stage(usbh_device_t *dev, int is_in, uint32_t pid,
                 void *buf, uint16_t len)
{
    uint32_t ch  = is_in ? CH_IN : CH_OUT;
    uint16_t mps = dev->mps0;
    uint32_t npkt = len ? (uint32_t)((len + mps - 1u) / mps) : 1u;
    uint32_t xfrsiz = is_in ? npkt * mps : len;

    /* dma_buf is fixed size and mps0 comes from the device's own descriptor,
     * so neither the packet count nor the rounded-up IN size is inherently
     * trustworthy. Refuse rather than let the core scribble past the buffer. */
    if (xfrsiz > sizeof dma_buf || len > sizeof dma_buf)
        return USBH_ERR_XFER;

    if (!is_in && len) {
        memcpy(dma_buf, buf, len);
    }

    OTG_HCINT(ch) = HCINT_ALL;
    OTG_HCCHAR(ch) = (mps & HCCHAR_MPSIZ_MASK)
                   | (0u << HCCHAR_EPNUM_SHIFT)
                   | (is_in ? HCCHAR_EPDIR_IN : 0u)
                   | (0u << HCCHAR_EPTYP_SHIFT)
                   | (1u << HCCHAR_MCNT_SHIFT)
                   | ((uint32_t)dev->addr << HCCHAR_DAD_SHIFT)
                   | ((dev->speed == 2u) ? HCCHAR_LSDEV : 0u);
    OTG_HCTSIZ(ch) = (xfrsiz & HCTSIZ_XFRSIZ_MASK)
                   | (npkt << HCTSIZ_PKTCNT_SHIFT)
                   | (pid << HCTSIZ_DPID_SHIFT);
    OTG_HCDMA(ch) = (uint32_t)(uintptr_t)dma_buf;

    /* The core reads HCDMA's memory: order the buffer writes before the
     * Device write that arms the channel (the Ethernet doorbell lesson). */
    barrier_dsb();

    uint32_t hcchar = OTG_HCCHAR(ch) & ~HCCHAR_CHDIS;
    if (OTG_HFNUM & 1u) hcchar &= ~HCCHAR_ODDFRM;
    else                hcchar |= HCCHAR_ODDFRM;
    OTG_HCCHAR(ch) = hcchar | HCCHAR_CHENA;

    if (wait_reg(&OTG_HCINT(ch), HCINT_CHH, HCINT_CHH, 5000)) {
        /* Buffer DMA auto-retries NAKs, so a device that simply keeps NAKing
         * EP0 holds CHENA for the whole timeout and never raises CHH. Leaving
         * the channel armed lets the core keep writing into dma_buf, which
         * the OTHER EP0 channel is about to reuse for the retry. */
        ch_halt(ch);
        return USBH_ERR_TIMEOUT;
    }

    uint32_t st = OTG_HCINT(ch);
    OTG_HCINT(ch) = st;                 /* rc_w1 clear of what we saw */

    if (!usbh_connected())      return USBH_ERR_DISC;

    /* XFRC first: a completed transfer may still carry a NAK flag from the
     * core's own internal retries — success outranks the noise. */
    if (!(st & HCINT_XFRC)) {
        if (st & HCINT_STALL)   return USBH_ERR_STALL;
        if (st & HCINT_NAK)     return USBH_ERR_NAK;
        return USBH_ERR_XFER;   /* TXERR/BBERR/FRMOR/DTERR or silent halt */
    }

    if (is_in) {
        uint32_t left = OTG_HCTSIZ(ch) & HCTSIZ_XFRSIZ_MASK;
        uint32_t got  = xfrsiz - left;
        if (got > len) got = len;
        barrier_dsb();                  /* DMA writes visible before reading */
        if (got) memcpy(buf, dma_buf, got);
        return (int)got;
    }
    return (int)len;
}

int usbh_control(usbh_device_t *dev, const uint8_t setup[8], void *data)
{
    uint16_t wlen  = (uint16_t)(setup[6] | (setup[7] << 8));
    int      d2h   = setup[0] & 0x80;
    int      r     = USBH_ERR_XFER;

    /* Whole-transfer retry, the sanctioned control-endpoint recovery. */
    for (int attempt = 0; attempt < 3; attempt++) {
        r = stage(dev, 0, PID_SETUP, (void *)setup, 8);
        if (r == USBH_ERR_DISC) return r;
        if (r < 0) continue;

        int moved = 0;
        if (wlen) {
            /* Data stage: whole buffer in one program, starting DATA1 —
             * the buffer-DMA core advances the toggle per packet. */
            r = stage(dev, d2h ? 1 : 0, PID_DATA1, data, wlen);
            if (r == USBH_ERR_DISC) return r;
            if (r < 0) continue;
            moved = r;
        }

        /* Status: opposite direction, zero-length, always DATA1. */
        r = stage(dev, d2h && wlen ? 0 : 1, PID_DATA1, 0, 0);
        if (r == USBH_ERR_DISC) return r;
        if (r < 0) continue;

        return wlen ? moved : 0;
    }
    return r;
}

/* ---- standard requests + enumeration ------------------------------------- */
static int get_descriptor(usbh_device_t *dev, uint8_t type, uint8_t index,
                          void *buf, uint16_t len)
{
    uint8_t setup[8] = { 0x80, 6, index, type,
                         0, 0, (uint8_t)len, (uint8_t)(len >> 8) };
    return usbh_control(dev, setup, buf);
}

int usbh_enumerate(usbh_device_t *dev)
{
    memset(dev, 0, sizeof *dev);
    dev->addr = 0;
    pipe_next_ch = 2;                    /* EP0 keeps channels 0/1 */

    wait_ms(200);                       /* connection debounce */
    int r = port_reset_enable(dev);
    if (r != USBH_OK) return r;

    /* EP0 starts at the speed's default MPS until the device tells us. */
    dev->mps0 = (dev->speed == 2u) ? 8u : 64u;

    uint8_t tmp[8];
    r = get_descriptor(dev, 1, 0, tmp, 8);
    if (r < 8) {
        kprintf("usbh: GetDescriptor(8) failed (%d)\n", r);
        return r < 0 ? r : USBH_ERR_XFER;
    }
    /* bMaxPacketSize0 is only ever 8, 16, 32 or 64 (USB 2.0 §9.6.1). A
     * garbled first descriptor read could otherwise hand stage() a zero
     * (division by zero when computing the packet count) or a huge value. */
    if (tmp[7] == 8u || tmp[7] == 16u || tmp[7] == 32u || tmp[7] == 64u) {
        dev->mps0 = tmp[7];
    } else {
        kprintf("usbh: bMaxPacketSize0 = %u is not legal, keeping %u\n",
                tmp[7], dev->mps0);
    }

    r = get_descriptor(dev, 1, 0, dev->dev_desc, 18);
    if (r < 18) {
        kprintf("usbh: full device descriptor failed (%d)\n", r);
        return r < 0 ? r : USBH_ERR_XFER;
    }
    dev->vid = (uint16_t)(dev->dev_desc[8]  | (dev->dev_desc[9]  << 8));
    dev->pid = (uint16_t)(dev->dev_desc[10] | (dev->dev_desc[11] << 8));

    uint8_t set_addr[8] = { 0x00, 5, 1, 0, 0, 0, 0, 0 };
    r = usbh_control(dev, set_addr, 0);
    if (r < 0) {
        kprintf("usbh: SetAddress failed (%d)\n", r);
        return r;
    }
    dev->addr = 1;
    wait_ms(2);

    r = get_descriptor(dev, 2, 0, dev->cfg_desc, 9);
    if (r < 9) {
        kprintf("usbh: config header failed (%d)\n", r);
        return r < 0 ? r : USBH_ERR_XFER;
    }
    dev->cfg_total = (uint16_t)(dev->cfg_desc[2] | (dev->cfg_desc[3] << 8));
    dev->cfg_len   = (dev->cfg_total <= sizeof dev->cfg_desc)
                   ? dev->cfg_total : (uint16_t)sizeof dev->cfg_desc;

    r = get_descriptor(dev, 2, 0, dev->cfg_desc, dev->cfg_len);
    if (r < (int)dev->cfg_len) {
        kprintf("usbh: full config descriptor failed (%d)\n", r);
        return r < 0 ? r : USBH_ERR_XFER;
    }
    return USBH_OK;
}

/* ==========================================================================
 * Milestone 2: configured state + interfaces + bulk pipes
 * ========================================================================== */

/* Halt an armed channel: both CHDIS and CHENA set is the halt request; the
 * core clears CHENA when done (RM0399 §60.15.5). In DMA mode a channel that
 * is already disabled needs nothing. The bulk-IN timeout path lands here —
 * without it the channel would stay armed with the DMA aimed at the bounce. */
static void ch_halt(uint32_t ch)
{
    if ((OTG_GAHBCFG & GAHBCFG_DMAEN) && !(OTG_HCCHAR(ch) & HCCHAR_CHENA)) {
        OTG_HCINT(ch) = HCINT_ALL;
        return;
    }
    OTG_HCCHAR(ch) |= HCCHAR_CHDIS | HCCHAR_CHENA;
    (void)wait_reg(&OTG_HCCHAR(ch), HCCHAR_CHENA, 0, 100);
    OTG_HCINT(ch) = HCINT_ALL;
}

int usbh_set_configuration(usbh_device_t *dev)
{
    /* bConfigurationValue is byte 5 of the config descriptor — never assume
     * it is 1. Entering Configured resets every endpoint toggle to DATA0. */
    uint8_t cfgv = dev->cfg_desc[5];
    uint8_t setup[8] = { 0x00, 9, cfgv, 0, 0, 0, 0, 0 };
    int r = usbh_control(dev, setup, 0);
    if (r < 0) return r;
    wait_ms(10);
    return USBH_OK;
}

int usbh_find_interface_alt(const usbh_device_t *dev, uint8_t cls, uint8_t sub,
                            uint8_t proto, uint8_t index, uint8_t alt,
                            usbh_iface_t *out)
{
    const uint8_t *p   = dev->cfg_desc;
    uint16_t       len = dev->cfg_len;
    uint8_t        seen = 0;
    int            in_match = 0;

    memset(out, 0, sizeof *out);
    for (uint16_t i = 9; i + 1u < len && p[i] >= 2u; i += p[i]) {
        const uint8_t *q = p + i;
        if (q[1] == 4) {                          /* interface descriptor */
            if (in_match) return USBH_OK;         /* scope of the match ended */
            /* q[3] = bAlternateSetting. CDC-ECM data endpoints live in alt 1
             * (alt 0 is the mandatory no-endpoint setting), so the caller
             * says which alt it wants. */
            if (q[3] == alt && q[5] == cls && q[6] == sub && q[7] == proto) {
                if (seen++ == index) {
                    out->intf = q;
                    out->alt  = q[3];
                    in_match  = 1;
                }
            }
        } else if (q[1] == 5 && in_match) {       /* endpoint descriptor */
            uint8_t type = q[3] & 3u;
            if (type == 2u) {                     /* bulk */
                if (q[2] & 0x80u) out->ep_in = q;
                else              out->ep_out = q;
            } else if (type == 3u && (q[2] & 0x80u)) {
                out->ep_int = q;                  /* interrupt IN (ECM notify) */
            }
        }
    }
    return in_match ? USBH_OK : USBH_ERR_XFER;
}

int usbh_find_interface(const usbh_device_t *dev, uint8_t cls, uint8_t sub,
                        uint8_t proto, uint8_t index, usbh_iface_t *out)
{
    return usbh_find_interface_alt(dev, cls, sub, proto, index, 0, out);
}

/* SET_INTERFACE (USB 2.0 §9.4.10): standard, interface-recipient, no data.
 * An ECM data interface exposes no endpoints until its alt 1 is selected. */
int usbh_set_interface(usbh_device_t *dev, uint8_t iface, uint8_t alt)
{
    uint8_t setup[8] = { 0x01, 0x0B, alt, 0, iface, 0, 0, 0 };
    int r = usbh_control(dev, setup, 0);
    return (r < 0) ? r : USBH_OK;
}

void usbh_pipes_reset(void)
{
    /* EP0's two channels are halted too. Nothing else halts them: DISCINT is
     * unmasked in GINTMSK but the global interrupt is off and GINTSTS is
     * never polled, so a channel left armed by a disconnect or a timeout
     * would still be pointing its DMA at dma_buf when the next control
     * transfer reuses it. */
    ch_halt(CH_OUT);
    ch_halt(CH_IN);
    for (uint32_t ch = 2; ch < pipe_next_ch; ch++) ch_halt(ch);
    pipe_next_ch = 2;
}

int usbh_pipe_open(usbh_device_t *dev, const uint8_t *ep_desc, usbh_pipe_t *p)
{
    (void)dev;
    /* Channel count is silicon-dependent: GHWCFG2[17:14] + 1. */
    uint32_t nch = ((OTG_GHWCFG2 >> 14) & 0xFu) + 1u;
    if (pipe_next_ch >= nch || pipe_next_ch >= 6u)  /* 4 bounce slots */
        return USBH_ERR_INIT;
    p->ep_addr = ep_desc[2];
    p->mps     = (uint16_t)((ep_desc[4] | (ep_desc[5] << 8)) & 0x7FFu);
    p->toggle  = 0;                       /* fresh after SET_CONFIGURATION */
    p->channel = pipe_next_ch++;
    OTG_HCINT(p->channel) = HCINT_ALL;
    return USBH_OK;
}

/* One bulk transaction. usbh_bulk() wraps this to add the ZLP that a bulk
 * OUT of an exact wMaxPacketSize multiple needs to signal end-of-transfer
 * (EC25 hardening U14: the Quectel driver guide mandates it, mainline omits
 * it, so it is cheap insurance rather than a proven hard requirement). */
static int bulk_once(usbh_device_t *dev, usbh_pipe_t *p, void *buf,
                     uint16_t len, uint32_t timeout_ms)
{
    uint32_t ch    = p->channel;
    int      is_in = (p->ep_addr & 0x80u) != 0;
    uint8_t *bounce = pipe_buf[ch - 2u];
    uint32_t npkt = len ? (uint32_t)((len + p->mps - 1u) / p->mps) : 1u;
    uint32_t xfrsiz = is_in ? npkt * p->mps : len;

    if (xfrsiz > USBH_BULK_BUF) return USBH_ERR_XFER;
    if (!is_in && len) memcpy(bounce, buf, len);

    OTG_HCINT(ch) = HCINT_ALL;
    OTG_HCCHAR(ch) = (p->mps & HCCHAR_MPSIZ_MASK)
                   | ((uint32_t)(p->ep_addr & 0xFu) << HCCHAR_EPNUM_SHIFT)
                   | (is_in ? HCCHAR_EPDIR_IN : 0u)
                   | (2u << HCCHAR_EPTYP_SHIFT)          /* bulk */
                   | (1u << HCCHAR_MCNT_SHIFT)
                   | ((uint32_t)dev->addr << HCCHAR_DAD_SHIFT)
                   | ((dev->speed == 2u) ? HCCHAR_LSDEV : 0u);
    OTG_HCTSIZ(ch) = (xfrsiz & HCTSIZ_XFRSIZ_MASK)
                   | (npkt << HCTSIZ_PKTCNT_SHIFT)
                   | ((p->toggle ? PID_DATA1 : PID_DATA0) << HCTSIZ_DPID_SHIFT);
    OTG_HCDMA(ch) = (uint32_t)(uintptr_t)bounce;
    barrier_dsb();

    uint32_t hcchar = OTG_HCCHAR(ch) & ~HCCHAR_CHDIS;
    if (OTG_HFNUM & 1u) hcchar &= ~HCCHAR_ODDFRM;
    else                hcchar |= HCCHAR_ODDFRM;
    OTG_HCCHAR(ch) = hcchar | HCCHAR_CHENA;

    if (wait_reg(&OTG_HCINT(ch), HCINT_CHH, HCINT_CHH, timeout_ms)) {
        ch_halt(ch);                      /* normal for an idle IN endpoint */
        /* A transfer that spans several packets may have moved SOME of them
         * before it stalled, and the device advanced its toggle once per
         * packet. Assuming "nothing happened" was safe only while every
         * transfer was a single packet; now that one covers up to 25, an odd
         * number of completed packets leaves the two ends disagreeing and
         * every later transfer fails on DTERR -- a permanently dead pipe.
         * The core tracks the next PID in HCTSIZ, so read it back rather
         * than assume. ch_halt clears HCINT, not HCTSIZ, so this survives. */
        uint32_t tdpid = (OTG_HCTSIZ(ch) >> HCTSIZ_DPID_SHIFT) & 3u;
        p->toggle = (tdpid == PID_DATA0) ? 0u : 1u;
        return USBH_ERR_TIMEOUT;
    }

    uint32_t st = OTG_HCINT(ch);
    OTG_HCINT(ch) = st;

    if (!usbh_connected())      return USBH_ERR_DISC;
    if (!(st & HCINT_XFRC)) {
        /* STALL: the halt is cleared separately and CLEAR_FEATURE resets both
         * ends to DATA0, so leave the toggle to usbh_clear_halt().
         * NAK: nothing moved, so nothing to resync. */
        if (st & HCINT_STALL)   return USBH_ERR_STALL;
        if (st & HCINT_NAK)     return USBH_ERR_NAK;
        /* Anything else (TXERR/BBERR/DTERR/FRMOR) can land part-way through a
         * multi-packet transfer, so resync from the core for the same reason
         * as the timeout path above. */
        uint32_t edpid = (OTG_HCTSIZ(ch) >> HCTSIZ_DPID_SHIFT) & 3u;
        p->toggle = (edpid == PID_DATA0) ? 0u : 1u;
        return USBH_ERR_XFER;
    }

    /* The core advanced the toggle per packet; read the next PID back so
     * the pipe stays in sync across transfers (CherryUSB pattern). */
    uint32_t dpid = (OTG_HCTSIZ(ch) >> HCTSIZ_DPID_SHIFT) & 3u;
    p->toggle = (dpid == PID_DATA0) ? 0u : 1u;

    if (is_in) {
        uint32_t left = OTG_HCTSIZ(ch) & HCTSIZ_XFRSIZ_MASK;
        uint32_t got  = xfrsiz - left;
        if (got > len) got = len;
        barrier_dsb();
        if (got) memcpy(buf, bounce, got);
        return (int)got;
    }
    return (int)len;
}

int usbh_clear_halt(usbh_device_t *dev, usbh_pipe_t *p)
{
    /* CLEAR_FEATURE(ENDPOINT_HALT): bmRequestType 0x02 (host->device,
     * standard, endpoint recipient), bRequest 1, wValue 0 = ENDPOINT_HALT,
     * wIndex = the endpoint address.
     *
     * Without this a bulk STALL is permanent: the device answers every
     * subsequent token with STALL and the pipe is dead until the whole
     * device is re-enumerated. USB 2.0 §9.4.5 also specifies that clearing
     * the halt resets the endpoint's data toggle to DATA0, so the host's
     * copy must follow or every later transfer fails on DTERR. */
    uint8_t setup[8] = { 0x02, 0x01, 0x00, 0x00, p->ep_addr, 0x00, 0x00, 0x00 };
    int r = usbh_control(dev, setup, 0);
    if (r < 0) return r;
    p->toggle = 0u;
    return USBH_OK;
}

int usbh_bulk(usbh_device_t *dev, usbh_pipe_t *p, void *buf, uint16_t len,
              uint32_t timeout_ms)
{
    int r = bulk_once(dev, p, buf, len, timeout_ms);

    /* A STALL is recoverable exactly once per transfer: clear the halt, put
     * the toggle back to DATA0 with the device, and try again. A second
     * STALL is the endpoint genuinely refusing, and is reported. */
    if (r == USBH_ERR_STALL) {
        if (usbh_clear_halt(dev, p) == USBH_OK)
            r = bulk_once(dev, p, buf, len, timeout_ms);
    }
    if (r < 0) return r;

    /* Terminate an exact-multiple bulk OUT with a zero-length packet, or the
     * device cannot tell the transfer ended (U14). IN transfers end on their
     * own short packet, so they never need this. */
    int is_in = (p->ep_addr & 0x80u) != 0;
    if (!is_in && len && (len % p->mps) == 0u) {
        int z = bulk_once(dev, p, 0, 0, timeout_ms);
        if (z < 0) return z;
    }
    return r;
}

/* SET_CONTROL_LINE_STATE (CDC 0x21/0x22) to a vendor-serial interface:
 * DTR (bit0) and RTS (bit1). The EC25 answers AT without it, but mainline
 * usb_wwan asserts it on open and it is a prerequisite for later QMI use;
 * the modem ignores an unsupported request, so a STALL here is not fatal —
 * callers treat any non-DISC result as "line set as far as it matters." */
/* Read one complete USB "transfer" (an Ethernet frame on an ECM data pipe).
 * A frame larger than the endpoint's max packet size arrives as several
 * back-to-back full packets terminated by a SHORT packet (possibly zero
 * length) — that short packet is the end-of-transfer marker (USB 2.0 §5.8.3).
 * Returns total bytes, 0 if nothing arrived, or USBH_ERR_*.
 *
 * Written for CDC-ECM: a 1514-byte frame is 24 packets at FS MPS 64. */
int usbh_bulk_frame(usbh_device_t *dev, usbh_pipe_t *p, void *buf,
                    uint16_t cap, uint32_t timeout_ms)
{
    uint8_t *dst = (uint8_t *)buf;
    uint16_t total = 0;

    for (;;) {
        if (total >= cap) return (int)total;      /* caller's buffer is full */

        /* Ask for as many WHOLE packets as the bounce buffer holds, not one
         * packet at a time. In buffer-DMA the core runs the whole multi-packet
         * IN itself and stops on the device's short packet, so a 1428-byte
         * frame costs ONE transaction instead of 23 -- each of which was a
         * channel program, a spin on CHH and a bounce-buffer memcpy.
         *
         * The size must be a whole multiple of MPS: bulk_once rounds an IN up
         * to npkt*mps, and anything the device sends past `want` would be
         * written into the bounce buffer and then dropped by the got>len clamp
         * -- silent data loss rather than a short read. */
        uint16_t want = (uint16_t)(cap - total);
        if (want > USBH_BULK_BUF) want = USBH_BULK_BUF;
        want = (uint16_t)(want - (want % p->mps));
        if (want == 0) return (int)total;         /* less than one packet left */

        int r = usbh_bulk(dev, p, dst + total, want,
                          total ? 100u : timeout_ms);
        if (r < 0) {
            /* Nothing yet on the first packet is simply "no frame". */
            if (total == 0) return (r == USBH_ERR_TIMEOUT) ? 0 : r;
            /* Mid-frame, only a TIMEOUT is benign: it means the device had
             * nothing more to send, so a frame that happened to be an exact
             * multiple of MPS ends here. Any other error means the frame is
             * incomplete, and returning a positive count would make it
             * indistinguishable from a valid short frame — which is how a
             * halted endpoint ends up being polled forever. */
            if (r == USBH_ERR_TIMEOUT) return (int)total;
            return r;
        }
        total = (uint16_t)(total + r);
        /* The core stops a multi-packet IN on the device's first short
         * packet, so getting less than we asked for means the frame ended.
         * Getting exactly `want` means the buffer filled and there may be
         * more, so loop. (Comparing against p->mps was only correct while
         * every request was a single packet.) */
        if ((uint16_t)r < want) return (int)total;
    }
}

int usbh_set_line_state(usbh_device_t *dev, uint8_t iface, int dtr, int rts)
{
    uint16_t v = (uint16_t)((dtr ? 1u : 0u) | (rts ? 2u : 0u));
    uint8_t setup[8] = { 0x21, 0x22, (uint8_t)v, (uint8_t)(v >> 8),
                         iface, 0, 0, 0 };
    int r = usbh_control(dev, setup, 0);
    if (r == USBH_ERR_DISC) return r;
    return USBH_OK;
}
