# USB2 OTG_FS host-mode register notes — STM32H745 (RM0399)

Extracted for the bare-metal **FS USB host driver** on the Nucleo-H745ZI-Q's CN13
connector. Every offset, bit position and encoding below was read out of
`RM0399-stm32h745-reference-manual.pdf` (**Rev 4, June 2023**) on this machine —
not remembered from other STM32 parts. Cross-checks against
`DS12923-stm32h745zi-datasheet.pdf` (Rev 3), `UM2408` (Rev 6) and the `MB1363`
schematic where noted.

**Page numbers are PDF pages** (viewer page box), same convention as INDEX.md.

---

## 0 — Which USB instance is this?

The RM has **one** OTG chapter (§60, "USB on-the-go high-speed (OTG_HS)",
PDF pp.2743–2930) describing **both** instances. "OTG_HS2, alias OTG_FS" is the
one on PA11/PA12.

| Fact | Source (PDF page) |
|---|---|
| **USB2 OTG_FS base = 0x4008 0000** (0x40080000–0x400BFFFF, AHB1/D2) | memory map, p.140 |
| USB1 OTG_HS base = 0x4004 0000 (0x40040000–0x4007FFFF) | p.140 |
| Two instances of the same Synopsys core; **only OTG_HS1 has ULPI**; OTG_HS2 is "incompatible with high speed operation" (integrated FS PHY only) | §60.3/§60.4.1 Table 530, pp.2746–2747 |
| Per instance: **16 host channels**, 9 bidirectional device EPs, **4 KB dedicated FIFO SRAM (= 1024 × 32-bit words)** | Table 530, p.2746; §60.4.3 p.2749 |
| Scatter/gather DMA: OTG_HS1 yes, **OTG_HS2 no** (slave mode is the safe choice for this driver) | Table 530, p.2746 |
| **PA11 = OTG_FS_DM, PA12 = OTG_FS_DP — AF10** (AF10 row = "OTG1_HS/OTG2_FS" group). PA9 = OTG_FS_VBUS, PA10 = OTG_FS_ID, PA8 = OTG_FS_SOF | DS12923 pin table p.77, AF Table 9 pp.86–87 |

### Board wiring (Nucleo-H745ZI-Q, MB1363)

| Fact | Source |
|---|---|
| CN13 = USB Micro-AB, VBUS switched by **U18 power switch**; **PD10 = USB_FS_PWR_EN, requires JP8 ON** on the H745ZI-Q | UM2408 §7.11 + Table 13, p.30 |
| PA9 VBUS sense via SB21 (ON by default), PA10 ID via SB29, PA11/PA12 via SB26/SB27 | UM2408 Table 13, p.30 |
| U18 = **STMPS2141STR** (schematic p.7). Per the STMPS2141 part (not the schematic text) its enable is **active-low** → PD10 = 0 should turn VBUS on. *Verify polarity on the bench before trusting it.* | MB1363 p.7 |
| CN13 cannot power the board — power the board first, warns of current injection otherwise | UM2408 p.30 |

---

## 1 — Chapter map (where to read more)

| What | Section | PDF page |
|---|---|---|
| OTG chapter start | §60 | 2743 |
| Implementation table (channels, SRAM, per-instance features) | §60.3 | 2746 |
| Core / 48 MHz kernel clock requirement | §60.4.3 | 2748–2749 |
| **AHB frequency must be > 30 MHz** (caution) | §60.4.3 | 2749 |
| CSR memory map (global / host / device / DFIFO tables) | §60.13.1 | 2769–2774 |
| Global register bitfields | §60.14.1–.17 | 2774–2805 |
| Host register bitfields | §60.14.20–.37 | 2807–2824 |
| Register map + reset values (Table 542) | §60.14.67 | 2856–2866 |
| Core initialization sequence | §60.15.1 | 2868 |
| **Host initialization sequence (14 steps)** | §60.15.2 | 2869 |
| Host channel init / **halting a channel** / ping / operational model | §60.15.5 | 2871–2872+ |

> **Trap:** registers must be accessed as **32-bit words only** (p.2769), and
> host-mode registers must not be touched while the core is in device mode —
> that raises MMIS (p.2769).

---

## 2 — Register offsets (from Tables 535/536/538, pp.2769–2774)

All offsets relative to 0x4008 0000.

### Global CSRs

| Register | Offset | Reset | PDF page (bitfields) |
|---|---|---|---|
| OTG_GOTGCTL | 0x000 | 0x0001 0000 | 2774–2777 |
| OTG_GOTGINT | 0x004 | 0x0000 0000 | 2777–2778 |
| OTG_GAHBCFG | 0x008 | 0x0000 0000 | 2779–2780 |
| OTG_GUSBCFG | 0x00C | 0x0000 1400 | 2780–2783 |
| OTG_GRSTCTL | 0x010 | 0x8000 0000 | 2783–2785 |
| OTG_GINTSTS | 0x014 | 0x0400 0020 | 2786–2790 |
| OTG_GINTMSK | 0x018 | 0x0000 0000 | 2790–2792 |
| OTG_GRXSTSR / OTG_GRXSTSP | 0x01C / 0x020 | — | 2796–2797 |
| OTG_GRXFSIZ | 0x024 | 0x0000 0400 | 2798 |
| OTG_HNPTXFSIZ (host) / DIEPTXF0 (device) | 0x028 | 0x0200 0200 | 2798–2799 |
| OTG_HNPTXSTS | 0x02C | 0x0008 0400 | 2799–2800 |
| OTG_GCCFG | 0x038 | 0x0000 XXXX | 2800–2801 |
| OTG_CID | 0x03C | 0x0000 2300 | 2802 |
| OTG_GLPMCFG | 0x054 | 0x0000 0000 | 2802 |
| OTG_HPTXFSIZ | 0x100 | 0x0400 0800 | 2806 |

### Host CSRs

| Register | Offset | Reset | PDF page |
|---|---|---|---|
| OTG_HCFG | 0x400 | 0x0000 0000 | 2807–2808 |
| OTG_HFIR | 0x404 | 0x0000 EA60 | 2808–2809 |
| OTG_HFNUM | 0x408 | 0x0000 3FFF | 2809 |
| OTG_HPTXSTS | 0x410 | 0x0008 0100 | 2810 |
| OTG_HAINT | 0x414 | 0x0000 0000 | 2811 |
| OTG_HAINTMSK | 0x418 | 0x0000 0000 | 2811 |
| OTG_HPRT | 0x440 | 0x0000 0000 | 2812–2814 |
| OTG_HCCHARx | 0x500 + 0x20·x (x = 0..15) | 0x0000 0000 | 2815–2816 |
| OTG_HCINTx | 0x508 + 0x20·x | 0x0000 0000 | 2817–2818 |
| OTG_HCINTMSKx | 0x50C + 0x20·x | 0x0000 0000 | 2818–2820 |
| OTG_HCTSIZx | 0x510 + 0x20·x | 0x0000 0000 | 2820 |

### DFIFO access (Table 538, p.2774)

Channel x FIFO window = **0x1000 + 0x1000·x**, i.e. channel 0 at
0x1000–0x1FFC, channel 1 at 0x2000–0x2FFC, … up to x = 15 in host mode.
Write for OUT channels, read for IN channels; always whole 32-bit words, pad
non-aligned packets (p.2872).

---

## 3 — Bitfields (host-relevant registers)

### OTG_GOTGCTL — 0x000 (pp.2774–2777)

| Bit | Name | Notes |
|---|---|---|
| 21 | CURMOD (r) | 1 = host mode |
| 20 | OTGVER | 1 = OTG 2.0 |
| 18 | ASVLD (r) | A-session valid (host) |
| 16 | CIDSTS (r) | connector ID |
| 5/4 | AVALOVAL / AVALOEN | force Avalid (host) |
| 3/2 | VBVALOVAL / VBVALOEN | force vbusvalid (host) — useful when not sensing VBUS on PA9 |

### OTG_GAHBCFG — 0x008 (pp.2779–2780)

| Bit | Name | Notes |
|---|---|---|
| 8 | PTXFELVL | periodic TxFIFO empty level (0 = half, 1 = completely empty) |
| 7 | TXFELVL | non-periodic TxFIFO empty level (host) |
| 5 | DMAEN | 0 = slave mode |
| 4:1 | HBSTLEN[3:0] | AHB burst (DMA only) |
| 0 | **GINTMSK** | global interrupt enable (1 = unmasked) |

### OTG_GUSBCFG — 0x00C, reset 0x0000 1400 (pp.2780–2783)

| Bit | Name | Notes |
|---|---|---|
| 30 | FDMOD | force device mode — **wait ≥ 25 ms** after setting |
| 29 | FHMOD | force host mode — **wait ≥ 25 ms** after setting |
| 13:10 | TRDT[3:0] | turnaround time. **Table 540 (p.2783) has a single row: AHB ≥ 30 MHz → TRDT = 0x9.** (Bit text says "device mode", set it anyway during core init per §60.15.1) |
| 9 | HNPCAP | 0 for a plain embedded host |
| 8 | SRPCAP | 0 for a plain embedded host |
| 6 | **PHYSEL** | **1 = USB 1.1 full-speed serial transceiver** (mandatory on USB2 — no ULPI). Reset value is 0; changing PHYSEL requires a core soft reset afterwards (p.2785) |
| 2:0 | TOCAL[2:0] | FS timeout calibration, +0.25 bit-times per PHY clock |

### OTG_GRSTCTL — 0x010, reset 0x8000 0000 (pp.2783–2785)

| Bit | Name | Notes |
|---|---|---|
| 31 | **AHBIDL** (r) | AHB master idle — poll =1 before/after resets and before TXFFLSH |
| 10:6 | TXFNUM[4:0] | 0x00 = non-periodic TxFIFO (host), 0x01 = periodic TxFIFO (host), **0x10 = flush all** |
| 5 | TXFFLSH (rs) | self-clearing, takes 8 clocks of the slower of phy_clk/hclk |
| 4 | RXFFLSH (rs) | same timing |
| 2 | FCRST | host frame counter reset |
| 0 | **CSRST** (rs) | core soft reset, self-clearing. Preserves FSLSPCS, GCCFG, DSPD, SDIS, GATEHCLK, STPPCLK. After it clears: **wait ≥ 3 PHY clocks and check AHBIDL = 1** before touching the PHY domain |

### OTG_GINTSTS — 0x014, reset 0x0400 0020 (pp.2786–2790)

Host-relevant bits, with access type. `rc_w1` = write-1-to-clear.

| Bit | Name | Access | Meaning (host) |
|---|---|---|---|
| 31 | WKUPINT | rc_w1 | remote wake-up detected |
| 30 | SRQINT | rc_w1 | session request detected |
| 29 | **DISCINT** | rc_w1 | device disconnect detected |
| 28 | CIDSCHG | rc_w1 | connector ID change |
| 26 | PTXFE | r | periodic TxFIFO empty |
| 25 | **HCINT** | r | channel interrupt → read HAINT → HCINTx (cleared by clearing HCINTx bits) |
| 24 | **HPRTINT** | r | port interrupt → read HPRT (cleared by clearing HPRT rc_w1 bits) |
| 21 | IPXFR | rc_w1 | incomplete periodic transfer |
| 5 | NPTXFE | r | non-periodic TxFIFO empty |
| 4 | **RXFLVL** | r | RxFIFO non-empty (cleared by popping) |
| 3 | SOF | rc_w1 | SOF sent (may read 1 right after reset — clear it) |
| 2 | OTGINT | r | → read GOTGINT |
| 1 | MMIS | rc_w1 | mode mismatch register access |
| 0 | CMOD | r | 1 = host |

Full rc_w1 set: WKUPINT, SRQINT, DISCINT, CIDSCHG, LPMINT(27), RSTDET(23),
DATAFSUSP(22), IPXFR(21), IISOIXFR(20), EOPF(15), ISOODRP(14), ENUMDNE(13),
USBRST(12), USBSUSP(11), ESUSP(10), SOF(3), MMIS(1). Everything else is
read-only status. Clear GINTSTS once at init before unmasking (p.2786).

### OTG_GINTMSK — 0x018 (pp.2790–2792)

Same bit layout as GINTSTS, 1 = unmasked. Host set: WUIM(31), DISCINT(29),
PTXFEM(26), **HCIM(25)**, **PRTIM(24)**, IPXFRM(21), NPTXFEM(5), RXFLVLM(4),
SOFM(3), OTGINT(2), MMISM(1).

### OTG_GCCFG — 0x038 (H7-specific — NOT the F4 layout!) (pp.2800–2801)

| Bit | Name | Notes |
|---|---|---|
| 21 | **VBDEN** | VBUS sensing comparators enable (PA9 pad). For host with sensed VBUS: set. Without sensing: leave 0 and use GOTGCTL VBVALOEN/VBVALOVAL + AVALOEN/AVALOVAL overrides |
| 20/19/18/17 | SDEN/PDEN/DCDEN/BCDEN | battery-charging detector — keep 0 for normal USB (BCD owns the PHY when BCDEN = 1) |
| 16 | **PWRDWN** | **1 = FS PHY enabled** (reset state 0 keeps the transceiver powered down) |
| 3:0 | PS2DET/SDET/PDET/DCDET (r) | BCD status |

There is no NOVBUSSENS bit on H7 (that is F4); the H7 equivalents are VBDEN
and the GOTGCTL override bits.

### OTG_GRXFSIZ — 0x024 (p.2798)

RXFD[15:0] = Rx FIFO depth in 32-bit words, **max 1024** (= whole 4 KB RAM).

### OTG_HNPTXFSIZ — 0x028 (host view, p.2799)

NPTXFSA[15:0] = non-periodic TxFIFO start address (words),
NPTXFD[31:16] = depth (min 16 words). Host uses this register; the device
alias is DIEPTXF0.

### OTG_HPTXFSIZ — 0x100 (p.2806)

PTXSA[15:0] = periodic TxFIFO start address, PTXFSIZ[31:16] = depth (min 16).
Budget: RXFD + NPTXFD + PTXFSIZ ≤ 1024 words.

### OTG_HNPTXSTS — 0x02C (r, pp.2799–2800)

NPTXFSAV[15:0] words free in non-periodic TxFIFO; NPTQXSAV[23:16] queue
entries free (0..8); NPTXQTOP[30:24] top-of-queue debug.

### OTG_HCFG — 0x400 (pp.2807–2808)

| Bit | Name | Notes |
|---|---|---|
| 23 | DESCDMA | keep 0 (scatter/gather unsupported on USB2 anyway) |
| 2 | FSLSS (r per diagram) | FS/LS-only support |
| 1:0 | **FSLSPCS[1:0]** | FS/LS PHY clock select. **FS host: 01 = 48 MHz (only legal value)**; LS host: 01 = 48 MHz or 10 = 6 MHz. **Changing it requires a software reset** (note, p.2808) |

### OTG_HFIR — 0x404, reset 0x0000 EA60 (pp.2808–2809)

FRIVL[15:0] = frame interval in PHY clocks (0xEA60 = 60000; at 48 MHz the FS
frame is 48000). Writable only after PENA is set; if not written, the core
derives it from FSLSPCS. RLDCTRL (bit 16): **RLDCTRL = 0 is not recommended**
(caution, p.2809) — set it at init if you ever adjust HFIR.

### OTG_HFNUM — 0x408 (r, p.2809)

FRNUM[15:0] frame number (wraps at 0x3FFF), FTREM[31:16] PHY clocks left in
the current frame (use for periodic scheduling).

### OTG_HPTXSTS — 0x410 (r, p.2810)

PTXFSAVL[15:0] words free in periodic TxFIFO; PTXQSAV[23:16] queue entries
free (0..8); PTXQTOP[31:24] debug (bit 31 odd/even frame).

### OTG_HAINT — 0x414 (r) / OTG_HAINTMSK — 0x418 (p.2811)

HAINT[15:0]: one read-only bit per channel (bit x = channel x); cleared by
clearing HCINTx bits. HAINTM[15:0]: rw mask.

### OTG_HPRT — 0x440 (pp.2812–2814) — the read-modify-write trap

| Bit | Name | Access | Semantics |
|---|---|---|---|
| 18:17 | PSPD[1:0] | r | attached device speed: **01 = FS, 10 = LS** (00 = HS won't occur on USB2, 11 reserved) |
| 16:13 | PTCTL[3:0] | rw | test modes, keep 0000 |
| 12 | PPWR | rw | port power. 1 = on (drives the core's VBUS-enable; the board's actual 5 V comes from PD10/U18). Cleared by core on overcurrent |
| 11:10 | PLSTS[1:0] | r | line state: bit 10 = DP level, bit 11 = DM level |
| 8 | PRST | rw | port reset. Application-timed: set, **hold ≥ 10 ms (FS/LS)**, then clear. 50 ms figure is for HS only |
| 7 | PSUSP | rs | port suspend (cleared by core on wake-up/reset/resume) |
| 6 | PRES | rw | drive resume signaling |
| 5 | **POCCHNG** | **rc_w1** | overcurrent change |
| 4 | POCA | r | overcurrent active |
| 3 | **PENCHNG** | **rc_w1** | port enable change (fires when core enables port after reset) |
| 2 | **PENA** | **rc_w1** | port enabled. **Set only by the core** after a reset sequence. **Writing 1 clears it (= disables the port)!** |
| 1 | **PCDET** | **rc_w1** | connect detected |
| 0 | PCSTS | r | 1 = device attached |

> **Trap (the classic one):** PENA, PENCHNG, PCDET, POCCHNG are all
> write-1-to-clear. A naive `HPRT |= x` read-modify-write reads PENA = 1 and
> writes it back, disabling the port. Every HPRT write must mask off
> `PENA | PENCHNG | PCDET | POCCHNG` first, then OR in exactly the w1c bit or
> control bit intended.

### OTG_HCCHARx — 0x500 + 0x20·x (pp.2815–2816)

| Bit | Name | Notes |
|---|---|---|
| 31 | **CHENA** (rs) | set by app, cleared by core (slave mode) |
| 30 | **CHDIS** (rs) | set to halt; wait for CHH interrupt before reusing the channel |
| 29 | ODDFRM | periodic transfers: 1 = execute in odd frame |
| 28:22 | DAD[6:0] | device address |
| 21:20 | MCNT[1:0] | transactions/frame for periodic EPs — **must be ≥ 01 even for non-periodic** |
| 19:18 | EPTYP[1:0] | 00 control, 01 iso, 10 bulk, 11 interrupt |
| 17 | LSDEV | 1 = LS device (via this FS port) |
| 15 | EPDIR | 0 = OUT, 1 = IN |
| 14:11 | EPNUM[3:0] | endpoint number |
| 10:0 | MPSIZ[10:0] | max packet size |

### OTG_HCINTx — 0x508 + 0x20·x (pp.2817–2818) — ALL bits rc_w1

| Bit | Name | | Bit | Name |
|---|---|---|---|---|
| 10 | DTERR | | 4 | NAK |
| 9 | FRMOR | | 3 | STALL |
| 8 | BBERR | | 2 | AHBERR (DMA only) |
| 7 | TXERR (CRC/timeout/bit-stuff/false-EOP) | | 1 | **CHH** (channel halted) |
| 6 | NYET | | 0 | **XFRC** (transfer complete, no errors) |
| 5 | ACK | | | |

(Bits 13:11 DESCLSTROLL/XCSXACTERR/BNA are scatter/gather-DMA-only —
reserved here.) OTG_HCINTMSKx at 0x50C + 0x20·x mirrors these as rw mask
bits (pp.2818–2820).

### OTG_HCTSIZx — 0x510 + 0x20·x (p.2820)

| Bit | Name | Notes |
|---|---|---|
| 31 | DOPNG | HS ping only — never set for IN (it disables the channel) |
| 30:29 | **DPID[1:0]** | initial PID: **00 = DATA0, 01 = DATA2, 10 = DATA1, 11 = SETUP (control) / MDATA** |
| 28:19 | PKTCNT[9:0] | expected packet count; decremented per packet, IRQ at 0 |
| 18:0 | XFRSIZ[18:0] | bytes. For IN: buffer size, **must be an integer multiple of MPSIZ** |

### OTG_GRXSTSP — 0x020 (host view, p.2797)

CHNUM[3:0], BCNT[14:4], DPID[16:15], PKTSTS[20:17]: **0010 = IN data packet,
0011 = IN transfer completed, 0101 = data-toggle error, 0111 = channel
halted** (the latter three just trigger; no data follows). Pop only while
RXFLVL = 1; reading when empty returns 0.

---

## 4 — Clocks (RCC, §9)

### Enable / reset

| Fact | Source |
|---|---|
| **RCC_AHB1ENR (offset 0x0D8; C1 bank 0x138, C2 bank 0x198): bit 27 = USB2OTGHSEN** — USB2 peripheral clocks = USBSEL kernel clock + rcc_hclk1. Bit 25 = USB1OTGHSEN; bits 28/26 = ULPI clocks (irrelevant for USB2 FS) | pp.487–488 |
| RCC_AHB1RSTR (0x080): bit 27 = USB2OTGRST, bit 25 = USB1OTGRST | p.463 |
| **RCC_D2CCIP2R (0x054): USBSEL[21:20]** — 00 kernel clock disabled (reset), **01 = pll1_q_ck, 10 = pll3_q_ck, 11 = hsi48_ck**. Shared by USB1 and USB2 | p.447 |
| The core "receives the 48 MHz clock from RCC… must be enabled prior to configuring the OTG core" | §60.4.3 p.2748 |

### HSE (RCC_CR, offset 0x000, pp.415–416)

| Bit | Name |
|---|---|
| 16 | HSEON |
| 17 | HSERDY (r) |
| 18 | HSEBYP — writable only while HSE is disabled; set before HSEON for the Nucleo's 8 MHz ST-LINK-MCO bypass clock |
| 19 | HSECSSON |
| 28 | **PLL3ON** |
| 29 | **PLL3RDY** (r) |

### PLL3 programming

| Register (offset) | Field (bits) | Encoding | Source |
|---|---|---|---|
| RCC_PLLCKSELR (0x028) | PLLSRC[1:0] | 00 HSI, 01 CSI, **10 HSE**, 11 none. **Shared by all 3 PLLs, writable only when all PLLs are off** | pp.429–430 |
| RCC_PLLCKSELR | **DIVM3[25:20]** | direct: 1 = ÷1 … 63 = ÷63; 0 = disabled. Locked while PLL3ON = 1 | p.429 |
| RCC_PLLCFGR (0x02C) | **PLL3RGE[11:10]** | ref3_ck range: **00 = 1–2 MHz, 01 = 2–4, 10 = 4–8, 11 = 8–16 MHz.** Write before PLL3ON | p.432 |
| RCC_PLLCFGR | **PLL3VCOSEL (bit 9)** | 0 = wide VCO **192–960 MHz**, 1 = medium VCO **150–420 MHz** | p.432 |
| RCC_PLLCFGR | PLL3FRACEN (bit 8) | 0 for integer mode | p.432 |
| RCC_PLLCFGR | **DIVQ3EN (bit 23)**, DIVP3EN (22), DIVR3EN (24) | output gates; unused outputs: set DIVyEN = 0 **and** DIVy = 0 | pp.431, 374 |
| RCC_PLL3DIVR (0x040) | **DIVN3[8:0]** | **stored value = N − 1** ("0x003: DIVN3 = 4 … 0x1FF: DIVN3 = 512"); legal N = 4…512 | p.441 |
| RCC_PLL3DIVR | **DIVQ3[22:16]** | **stored value = divider − 1** ("0000001: pll3_q_ck = vco3_ck / 2") | p.441 |
| RCC_PLL3DIVR | DIVP3[15:9], DIVR3[30:24] | same value+1 encoding | pp.440–441 |
| — | all PLL3DIVR fields + RGE/VCOSEL/DIVyEN | writable **only when PLL3ON = 0 and PLL3RDY = 0** | pp.440–441, 374 |

**VCO-selection rule (p.373, easy to miss):** "if the frequency of the
reference clock is lower or equal to 2 MHz, then VCOL must be selected,
otherwise VCOH" — i.e. ref ≤ 2 MHz **forces PLL3VCOSEL = 1 (150–420 MHz)**;
the wide VCO's input range is 2–16 MHz.

### Verified PLL3 recipe: 8 MHz HSE (bypass) → 48.000 MHz pll3_q_ck

Chosen so every parameter sits strictly *inside* its legal range (8 MHz with
power-of-two DIVM always lands on a range boundary; this avoids that):

| Step | Value | Check (all from pp.373, 432, 441, 447) |
|---|---|---|
| RCC_CR | HSEBYP = 1 then HSEON = 1, wait HSERDY | Nucleo feeds 8 MHz from ST-LINK MCO |
| RCC_PLLCKSELR | PLLSRC = 10 (HSE), **DIVM3 = 5** | ref3_ck = 8/5 = **1.6 MHz** — strictly inside 1–2 MHz |
| RCC_PLLCFGR | **PLL3RGE = 00** (1–2 MHz), **PLL3VCOSEL = 1** (medium VCO — mandatory at ≤ 2 MHz ref), PLL3FRACEN = 0, DIVQ3EN = 1, DIVP3EN = DIVR3EN = 0 | |
| RCC_PLL3DIVR | **DIVN3 = 149** (× 150), **DIVQ3 = 4** (÷ 5), DIVP3 = DIVR3 = 0 | VCO = 1.6 × 150 = **240 MHz** — inside 150–420; 240/5 = **48.000 MHz exact** |
| RCC_CR | PLL3ON = 1, wait PLL3RDY = 1 | |
| RCC_D2CCIP2R | USBSEL = 10 (pll3_q_ck) | |
| RCC_AHB1ENR (C1 bank) | USB2OTGHSEN = 1 | |

> The "DIVM3 = 4, RGE = 00, N = 96, wide VCO" folk config is doubly wrong per
> this manual: a 2 MHz ref is the boundary of RGE 00/01, **and** with wide
> VCOSEL it violates the p.373 rule (wide VCO wants ref > 2 MHz) while putting
> the VCO at exactly 192 MHz — the wide range's minimum, zero margin.
> Alternative if PLL1 already has a spare Q output at 48 MHz: USBSEL = 01.
> HSI48 (USBSEL = 11) exists but is crystal-less; for a host (which cannot
> CRS-sync to anything) prefer the HSE-derived PLL3.

---

## 5 — PWR: the USB 3.3 V transceiver supply (PWR_CR3, offset 0x00C)

| Bit | Name | Meaning | Source |
|---|---|---|---|
| 26 | **USB33RDY** (r) | USB33 supply ready | p.329 |
| 25 | **USBREGEN** | USB regulator enable (VDD33USB from internal regulator) | p.330 |
| 24 | **USB33DEN** | VDD33USB voltage level detector enable — **required before using the transceivers in either supply mode** | pp.285, 330 |

Order (§7.4.7, p.285): set USBREGEN = 1 (if using the internal regulator),
set USB33DEN = 1, then poll USB33RDY = 1 before touching the PHY. Reset value
of PWR_CR3 is 0x0000 0006; the write-once lock applies only to the **lower
byte** (supply config) — the USB bits in the upper bytes are freely writable.

---

## 6 — Interrupt vectors

The NVIC table is **Table 149** (§20.2, pp.789–796) — *not* Table 143, which
is "Data order in memory" (p.761). NVIC1 (CM7) and NVIC2 (CM4) positions are
identical for the USB rows:

| Vector | IRQn | Vector address | Source |
|---|---|---|---|
| **OTG_FS (USB2) global** — `usb2_gbl_it` | **101** | 0x0000 01D4 | p.794 |
| OTG_FS_EP1_OUT / EP1_IN / WKUP | 98 / 99 / 100 | 0x01C8 / 0x01CC / 0x01D0 | p.794 |
| OTG_HS (USB1) global — `usb1_gbl_it` | **77** | 0x0000 0174 | p.793 |
| OTG_HS_EP1_OUT / EP1_IN / WKUP | 74 / 75 / 76 | 0x0168 / 0x016C / 0x0170 | p.793 |

---

## 7 — FS-host bring-up checklist (all cautions in one place)

1. **AHB (rcc_hclk1) > 30 MHz** or the peripheral is out of spec (p.2749).
2. 48 MHz kernel clock selected (USBSEL) and AHB1ENR bit 27 set **before**
   touching any OTG register (p.2748).
3. PWR: USBREGEN/USB33DEN, wait USB33RDY (p.285).
4. GPIO: PA11/PA12 → AF10; PD10 (JP8 ON) drives U18 for real 5 V VBUS.
5. GUSBCFG.PHYSEL = 1, then **CSRST**; after CSRST self-clears wait ≥ 3 PHY
   clocks and poll AHBIDL = 1 (p.2785).
6. GCCFG: PWRDWN = 1 (PHY on); VBDEN = 1 if sensing PA9, else the GOTGCTL
   override bits (pp.2800–2801).
7. GUSBCFG: FHMOD = 1 and **wait ≥ 25 ms** (p.2781), TRDT = 0x9 (p.2783).
8. HCFG.FSLSPCS = 01 (48 MHz) — the only legal FS value; software reset after
   any change to it (p.2808).
9. Host init sequence §60.15.2 (p.2869): unmask HPRTINT → PPWR = 1 → wait
   PCDET → PRST = 1, **hold ≥ 10 ms**, PRST = 0 → wait PENCHNG → read PSPD →
   size FIFOs (GRXFSIZ, HNPTXFSIZ, HPTXFSIZ; budget ≤ 1024 words) → flush all
   TxFIFOs (TXFNUM = 0x10) + RxFIFO.
10. **Every HPRT write masks PENA/PENCHNG/PCDET/POCCHNG** (§3 above).
11. Channel halt protocol (p.2871): set CHDIS = 1 **and** CHENA = 1 together,
    wait for CHH in HCINTx before reusing the channel; ensure ≥ 1 free
    request-queue entry first (CHDIS alone with a full queue just flushes and
    clears CHENA). Halt all channels on DISCINT.
12. Disable a channel after STALL/TXERR/BBERR/DTERR, and after XFRC on
    non-periodic IN (p.2872).
13. SETUP packets: HCTSIZ.DPID = 11; DATA1 = 10, DATA0 = 00.
14. Slave-mode FIFO discipline: check NPTXSTS/HPTXSTS space (words **and**
    queue entries) before pushing; whole-word writes with padding (p.2872).
