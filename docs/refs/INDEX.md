# Reference documents — STM32H745 Ethernet driver

The PDFs themselves are gitignored (~99 MB, vendor copyright). This index makes
the set reproducible: every entry has its download link, and the page numbers
below were read out of the actual files, not remembered.

**Page numbers are PDF pages** (what your viewer's page box shows), not the
printed page numbers in the footer. The two differ by a few pages in ST docs.

---

## Tier 1 — needed now (milestone 1: MDIO)

### RM0399 — STM32H745/755 reference manual
`RM0399-stm32h745-reference-manual.pdf` · Rev 4, June 2023 · 3556 pp ·
[download](https://www.st.com/resource/en/reference_manual/rm0399-stm32h745755-and-stm32h747757-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)

The Ethernet peripheral is **section 61**, starting at PDF **p.2931**.

| What | Section | PDF page |
|---|---|---|
| Ethernet chapter start | §61 | 2931 |
| Ethernet pins and internal signals | §61.3 | 2935 |
| PHY interfaces (RMII, MDIO) | §61.6 | 3005 |
| **MDIO clause 22 frame structure** (Fig. 840, Table 574) | §61.6 | 3007–3008 |
| Programming model | §61.9 | 3031 |
| Register descriptions | §61.11 | 3074 |
| **`ETH_MACMDIOAR` / `ETH_MACMDIODR` full bitfields** | §61.11 | 3150–3152 |
| **CSR clock range (`CR`) divider table** | §61.11 | 3151 |
| `RCC_AHB1ENR` (ETH1MACEN/TXEN/RXEN) | §9 | 487 |

> **Trap:** section **57 is "MDIOS"** (PDF p.2518) — a completely different
> peripheral that makes the STM32 an MDIO *slave*. If you search the PDF for
> "MDIO" you will land there first. It is not what you want.

### DS12923 — STM32H745ZI datasheet
`DS12923-stm32h745zi-datasheet.pdf` · Rev 3, January 2026 · 245 pp ·
[download](https://www.st.com/resource/en/datasheet/stm32h745zi.pdf)

| What | PDF page |
|---|---|
| Pinout, pin description and alternate functions | 53 |
| **Table 9 — Alternate functions** (the AF11 proof) | 86 |
| `ETH_MDIO` / `ETH_MDC` entries | 62, 64, 86, 88 |

### LAN8742A — PHY datasheet
`LAN8742A-phy-datasheet.pdf` · Rev 1.1 · 132 pp ·
[download](https://ww1.microchip.com/downloads/en/DeviceDoc/8742a.pdf)

Management register map, the ID values in registers 2/3, strapping options,
MDIO timing.

### UM2408 — Nucleo-144 (MB1363) user manual
`UM2408-nucleo144-mb1363-user-manual.pdf` · Rev 6, February 2025 · 57 pp ·
[download](https://www.st.com/resource/en/user_manual/um2408-stm32h7-nucleo144-boards-mb1363-stmicroelectronics.pdf)

### MB1363 — Nucleo-H745ZI-Q schematic
`MB1363-nucleo-h745zi-q-schematic.pdf` · rev C-01 · 11 pp ·
[download](https://www.st.com/resource/en/schematic_pack/mb1363-h745ziq-c01_schematic.pdf)

The definitive answer on MDIO pull-up, PHY clock source and PHY reset. When
UM2408 is vague, this is not.

---

## Tier 2 — before the DMA milestone

### ES0445 — device errata
`ES0445-stm32h745-errata.pdf` · Rev 6, September 2025 · 57 pp ·
[download](https://www.st.com/resource/en/errata_sheet/es0445-stm32h745747xig-and-stm32h755757xi-device-limitations-stmicroelectronics.pdf)

**§2.25 ETH — PDF pp.42–47.** Read before debugging any DMA stall:

| Erratum | Subject |
|---|---|
| 2.25.1 | MAC starves a high-priority request after servicing a low-priority one |
| 2.25.2 | Rx DMA may fail to recover after a bus error (Rx timestamping on) |
| 2.25.3 | Tx DMA fails to recover / corrupts header on AHB bus error |
| 2.25.4 | Incorrectly weighted round-robin between Tx and Rx DMA |
| 2.25.7 | Bus error + start-of-packet can corrupt MAC-generated transmission |
| 2.25.10 | Packets dropped in RMII **10 Mbps** mode (fake dribble / CRC error) |
| 2.25.12 | Tx DMA may halt fetching a TSO header |

Also **§2.3.11** (p.16): MII mode is unavailable on packages with PC2_C/PC3_C
pins. Harmless here — this board is RMII — but worth knowing why MII was never
an option for you.

### PM0253 — Cortex-M7 programming manual
`PM0253-cortex-m7-programming-manual.pdf` · Rev 6, May 2026 · 254 pp ·
[download](https://www.st.com/resource/en/programming_manual/pm0253-stm32f7-series-and-stm32h7-series-cortexm7-processor-programming-manual-stmicroelectronics.pdf)

MPU registers, cache maintenance operations, barrier semantics.

### AN4838 — managing the MPU
`AN4838-mpu-management.pdf` · Rev 11, December 2025 · 24 pp ·
[download](https://www.st.com/resource/en/application_note/an4838-managing-memory-protection-unit-in-stm32-mcus-stmicroelectronics.pdf)

How to mark the SRAM3 descriptor region non-cacheable.

### AN4839 — L1 cache on STM32F7/H7
`AN4839-l1-cache-coherency.pdf` · Rev 2, March 2018 · 13 pp ·
[download](https://www.st.com/resource/en/application_note/an4839-level-1-cache-on-stm32f7-series-and-stm32h7-series-stmicroelectronics.pdf)

*The* document for the D-cache/DMA coherency problem. Short — read it whole
before writing descriptor code.

| What | PDF page |
|---|---|
| Cache maintenance ops via CMSIS | 4 |
| STM32F7/H7 default cache settings | 5 |
| **Worked example: cache maintenance and DMA data coherency** | 5–7 |
| **"Mistakes to avoid and tips"** | 8 |

Note it is Rev 2 (2018) and predates this errata sheet — where they disagree
about H7 behaviour, ES0445 wins.

---

## Tier 3 — orientation and second opinions

### ST training deck — STM32H7 Ethernet peripheral
`ST-training-stm32h7-ethernet.pdf` · 18 pp ·
[download](https://www.st.com/content/ccc/resource/training/technical/product_training/group0/00/2e/df/ff/be/ad/4e/79/STM32H7-Peripheral-Ethernet_ETH/files/STM32H7-Peripheral-Ethernet_ETH.pdf/_jcr_content/translations/en.STM32H7-Peripheral-Ethernet_ETH.pdf)

Block-diagram overview. Read this before the 200-page chapter, not instead of it.

### Not downloaded (deliberately)

| Source | Why |
|---|---|
| [IEEE 802.3 via IEEE GET](https://ieeexplore.ieee.org/browse/standards/get-program/page/series?id=68) | Needs a free IEEE account. RM0399 §61.6 already reproduces the clause-22 frame format |
| [Oryx `stm32h7xx_eth_driver.c`](https://www.oryx-embedded.com/doc/stm32h7xx__eth__driver_8c_source.html) | Independent bare-metal H7 driver — a second opinion *after* a milestone, not during |

---

## Already on this machine (no download needed)

| What | Path |
|---|---|
| CMSIS device header — all `ETH_*` bit positions and masks | `C:/ncs/v3.3.0/modules/hal/stm32/stm32cube/stm32h7xx/soc/stm32h745xx.h` |
| ST HAL Ethernet driver | `C:/ncs/v3.3.0/modules/hal/stm32/stm32cube/stm32h7xx/drivers/src/stm32h7xx_hal_eth.c` |
| Zephyr board DTS (verified pin map) | `C:/Users/ghkh1/mcu-eth-rx/build/zephyr/zephyr.dts` |

The CMSIS header gives bit *positions* for free. It does not give the CR field's
*semantics* — which encoding covers which HCLK range is RM0399 §61.11, p.3151
only.
