# Architecture — how the kernel actually works

This is the study companion to the source. Read it next to `src/kernel/`.

## 1. The one mental model

A context switch on Cortex-M is nothing more than: **enter an exception with
task A's registers, return from it with task B's**. The CPU already saves half
a task's state on exception entry; the kernel's only real job is saving the
other half and swapping which stack the return uses.

Per task, "state" = the CPU registers. They live in three places:

| Where | What | Who saves it |
|---|---|---|
| Hardware frame (on task's PSP) | r0–r3, r12, lr, pc, xPSR (+ s0–s15, FPSCR if FP active) | The CPU, automatically, at exception entry |
| Software frame (pushed by `port.s`) | r4–r11 + EXC_RETURN (+ s16–s31 if FP active) | `PendSV_Handler` |
| TCB | just the resulting stack pointer | `str r0, [r2]` — one word |

`k_task_t.sp` is the **first** member so `port.s` can use offset 0.

## 2. Why PendSV, and why lowest priority

Preemption decisions happen in many places (tick, `sem_give` from an ISR,
`k_yield`) but the *switch itself* must run when no other exception is active —
otherwise you'd be swapping thread state under a half-serviced interrupt.

PendSV solves this by being **pended, not called**: anyone sets
`ICSR.PENDSVSET`, and because PendSV is configured as the *lowest* exception
priority (SHPR3 = 0xF0), the hardware takes it only after every other
exception unwinds. The tail-chaining that results — ISR → PendSV → new task —
is the textbook Cortex-M pattern, and interrupt latency for high-priority ISRs
is never affected by a context switch in progress.

## 3. Walkthrough of one preemption

Button pressed while `fpu-a` (prio 4) runs; `btn` task (prio 1) is blocked on
`btn_sem`:

1. EXTI15_10 fires (priority 0xE0). CPU pushes fpu-a's hardware frame onto
   fpu-a's PSP. Because fpu-a used the FPU and lazy stacking is on, the CPU
   *reserves* space for s0–s15 but doesn't write it yet (that's the "lazy").
2. The ISR calls `k_sem_give(&btn_sem)`: pops `btn` off the waitlist,
   `k_ready_locked(btn)`, sees `btn->prio (1) < k_curr->prio (4)`, sets
   PENDSVSET. Returns.
3. Exception return begins, sees PendSV pending at lower priority than
   thread → tail-chains straight into `PendSV_Handler` (no unstack/restack).
4. `PendSV_Handler`: `mrs r0, psp` → fpu-a's frame. `tst lr, #0x10` — bit 4
   of EXC_RETURN is 0 (FP context active), so `vstmdbeq` pushes s16–s31 (this
   also forces the lazy s0–s15 stacking to materialize). `stmdb` pushes
   r4–r11 + EXC_RETURN. The final PSP lands in `k_curr->sp`.
5. Under `BASEPRI = 0xE0`, `k_switch_context()` runs: canary check on fpu-a,
   requeues it (still RUNNING → READY, back of queue 4), `__builtin_ctz`
   on the ready bitmap finds queue 1, pops `btn`, `k_curr = btn`.
6. Reverse of step 4 from `btn`'s stack: `ldmia {r4-r11, lr}` — btn blocked
   from plain C (no FP), so its EXC_RETURN has bit 4 set and the `vldmiaeq`
   is skipped. `msr psp`, `bx lr`.
7. The CPU unstacks btn's hardware frame from the new PSP: btn resumes
   *inside* `k_sem_take()`, exactly where it blocked, token in hand.

Total path from ISR give to task running: measured by T2 in the suite.

## 4. Launching tasks: fabricated frames

`k_task_create()` writes onto a fresh stack exactly what steps 4–7 expect to
find, as if the task had been switched out before it ever ran:

```
higher addresses
   xPSR = 0x01000000     Thumb bit — mandatory or the first return faults
   PC   = task entry
   LR   = task_exit_trap  catches 'return' from a task body
   r12, r3, r2, r1 = 0
   r0   = arg             ← this is how the argument arrives
   EXC_RETURN = 0xFFFFFFFD  thread mode, PSP, no FP frame yet
   r11..r4 = 0
lower addresses            ← task->sp points here
```

First launch cheats the same path: `k_start()` executes `svc 0`, and
`SVC_Handler` performs steps 6–7 only. The boot MSP is reset to `_estack`
because thread-mode-on-MSP is dead from that instant — from then on MSP
belongs exclusively to exceptions.

`0xFFFFFFFD` decodes as: return to **thread** mode, restore from **PSP**,
frame is **basic** (no FP). After the task's first FP instruction, the CPU
flips CONTROL.FPCA, and the *next* exception's EXC_RETURN shows bit 4 = 0.

## 5. The single kernel lock

Everything shared (ready queues, waitlists, tick bookkeeping) is protected by
one rule: **raise BASEPRI to 0xE0 before touching it**.

- Tasks: `k_crit_enter()/k_crit_exit()`.
- SysTick and kernel-aware ISRs already *run at* 0xE0, so they can't preempt
  a masked region nor each other — they touch the structures lock-free.
- PendSV masks 0xE0 around `k_switch_context()` because it runs *below* 0xE0.

Anything with NVIC priority numerically **below** 0xE0 (= logically higher)
is forbidden from calling kernel APIs — in exchange it is never masked, not
even for a cycle, preserving hard-real-time latency for non-kernel ISRs.

Why BASEPRI and not PRIMASK (`cpsid i`)? Same reason as FreeRTOS: PRIMASK is
all-or-nothing; BASEPRI masks *from a level down*, keeping the escape hatch.

## 6. Scheduler data structures

```
rq_bitmap: 0b10010010
              │  │  └─ prio 1 ready (btn)
              │  └──── prio 4 ready (fpu-a, fpu-b)
              └─────── prio 7 ready (idle)
__builtin_ctz(bitmap) → 1   (RBIT+CLZ: two instructions, O(1))
```

- One FIFO (head+tail) per priority. Push = tail, pop = head ⇒ round-robin
  within a level falls out for free when the tick expires the timeslice and
  PendSV requeues the running task at its tail.
- The **running task is never in a queue** — `k_switch_context()` requeues it
  only if still RUNNING (i.e. not blocked/sleeping/zombie). This asymmetry is
  what makes blocking trivial: `sem_take` just marks BLOCKED and pends.
- The idle task (prio 7, `wfi` loop) guarantees the bitmap is never empty at
  pick time. No special cases in the hot path.

## 7. Semaphores: direct handoff

`give()` with waiters does **not** increment the count and wake someone to
re-try; it transfers the token directly to the highest-priority waiter and
readies it. Consequences worth being able to defend:

- No thundering herd, no lost wakeups, no re-check loop in `take()`.
- Waitlist is priority-sorted at insert (O(waiters), tiny n) → `give()` pops
  the head in O(1), and priority inversion via FIFO wakeup can't happen.
- A waiter that outranks the giver preempts *inside* `give()` — by the time
  `give()` returns, the consumer may already have consumed. T4 hammers this
  20 000 times.

Deliberately absent: timeouts (a task would sit on two lists — doable, but
minimal means minimal) and priority inheritance (listed as a stretch).

## 8. Stack discipline

- Fill pattern `0xA5A5A5A5` at create → `k_stack_high_water()` scans for the
  first untouched word: peak usage in words, printed by the stats task.
- Canary `0xC0DE57AC` in the lowest word, checked on **every switch-out** of
  the outgoing task — an overflow is caught at the next context switch, with
  the task named in the panic. T7 proves the whole path by genuinely
  overflowing a 256-word stack into guard memory the test owns.
- AAPCS requires 8-byte-aligned frames: the stack top is rounded down at
  create, and the hardware re-aligns on exception entry via CCR.STKALIGN.

## 9. Faults are features

`HardFault_Handler` (port.s) picks MSP or PSP from EXC_RETURN bit 2 and hands
the stacked frame to `k_fault_dump()`: r0–r3, r12, lr, **pc**, xPSR plus
CFSR/HFSR/BFAR/MMFAR and the current task name. With `pc` and the `.map`/
`kernel.lst`, most crashes localize in seconds. Bus/Usage/MemManage stay
disabled in SHCSR so everything funnels through the one dump path.

## 10. H745 dual-core reality

The kernel owns the CM7. But the H745 boots **both** cores from flash
(BCM7/BCM4 option bits default to 1): the CM4 fetches its vector table from
0x08100000 — which on this board still contained the ST factory demo, happily
driving the same LEDs. `stub/cm4_stub.s` is a 4-instruction image (SP, PC,
`wfi` loop) flashed to bank 2 once, no option-byte writes needed (safer than
touching BCM4 — a wrong option write can brick the boot config).

## 11. Deliberate scope cuts (know these cold)

| Cut | Why | What adding it takes |
|---|---|---|
| 64 MHz HSI, no PLL | H7 clock raise = PWR VOS + SMPS/LDO config (-Q board is SMPS!) + flash latency; orthogonal to kernel design | `system.c` only |
| I/D caches off | determinism first; DTCM is 0-wait anyway | SCB cache ops + think about flash-side coherency |
| No sem timeouts | two-list membership complicates every wake path | wake_tick + removal from both lists |
| No priority inheritance | needs owner tracking + dynamic prio | mutex type with owner field, prio push/pop |
| No MPU stack guards | canary catches overflow at switch granularity | MPU region per task around stack base |
| Privileged tasks only | user mode needs SVC syscall layer | CONTROL.nPRIV + SVC dispatch table |
