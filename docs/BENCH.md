# Bench runbook — hardware validation session

Preconditions: Nucleo-H745ZI-Q on USB (CN1), Windows enumerated it.
Two ST-Links may exist on this PC — the Makefile pins serial
`002300373331511434333834` (the Nucleo's on-board ST-Link V3).

All `make` invocations need msys tools first:
`$env:PATH = "C:\msys64\usr\bin;$env:PATH"`

## 0. Find the VCP port

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
```

Historically COM44 on this PC. If different, pass `--port COMxx` below.

## 1. One-time: park the CM4

```powershell
make flash-stub
```

Expect OpenOCD `** Verified OK **`. Without this, the factory CM4 demo in
flash bank 2 keeps driving the LEDs underneath the kernel.

## 2. Demo smoke test

Start the monitor FIRST (background), then flash:

```powershell
python tools\monitor.py --port COM44 --seconds 25 --log logs\demo-boot.txt   # background
make flash
```

Expected within a few seconds of reset:

- Banner `=== h745-rtos ===`, then green LED 2 Hz, yellow 1 Hz.
- Every 5 s a stats block: 7 tasks (idle/btn/green/yello/fpu-a/fpu-b/stats),
  stack peaks well under 512, `fpu: ... 0 fail`.
- Press B1 (blue): red LED toggles + `[btn] press N: ISR->task wake in X
  cycles = Y us` (expect a few hundred cycles / handful of µs).

## 3. Validation suite

```powershell
make APP=test BUILD=build-test flash
python tools\monitor.py --port COM44 --seconds 30 --log logs\validation.txt
```

(Or monitor first, then flash — the suite runs in ~8 s from reset.)

PASS criteria printed on-target; the run MUST end with:

```
*** KERNEL PANIC: stack overflow  task=victm ... ***
```

— that's T7 succeeding, not a failure.

## 4. Afterwards

1. Copy T1/T2/T3/T5 numbers from `logs/validation.txt` into the README
   validation table (replace the *pending bench run* placeholders).
2. Reflash the demo (`make flash`) and leave it running as a soak test;
   re-check a stats block after a few hours (`fpu ... 0 fail`, no panic).
3. Commit logs + README update.

## Troubleshooting

- OpenOCD can't find the probe → check `STLINK_SERIAL` matches
  (`openocd ... -c "adapter list"`, or unplug the other ST-Link).
- Garbage on the VCP → baud is 115200; the H7 runs on raw HSI, no PLL — if
  text is *consistently* garbled, HSI trim is off (unlikely; HSI is ±1 %).
- No LEDs at all → did `make flash` actually program bank 1? `program`
  verifies; check for `** Verified OK **`.
- LEDs blink a pattern the demo doesn't explain → CM4 stub step skipped.
