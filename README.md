# h745-rtos — preemptive RTOS kernel, Ethernet driver and USB-host LTE modem stack for STM32H745 (Cortex-M7)

A from-scratch RTOS kernel (~480 lines) and a from-scratch Ethernet driver
(~830 lines), ~2.6 kLOC in total with platform and demo apps. No HAL, no CMSIS
headers, no libc, no vendor network stack — the vector table, linker script,
register definitions, context switch, scheduler, MAC driver and enough of ARP
and ICMP to answer a ping are all written by hand, with every register cited to
RM0399 / PM0253.

## Kernel features

- **Preemptive, priority-based scheduling** — 8 fixed levels (0 = highest,
  7 = idle), O(1) highest-ready pick via per-priority FIFO queues + ready
  bitmap + `CTZ`, timesliced round-robin within a level (10 ms quantum).
- **PendSV context switching** — configured as the lowest exception priority so
  a switch only ever interrupts pure thread state. Software frame
  `{r4–r11, EXC_RETURN}` on the task's PSP; exceptions run on MSP.
- **FPU-aware** — lazy stacking left enabled; PendSV checks `EXC_RETURN[4]`
  and saves/restores `S16–S31` only for tasks with an active FP context.
- **Task stacks, guarded** — caller-allocated (static allocation only, no heap
  anywhere). Every stack is fill-patterned for high-water reporting and
  canary-checked on every switch-out; overflow ⇒ named kernel panic.
- **Counting semaphores** — priority-ordered waitlists; `give()` with waiters
  hands the token *directly* to the highest-priority waiter (no thundering
  herd, no re-check loop) and preempts if it outranks the caller. ISR-safe.
- **One kernel lock** — `BASEPRI = 0xE0` masks SysTick + PendSV + any
  kernel-aware ISR; higher-priority interrupts stay hard-real-time and are
  never blocked by the kernel.
- **Sleep & tick** — 1 kHz SysTick, `k_sleep_ms()` via wake-tick scan,
  wrap-safe compare.
- **Diagnostics** — HardFault register dump (stacked frame + CFSR/HFSR/BFAR)
  over UART, per-task switch counts and stack high-water stats.

## Ethernet driver

A bare-metal driver for the STM32H745's Synopsys DesignWare EQOS MAC, driving
the board's Microchip LAN8742A PHY over RMII. Verified against a PC over a
direct cable: receives live traffic, transmits, and answers `ping`.

- **MDIO management** — IEEE 802.3 clause-22 reads with a bounded busy poll.
  MDC is HCLK/42 = 1.52 MHz at 64 MHz, chosen mid-range rather than against
  either edge of the 1.0–2.5 MHz window.
- **RMII bring-up** — nine pins to AF11 plus the `SYSCFG_PMCR` mode select,
  which only latches while the MAC clock is still off.
- **Link handling** — polls PHY basic status, then reads the negotiated speed
  and duplex from the LAN8742A's vendor register 31 before starting the MAC.
- **DMA-safe memory** — descriptor rings and packet buffers placed in D2 SRAM3
  via a dedicated `.eth_ram` section, with an MPU region marking it Normal,
  shareable and **non-cacheable**. The ETH DMA cannot reach DTCM at all, and
  the D-cache would otherwise hand it stale descriptors.
- **RX and TX** — four-entry descriptor rings, `OWN`-bit handshake, tail-pointer
  doorbell after every re-arm, with a `DSB` before the doorbell because
  descriptors are Normal memory and the tail pointer is a Device register.
- **ARP and ICMP echo** — enough to be pingable, including the RFC 1071
  internet checksum (validated against the RFC's own worked example).

Four failures found during bring-up, none of which announced themselves:

| Symptom | Cause |
|---|---|
| Nothing at all; DMA reset never completed | One GPIO clock constant typed twice, so `PA1` stayed analog and no RMII reference clock reached the MAC. Every register written afterwards landed in a peripheral held in reset. |
| Exactly four frames, then silence | Re-arming a descriptor is not enough once the DMA has suspended on buffer-unavailable; the tail pointer must be rewritten to wake it. |
| Ping never answered | `MACCR.CST` unset, so frames still carried their 4-byte FCS and the reported length was 4 too long, corrupting the ICMP checksum. |
| Ping answered, but ~1 s late | Missing `DSB` before the TX doorbell: the MAC could see the doorbell before `OWN` and go back to sleep. |

## Cellular modem over a from-scratch USB host

The MCU drives a Quectel RM500Q-GL 5G module (`2c7c:0800`, used here on LTE)
through its own USB port and reaches the public internet with no PC and no
vendor driver in the loop:

> RTOS → bare-metal DWC2 USB host (`usbh.c`) → AT engine (`atc.c`, `modem.c`)
> → QMI control over EP0 (`qmi.c`) → raw-IP data pipe (`rmnet.c`) → IPv4/ICMP/UDP/DNS (`ipnet.c`) → LTE

- **USB host** on `OTG_FS` (`0x40080000`), buffer DMA, polled: enumeration,
  descriptor parsing, bulk pipes with toggle tracking, STALL recovery
  (`CLEAR_FEATURE(ENDPOINT_HALT)`), multi-packet transfers. Full speed only:
  the Nucleo has no ULPI PHY, so 12 Mbit/s is the bus ceiling.
- **AT engine** — command/response/URC classifier with resync, echo handling
  and `+CME` decoding; finds the AT port by class triple plus a live `AT`.
- **QMI over EP0** — CTL/WDA/WDS: client-id allocate/release, raw-IP data
  format, start/stop network, current settings (address, mask, gateway, DNS,
  MTU). The APN is read from the module's own context 1 or passed at build
  time; nothing operator-specific is compiled in.
- **Clocking** — `clock_init()` takes the core from 64 MHz HSI to 480 MHz
  (HSE → PLL1, VOS0, SMPS supply config), with a safe 200 MHz fallback, and
  clock rates are runtime values so the console stays correct either way.
- **Host simulation** — `tests/host` compiles the real `usbh.c`, `atc.c`,
  `modem.c` and `qmi.c` for x86 against a register-level OTG model with
  emulated modems: **255 checks** (`make && ./test_m1 … ./test_m4`).

### Measured on hardware (Nucleo-H745ZI-Q + RM500Q-GL on a commercial LTE network)

| What | Result |
|---|---|
| Public internet from the MCU | `ping 8.8.8.8` reply; `example.com` resolved over LTE |
| Echo reliability | 18/20 replies, RTT min 39 / avg 52 / max 61 ms |
| Packet sizes | 8, 64, 256, 512, 1024, 1400 B payloads, each reply verified byte for byte |
| Uplink burst, 1400 B, core at 64 / 200 / 480 MHz | **368 / 676 / 842 kB/s** (3870 / 2110 / 1695 µs per packet) |
| Downlink, 1400 B, 32 echoes in flight | **993 kB/s** (a floor: the network sets the reply rate) |
| Share of the USB FS bulk ceiling (~1216 kB/s) | uplink 69 %, downlink 82 % |
| RTT at 1400 B after one-transaction frame reads | 163 → 45 ms |
| Kernel self-test at 480 MHz | 6/6 PASS; round robin 7.05 M iterations/s (~1.2 M at 64 MHz) |

Write-ups: `docs/QMI-DATAPLANE.md` (transport, TLVs, the bugs found and how a
provisioning fault on one SIM was told apart from a driver fault),
`docs/THROUGHPUT.md` (ceiling arithmetic and the optimisation plan),
`docs/USBH-VERIFICATION.md` (register-level proof chain for the host).

```
make APP=modem BUILD=build/modem                          # APN from the SIM
make APP=modem BUILD=build/modem CFLAGS_EXTRA='-DDATA_APN=\"<apn>\"'
```

## Hardware

| | |
|---|---|
| Board | ST Nucleo-H745ZI-Q (STM32H745ZI, dual-core CM7 + CM4) |
| Kernel core | Cortex-M7 @ 480 MHz (HSE → PLL1, VOS0; falls back to 200 MHz, then 64 MHz HSI) |
| USB host | `OTG_FS` on CN13 via a micro-B OTG adapter; modem powered from its own carrier supply |
| CM4 core | Parked by a 4-instruction `wfi` image in flash bank 2 (`stub/`) — the chip boots both cores |
| Ethernet | Microchip LAN8742A PHY over RMII, MDIO address 0 |
| Console | USART3 PD8/PD9 → on-board ST-Link VCP, 115200 8N1 |
| LEDs / button | LD1 PB0, LD2 PE1, LD3 PB14, B1 PC13 → EXTI15_10 |
| RAM layout | All data + every stack in DTCM (128 KiB, zero-wait-state, core-private); MSP at DTCM top |

## Repository layout

```
src/kernel/   kernel.c/.h  scheduler, tasks, tick, panic
              port.s       PendSV/SVC context switch, HardFault shim
              sem.c/.h     counting semaphores
              fault.c      HardFault register dump
src/          eth.c/.h     Ethernet MAC + DMA driver
              net.c/.h     ARP and ICMP echo
              usbh.c/.h    USB host (DWC2 OTG_FS), usbdump.c descriptor dump
              atc.c/.h     AT command engine; modem.c/.h modem policy layer
              qmi.c/.h     QMI over EP0; rmnet.c/.h raw-IP data pipe
              ipnet.c/.h   IPv4/ICMP/UDP/DNS client for the data pipe
              startup.s    vector table + reset (data/bss init, FPU on)
              stm32h745.h  hand-written register map (only what's used)
              system.c uart.c board.c kprintf.c libmin.c/.h
app/          app_demo.c   6-task demo (default APP=demo)
              app_test.c   on-target validation suite (APP=test)
              app_eth.c    Ethernet demo (APP=eth)
              app_usb.c    USB host bring-up + enumeration (APP=usb)
              app_modem.c  modem bring-up, data call, proof run, benchmarks (APP=modem)
              app_bench.c  context-switch / semaphore-wake benchmark (APP=bench)
linker/       stm32h745_cm7.ld, cm4_stub.ld
stub/         cm4_stub.s   CM4 parking image
tests/host/   x86 simulation of the USB host, AT engine and QMI (255 checks)
tools/        monitor.py capture.py at.py  serial tools; hil.py  bench harness
docs/         ARCHITECTURE.md (context-switch walkthrough), BENCH.md,
              QMI-DATAPLANE.md, THROUGHPUT.md, USBH-VERIFICATION.md
              refs/INDEX.md  every vendor document used, with page numbers
```

## Build & flash

Requires: `arm-none-eabi-gcc`, GNU make, OpenOCD, an ST-Link. Paths at the top
of the `Makefile` are machine-specific — adjust `GCC_BIN`, `OCD_ROOT` and
`STLINK_SERIAL` (serial-pinning matters when more than one ST-Link is
attached).

```
make                # demo app  -> build/kernel.elf
make APP=test BUILD=build-test   # validation suite
make APP=eth        # Ethernet demo
make flash-stub     # one-time: park the CM4 (flash bank 2)
make flash          # program + verify + reset via OpenOCD
python tools/monitor.py --port COMxx --log logs/run.txt
```

## Demo applications

**`APP=demo`** — six tasks and one ISR exercise every kernel feature at once:
two heartbeat LEDs from sleeping tasks, a button ISR that `k_sem_give()`s a
priority-1 task (the ISR-to-task wake latency prints in microseconds on every
press), two busy FPU tasks that continuously prove `S16-S31` survive
preemption, and a stats task dumping the task table every 5 s.

**`APP=eth`** — brings the link up, reports the negotiated speed and duplex,
prints a one-line summary of each received frame (destination MAC, source MAC,
EtherType), answers ARP and ICMP for `192.168.137.50`, and transmits one
broadcast frame per second as a liveness check.

```
=== h745-rtos: bare-metal ethernet ===
ID1=0007 ID2=c131 BSR=7809
PASS: LAN8742A responding (rev 1)
Link up
negotiated 100 Mbps, full duplex -- starting MAC
ff:ff:ff:ff:ff:ff <- aa:bb:cc:dd:ee:ff  type=0806 ARP   64 B
33:33:00:00:00:02 <- aa:bb:cc:dd:ee:ff  type=86dd IPv6  66 B
tx 6 sent (last rc=0) | rx 98 frames, 11887 bytes
```

## Validation

`APP=test` runs a 7-test suite on the hardware and prints PASS/FAIL with
measured numbers; the run deliberately ends in a `stack overflow` panic to
prove the canary path end-to-end.

| # | Test | Result |
|---|------|--------|
| T1 | Context-switch cost (yield round trip, 1000×) | *pending bench run* |
| T2 | Semaphore give → preempt → wake latency (1000×) | *pending bench run* |
| T3 | Round-robin fairness, 2 busy tasks / 1 s | *pending bench run* |
| T4 | 20 000-token producer→consumer handoff churn | *pending bench run* |
| T5 | `k_sleep_ms(50)` accuracy, 20× | *pending bench run* |
| T6 | FPU S16–S31 integrity across preemption | *pending bench run* |
| T7 | Stack-overflow canary ⇒ deliberate panic | *pending bench run* |

*(Table filled with measured values from `logs/validation.txt` after the bench
session — see `docs/BENCH.md`.)*

## Design constraints & non-goals

Kept out on purpose, to stay minimal and reviewable:

- Caches off — deterministic bring-up. (The clock is now raised to 480 MHz
  at boot; see the cellular section.)
- No semaphore timeouts, no mutex/priority-inheritance, no per-task MPU
  guards, no tickless idle — each is a clean extension point, listed in
  `docs/ARCHITECTURE.md`. (The MPU *is* used, for one region: marking the
  Ethernet DMA memory non-cacheable.)
- The Ethernet driver polls rather than using its interrupt, and runs from a
  bare `main()` rather than as a kernel task. Both are deliberate next steps.
- Tasks run privileged; no user/kernel isolation.

## References

- ST RM0399 — STM32H745/755 reference manual (peripherals, dual-core boot)
- ARM PM0253 — Cortex-M7 programming manual (exception model, lazy FP stacking)
- ST UM2408 — Nucleo-H745ZI-Q board (pin mapping, Ethernet wiring)
- ST DS12923 — STM32H745ZI datasheet (alternate-function map)
- Microchip DS00002165 — LAN8742A PHY
- IETF RFC 826 / 791 / 792 / 1071 — ARP, IPv4, ICMP, internet checksum

Every document above, with the exact page number for each fact used, is
listed in `docs/refs/INDEX.md`. The PDFs themselves are not redistributed.
