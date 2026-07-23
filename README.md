# h745-rtos — minimal preemptive RTOS kernel for STM32H745 (Cortex-M7)

A from-scratch RTOS kernel in ~420 lines of kernel code (~1.2 kLOC total with
platform + demo apps). No HAL, no CMSIS headers, no libc — the vector table,
linker script, register definitions, context switch and scheduler are all
written by hand, with every register cited to RM0399 / PM0253.

## Features

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

## Hardware

| | |
|---|---|
| Board | ST Nucleo-H745ZI-Q (STM32H745ZI, dual-core CM7 + CM4) |
| Kernel core | Cortex-M7 @ 64 MHz (reset HSI — the H7 PWR/PLL/SMPS dance is deliberately out of scope) |
| CM4 core | Parked by a 4-instruction `wfi` image in flash bank 2 (`stub/`) — the chip boots both cores |
| Console | USART3 PD8/PD9 → on-board ST-Link VCP, 115200 8N1 |
| LEDs / button | LD1 PB0, LD2 PE1, LD3 PB14, B1 PC13 → EXTI15_10 |
| RAM layout | All data + every stack in DTCM (128 KiB, zero-wait-state, core-private); MSP at DTCM top |

## Repository layout

```
src/kernel/   kernel.c/.h  scheduler, tasks, tick, panic
              port.s       PendSV/SVC context switch, HardFault shim
              sem.c/.h     counting semaphores
              fault.c      HardFault register dump
src/          startup.s    vector table + reset (data/bss init, FPU on)
              stm32h745.h  hand-written register map (only what's used)
              system.c uart.c board.c kprintf.c libmin.c
app/          app_demo.c   6-task demo (default APP=demo)
              app_test.c   on-target validation suite (APP=test)
linker/       stm32h745_cm7.ld, cm4_stub.ld
stub/         cm4_stub.s   CM4 parking image
tools/        monitor.py   raw serial capture
docs/         ARCHITECTURE.md (context-switch walkthrough), BENCH.md
```

## Build & flash

Requires: `arm-none-eabi-gcc`, GNU make, OpenOCD, an ST-Link. Paths at the top
of the `Makefile` are machine-specific — adjust `GCC_BIN`, `OCD_ROOT` and
`STLINK_SERIAL` (serial-pinning matters when more than one ST-Link is
attached).

```
make                # demo app  -> build/kernel.elf
make APP=test BUILD=build-test   # validation suite
make flash-stub     # one-time: park the CM4 (flash bank 2)
make flash          # program + verify + reset via OpenOCD
python tools/monitor.py --port COM44 --log logs/run.txt
```

## Demo application

Six tasks + one ISR exercise every feature at once: two heartbeat LEDs from
sleeping tasks, a button ISR that `k_sem_give()`s a priority-1 task (the
ISR→task wake latency prints in µs on every press), two busy FPU tasks that
continuously prove `S16–S31` survive preemption, and a stats task dumping the
task table every 5 s.

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

- 64 MHz reset clocks, caches off — deterministic bring-up; the 480 MHz
  PWR/VOS/flash-latency sequence is a documented stretch goal.
- No semaphore timeouts, no mutex/priority-inheritance, no MPU guards, no
  tickless idle — each is a clean extension point, listed in
  `docs/ARCHITECTURE.md`.
- Tasks run privileged; no user/kernel isolation.

## References

- ST RM0399 — STM32H745/755 reference manual (peripherals, dual-core boot)
- ARM PM0253 — Cortex-M7 programming manual (exception model, lazy FP stacking)
- ST UM2408 — Nucleo-H745ZI-Q board (pin mapping)
