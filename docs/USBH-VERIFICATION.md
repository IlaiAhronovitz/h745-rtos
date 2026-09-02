# usbh.c — what is proven, what is not (2026-08-24)

The honest status of the milestone-1 USB host driver. Rule inherited from
BENCH.md: a claim is proven when a captured transcript or register readout
shows it — never because the code "should" do it.

## The split, in one sentence

**The bring-up half is proven on silicon; the transfer half (EP0 engine,
enumeration) is written but has never executed, because no device has been
plugged into CN13 yet.** Anyone claiming "100% working" without the device
test would be lying; this document is the alternative.

## Proof chain — what IS proven, and by what evidence

| # | Claim | Evidence | Where |
|---|---|---|---|
| 1 | Builds clean under -Wall -Wextra -Werror | first-try compile, text=7160 | `make APP=usb BUILD=build/usb` |
| 2 | HSE bypass runs, PLL3 locks | RCC_CR=0x3007C025: HSEON+HSERDY+HSEBYP+PLL3ON+PLL3RDY all 1 | GDB audit (below) |
| 3 | PLL3 makes exactly 48.000 MHz | RCC_PLL3DIVR=0x00040095 (N=150, Q=/5 from 1.6 MHz ref); RCC_D2CCIP2R=0x00200000 (USBSEL=pll3_q) | GDB audit |
| 4 | VDD33USB detector up | PWR_CR3=0x05010044: USB33DEN=1, USB33RDY=1 | GDB audit |
| 5 | Core is alive and is the expected IP | CID=00002300, GSNPSID=4f54330a (Synopsys OTG 3.30a) | serial banner + HIL `core ID readout` PASS |
| 6 | USB2 has internal DMA (design precondition) | GHWCFG2=229fe190, bits[4:3]=2 | serial + HIL `internal DMA architecture` PASS |
| 7 | Core soft reset + PHY select completed | init would print a specific failure and halt otherwise; GUSBCFG=0x20002447 shows PHYSEL=1 | serial + GDB audit |
| 8 | Host mode entered | GINTSTS bit0 CMOD=1; GOTGCTL bit21 CURMOD=1; GUSBCFG FHMOD=1 | GDB audit |
| 9 | FIFOs carved as designed | GRXFSIZ=0x238 (568w), HNPTXFSIZ=0x00800238 (128w@568), HPTXFSIZ=0x010002B8 (256w@696) | GDB audit |
| 10 | H7-correct GCCFG (PHY on, no VBUS sensing) | GCCFG=0x00010000 exactly (PWRDWN only); GOTGCTL BVALOEN/BVALOVAL=0 | GDB audit |
| 11 | DMA + burst configured, interrupts deliberately off (polled M1) | GAHBCFG=0x2E: DMAEN=1, HBSTLEN=INCR16, GINT=0 | GDB audit |
| 12 | 48 MHz FS PHY clock selected | HCFG=0x201: FSLSPCS=01 | GDB audit |
| 13 | HFIR discipline honored (write only after PENA) | HFIR=0x0001EA60: RLDCTRL=1 (ours) + FRIVL still at reset 60000 — correct, since PENA has never happened | GDB audit |
| 14 | VBUS commanded on, port powered, no device present | HPRT=0x00001000: PPWR=1, PCSTS=0 | GDB audit + LD8 green LED |
| 15 | Debuggable live without reset | attached running, symbolic bt (wait_ms<-usbh_wait_connect<-main), hw breakpoint hit, detach clean | GDB session transcript |
| 16 | Bring-up is regression-checked from now on | `python tools/hil.py --stages usb` = 8 PASS | logs/hil/20260824-133051Z/ |

Footnotes, in the spirit of honesty: HCFG read back 0x201 — bit9 is not a
bit this driver writes (reserved/undocumented readback; FSLSPCS[1:0]=01 is
the claim and it matches). GOTGCTL upper status bits (0x03200000) include
read-only session status beyond CURMOD; only CURMOD and the two cleared
override bits are claimed. "VBUS on" is proven at the commanded level
(PPWR + PD10 low + LD8); the 5 V rail itself was not measured with a meter.

## Simulation tier (added later on 2026-08-24): tests/host/

The unproven half now has an intermediate proof tier: `tests/host/` compiles
the REAL `src/usbh.c` (unmodified) for x86 against a register-level
behavioral model of the OTG core + emulated devices, and drives every
function through its input space — **77 checks, 0 failures** (run:
`cd tests/host && make && ./test_m1` under Linux or WSL).

What the suite covers: init happy path (end state diffed against the REAL
bench register audit values), all 7 injectable init failures, connect/
absence/detach, FS enumeration against EC25-like descriptors (VID 2c7c,
5 vendor interfaces) with transaction-order + device-addressing assertions,
LS mouse (mps0=8, 6 MHz branch, HFIR=5999), config-larger-than-buffer
truncation, control-transfer length boundaries (0/1/MPS/MPS+1/full/over),
OUT data stages, persistent-fault propagation (STALL→-3, NAK→-5,
TXERR/DTERR/BBERR→-4), single-shot-fault retry recovery, disconnect abort
with no retry storm, sub-header garbage rejection, dead-port timeout, and a
global invariant that no HPRT write ever carried a w1c bit.

Honest boundary of this tier: simulation proves the DRIVER'S LOGIC against
a model of the core; it cannot prove the model matches real silicon timing.
The bench (a real device on CN13) remains the final word. Note also that
the harness itself was debugged by disagreement: its first run failed 10
checks because the model lacked rc_w1 semantics on HCINT — plain storage
handed the driver a fake XFRC — which is itself evidence the checks bite.

## What is NOT proven ON HARDWARE (the entire list — all now sim-covered)

1. **Connect detection** (PCSTS/PCDET path) — no device ever attached.
2. **Port reset & enable sequence** (PRST timing, PENA, speed read).
3. **The whole EP0 engine**: SETUP packets, IN/OUT data stages, status
   stages, DATA1 toggling under buffer DMA, the XFRC/CHH classification.
4. **Enumeration**: GetDescriptor(8) -> mps update -> SetAddress ->
   full device + config descriptors.
5. **The DMA data path itself** (bounce buffer in .usb_ram, dsb ordering).
6. **Error paths**: NAK-giveup, STALL, TXERR, retry loop, disconnect
   mid-transfer, LS device support (FSLSPCS=6 MHz branch).
7. **Overcurrent handling** (PG7 is read by nobody yet).

## How each becomes proven

- Items 1–5: **plug any USB device into CN13** (Micro-B OTG adapter +
  flash stick / mouse). One successful descriptor dump on the VCP proves the
  entire happy path end-to-end; the HIL `device enumeration` check flips
  from INFO to PASS automatically and records VID:PID.
- Item 6 partially: unplug mid-enumeration (DISC path), plug a low-speed
  mouse (LS branch), plug a device behind its own cable extender for
  marginal-signal TXERR exercise. Deliberate STALL: request descriptor
  type 0xEE from a device that lacks it.
- Item 7: short VBUS through a 10 Ω load only if you enjoy risk — or just
  watch PG7 with the button-style EXTI in a later milestone.

## Reproduce every proof yourself

```
# machine-checked bring-up gates (build + flash + parse):
python tools/hil.py --stages usb

# live register audit (attach without reset, board keeps running):
#   VS Code: "Attach APP=usb (live, no reset)" -> Debug Console:
#   x/1wx 0x40080440   (HPRT)   x/1wx 0x40080014   (GINTSTS)
# or the same via command-line gdb against openocd.

# watch the serial evidence:
python tools/monitor.py --port COMxx
```
