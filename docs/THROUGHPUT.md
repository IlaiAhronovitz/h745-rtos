# USB Full-Speed data path — throughput optimisation plan

Scope: the RM500Q-GL QMI bulk pair (EP `0x8e` IN / `0x0f` OUT, wMaxPacketSize 64)
driven by `src/usbh.c` in polled buffer-DMA mode, through `src/rmnet.c` and
`src/ipnet.c`.

> **Status.** This is the plan as written at 200 MHz, before the clock work.
> The measured outcome (368 → 676 → 842 kB/s uplink at 64/200/480 MHz, 993
> kB/s downlink) is in the README. Section 4's call that 480 MHz was not worth
> it turned out wrong: it bought another 1.25×. The bench logs cited below
> (`logs/`) are not part of the repository.

---

## 0. Read this first

### 0.1 High Speed is not available on this board. It is not a lever.

The driver targets `OTG_BASE = 0x40080000` (`src/usbh.c:27`), which on the
STM32H745 is **USB2_OTG_FS**. Three independent reasons HS is off the table:

1. `src/usbh.c:265` sets `GUSBCFG.PHYSEL` — the internal full-speed serial
   transceiver. USB2_OTG_FS exposes no ULPI signals on any package pin.
2. The other core, USB1_OTG_HS at `0x40040000`, *does* have ULPI alternate
   functions, but ULPI needs an external HS PHY (USB3300 class, 12 signals plus a
   60 MHz clock). The Nucleo-H745ZI-Q fits none.
3. The board's USB micro-AB is wired to the FS core's D+/D-.

Reaching HS (480 Mbit/s, MPS 512) means a carrier board or a ULPI daughterboard
plus a driver port to the HS core. **Everything below assumes 12 Mbit/s and a
64-byte MPS, permanently.**

### 0.2 Two facts in the task brief that the tree contradicts

| Brief says | Tree says | Where |
|---|---|---|
| Cortex-M7 at 480 MHz | **200 MHz** (`SYS_CLK_HZ_TARGET`), AHB/AXI 100 MHz, APB 50 MHz, VOS3 | `src/stm32h745.h:25`, `src/system.c:57-86` |
| `usbh_bulk_frame()` clamps `want` to `p->mps`, so 1428 B costs 23 transactions | Already fixed. `want` is clamped to `USBH_BULK_BUF` and floored to a whole multiple of MPS — a 1428 B frame is **one** transaction | `src/usbh.c:801-805` |

The 62/62/72/88/106/163 ms table in the brief (fact 6) therefore measures code
that no longer exists, *and* was taken with the debug hex dump running. It cannot
justify any change. See §3.

---

## 1. THE CEILING

### 1.1 One 64-byte bulk transaction on Full Speed

Bit time at 12 Mbit/s = 83.333 ns.

| Phase | Bits |
|---|---|
| Token (SYNC 8 + PID 8 + ADDR/ENDP/CRC5 16 + EOP 3) | 35 |
| Host→device inter-packet gap | ~8 |
| DATA0/1 (SYNC 8 + PID 8 + 512 payload + CRC16 16 + EOP 3) | 547 |
| Bus turnaround | ~8 |
| ACK (SYNC 8 + PID 8 + EOP 3) | 19 |
| Inter-transaction gap | ~8 |
| **Total** | **~625** |

625 × 83.333 ns = **52.08 µs**.

Cross-check against the USB 2.0 spec's own budget (§5.11.3, Table 5-10, FS bulk):

```
t = 9107 + 83.54 × floor(3.167 + BitStuffTime(64 B)) + Host_Delay   [ns]
  = 9107 + 83.54 × floor(3.167 + 512)
  = 9107 + 83.54 × 515
  = 52,130 ns = 52.13 µs
```

The two agree. **Use 52.13 µs per 64-byte bulk transaction.**

### 1.2 The ceiling

```
transactions per 1 ms frame = floor(1000 / 52.13) = 19     (spec Table 5-10 agrees)

ceiling = 19 × 64 B / 1 ms = 1216 B/ms
                           = 1,216,000 B/s
                           = 1216 kB/s = 1.216 MB/s = 9.73 Mbit/s
                           = 81.1 % of the 12 Mbit/s line rate
```

Derived constants used throughout:

```
wire cost per byte     = 52.13 µs / 64 B = 0.8146 µs/B
1428 B frame (23 pkts) = 23 × 52.13 µs   = 1199 µs  ->  1191 kB/s
1500 B MTU   (24 pkts) = 24 × 52.13 µs   = 1251 µs  ->  1199 kB/s
```

**USB is half-duplex on the wire.** IN and OUT share the same 1216 kB/s. A
symmetric echo workload tops out near **608 kB/s each way**.

A real host loses the last slot of most frames (it will not start a transaction
that cannot finish before EOF) and eats occasional device NAKs. Linux on FS bulk
lands at 1.0–1.15 MB/s. Take **1.10 MB/s** as the *practical* one-way ceiling.

### 1.3 What the current code achieves

Only one throughput run exists in the tree (`logs/modem/bench.txt:471-478`,
`bench_datapath()`, `rmn.debug` forced to 0). **n = 1.** Treat every figure below
as a single sample.

| Path | Rate | % of 1216 kB/s | Source |
|---|---|---|---|
| OUT burst, 1400 B payload, debug **off** | **368 kB/s** | **30.3 %** | bench.txt:478 |
| OUT burst, 1024 B, debug off | 369 kB/s | 30.3 % | bench.txt:477 |
| OUT burst, 64 B, debug off | 340 kB/s | 28.0 % | bench.txt:474 |
| RX, 1428 B frames, debug **on** (as shipped) | **70.4 kB/s** | **5.8 %** | derived, §2 R1 |
| RX, 92 B frames, debug on (as shipped) | 5.6 kB/s | 0.46 % | derived, §2 R1 |
| Echo both ways, debug on | ~35 kB/s each way | 2.9 % | derived |
| **RX sustained, debug off** | **never measured** | — | — |

### 1.4 The one model that explains the OUT data

Fit `t_datagram = bytes / R + C` to bench.txt:474-478 (bytes = payload + 28):

| Payload | Bytes | USB pkts | Measured µs | Model µs | Error |
|---|---|---|---|---|---|
| 64 | 92 | 2 | 270 | 270 | 0.0 % |
| 256 | 284 | 5 | 780 | 787 | +0.9 % |
| 512 | 540 | 9 | 1480 | 1477 | −0.2 % |
| 1024 | 1052 | 17 | 2845 | 2857 | +0.4 % |
| 1400 | 1428 | 23 | 3870 | 3870 | 0.0 % |

```
R = 371 kB/s   (2.695 µs per byte)
C =  22 µs     (fixed cost per transfer)
```

Three conclusions fall straight out, and they set the whole priority order.

**1. The limiter is a byte rate, not a transaction rate.** A per-transaction model
needs 171 µs/packet with a *negative* intercept to fit; the byte-rate model fits
to under 1 % with a physically sensible +22 µs intercept. A 2-packet transfer
completing in 270 µs rules out any one-transfer-per-USB-frame quantisation.

**2. Host software is bounded at 22 µs per transfer** plus whatever scales with
bytes. The entire software chain per 1428 B datagram — 5664 bytes of
byte-at-a-time `memcpy` (`src/libmin.c:12-18`) plus a 710-iteration `csum16` —
costs roughly 141 µs at 200 MHz, i.e. **3.6 %** of 3870 µs. *Optimising host code
cannot buy more than about 4 % today.* (That 141 µs is an estimate at ~4
cycles/byte, not a measurement — §3.1 item 3 says how to measure it.)

**3. 69 % of every transfer is bus idle or NAK retry.** `1428 B × 0.8146 µs/B =
1199 µs` of wire against 3870 µs measured; the missing **2671 µs** is the modem
refusing data. 371 kB/s is 2.97 Mbit/s — an entirely ordinary LTE uplink grant.
The remaining **3.3×** is probably the bearer, not ours; §3.2 gives the
eight-line experiment that settles it.

---

## 2. RANKED CHANGES

Ordered by gain per unit of work. Every entry names the file, the function, the
edit, a number, and the specific test that proves framing and the data toggle
survived.

---

### R1 — Stop the debug hex dump
**1 line. 5.2× on RX. Do this first.**

**Where:** `app/app_modem.c:576`; `dump()` at `src/rmnet.c:16-25`, fired at `:86`
and `:97`.

**Mechanism.** `dump()` emits, per frame:

```
header  "rmnet: %s [%u]"           = 12 + digits(n) chars
indent  "\n" + 7 spaces, per 16 B  =  9 chars  (kprintf.c:27 expands \n -> \r\n)
hex     "%02x " per byte, show = min(n,48)     =  3 × show
tail    "\n"                                   =  2 chars
```

`uart_putc()` (`src/uart.c:24`) spins on `USART_ISR_TXE` — no FIFO, no DMA, no
interrupt. At 115200 8N1 that is 86.806 µs per character, hard-blocking.

| Frame | chars | ms per dump | ms per echo (tx+rx) |
|---|---|---|---|
| 36 B (8 B ping) | 151 | 13.11 | **26.22** |
| 92 B | 187 | 16.23 | 32.47 |
| 284 / 540 B | 188 | 16.32 | 32.64 |
| 1052 / 1428 B | 189 | 16.41 | **32.81** |

Because `show = min(n,48)`, this is a **flat ~32.8 ms tax on every echo ≥ 64 B**
— it adds only 6.6 ms of *slope* across the whole 8 B → 1400 B sweep.

**Cost as a throughput cap** (dump 16.41 ms + the measured USB 3.87 ms):

```
RX   @1428 B : 1/(20.28 ms) = 49.3 frames/s × 1428 B =  70.4 kB/s   (5.8 % of ceiling)
RX   @  92 B : 1/(16.50 ms) = 60.6 frames/s ×   92 B =   5.6 kB/s   (0.46 %)
echo @1428 B : 1/(40.55 ms) = 24.7 frames/s × 1428 B =  35.2 kB/s each way
```

**Already validated in-tree.** Same board, same run, `logs/modem/bench.txt`:
section [3] (dump on) versus section [5] round trip (dump off), at
256/512/1024/1400 B:

```
dump ON  : 75, 84, 72, 75  ms   mean 76.50
dump OFF : 46, 38, 54, 45  ms   mean 45.75
observed difference 30.75 ms    vs   predicted 32.6 ms
```

**Edit.**

```c
/* app/app_modem.c:576 — before */
    rmn.debug  = 1;

/* after */
    rmn.debug  = 0;   /* dump() = 16.4 ms of blocking 115200 UART per frame,
                       * both directions. It caps RX at ~49 frames/s (70 kB/s
                       * at 1428 B, 5.6 kB/s at 92 B) and was the only thing
                       * the [2]/[3] proof_run numbers ever measured. Turn it
                       * on by hand when you need to look at framing. */
```

One companion edit, so silencing the dump does not silence a real fault:

```c
/* src/rmnet.c:88 — before */
    if (r->debug && w != (int)n)
        kprintf("rmnet: tx SHORT (%d of %u)\n", w, n);

/* after — a short write is a fault, not a debug detail */
    if (w != (int)n)
        kprintf("rmnet: tx SHORT (%d of %u)\n", w, n);
```

**Expected gain.** RX one-way 70.4 → 368 kB/s = **5.2×**. Echo path 35.2 →
184 kB/s each way = **5.2×**. At 64 B frames, 5.6 → 340 kB/s = **61×**. The 8 B
RTT floor drops 62 → ~36 ms; the whole `proof_run` [2]/[3] table needs
remeasuring.

**Do not expect one re-run to prove it.** Twenty identical 8 B pings in one [2]
block spread 39–69 ms (bench.txt:381) — wider than the 26 ms effect. Compare the
**20-sample [2] mean** with `debug 0` versus `debug 1`.

**Tests that prove framing and toggle survived.**
- `proof_run` [3] must report **"payload verified" 6/6**. `ipnet_ping_n` compares
  the echoed payload byte for byte against `i ^ 0x5A` (`src/ipnet.c:305-308`), so
  any truncation or mis-assembly fails here.
- `proof_run` [3b] must still resolve `example.com` **after** [3] has run. A
  desynced data toggle kills the very next transfer on the pipe; a passing [3b]
  is the cheapest toggle proof in the tree.
- `bench_datapath` [5] must still report ≥ 340 kB/s at every size. It already ran
  with the dump off, so this number must be **unchanged**, not improved — if [5]
  moves, something other than the dump changed.

**Risk.** Loses the hex dump that settled the raw-IP-versus-QMAP question
(`src/rmnet.c:13-15` says so). Keep the flag; only the default changes.

---

### R2 — Give the data plane its own sleep hook
**6 lines. Up to 2.3× on sustained RX.**

**Where:** `app/app_modem.c:64-68` (`at_sleep`), `:253` (`dp_ops`),
`src/ipnet.c:314`.

**Mechanism.** `dp_ops` reuses `at_sleep` as its sleep hook:

```c
static void at_sleep(uint32_t ms)
{
    uint32_t t0 = ms_now();
    do { pump_in(); } while (ms_now() - t0 < ms);
}
static const ipnet_ops_t dp_ops = { dp_tx, dp_rx, ms_now, at_sleep };
```

`pump_in()` is `usbh_bulk(&dev, &at_in, buf, 64, 5)` — a single-packet bulk IN on
the **AT** pipe with a 5 ms timeout. During the data plane the AT pipe is idle, so
every call spins the full 5 ms and returns `USBH_ERR_TIMEOUT`. The `do/while`
means **`sleep_ms(1)` costs ≥ 5 ms**, and for all of it the *data* IN channel is
unarmed — `bulk_once` halts it on every exit (`src/usbh.c:698`, `:707`).

`ipnet_ping_n`'s poll loop is `ops.rx(...)` then `ops.sleep_ms(1)`
(`src/ipnet.c:295-314`). On a busy downlink `ops.rx` returns as soon as a frame
lands, so the duty cycle is:

```
1428 B frame: 3.87 ms armed, then 5.00 ms blind
duty = 3.87 / (3.87 + 5.00) = 44 %
```

**The AT poll costs up to 56 % of downlink bandwidth**, and it is not only
latency: while the channel is unarmed the modem must buffer, and a full buffer
drops.

**Edit.**

```c
/* app/app_modem.c — add next to dp_tx/dp_rx, then replace the ops initialiser */

/* The data plane must NOT poll the AT pipe. at_sleep() spends >= 5 ms in a
 * bulk IN on at_in for every millisecond asked for, and bulk_once halts the
 * data IN channel on exit -- so each 1 ms "sleep" is a 5 ms window in which
 * the modem physically cannot hand us a downlink packet. */
static void dp_sleep(uint32_t ms)
{
    uint32_t t0 = ms_now();
    while (ms_now() - t0 < ms) { }
}

/* before */ static const ipnet_ops_t dp_ops = { dp_tx, dp_rx, ms_now, at_sleep };
/* after  */ static const ipnet_ops_t dp_ops = { dp_tx, dp_rx, ms_now, dp_sleep };
```

And delete the sleep from the echo poll entirely — `ops.rx` already blocks for
20 ms inside `usbh_bulk_frame`, so the sleep adds nothing but blind time:

```c
/* src/ipnet.c:314 — before */
        ops.sleep_ms(1);
/* after: delete the line. usbh_bulk_frame's own 20 ms timeout is the wait. */
```

Leave `ops.sleep_ms(10)` in `ipnet_dhcp` (`src/ipnet.c:253`) alone — DHCP is not
on this data path, QMI supplies the lease.

**Expected gain.** Sustained RX duty cycle 44 % → ~100 %, i.e. **up to 2.3×** on
downlink. Every echo RTT drops by ≥ 5 ms. No effect on TX.

**Tests.**
- [3] payload verified 6/6, [3b] resolves after [3] — as R1.
- The AT engine must still work *after* the data plane: `proof_run` is followed by
  `AT+CIMI` / `AT+COPS?` / `AT+CGDCONT?` (bench.txt:487-495). All three must still
  return `OK`. This proves `at_in` was not left broken by no longer being polled.
- Sanity: the [2] 20-ping minimum must not rise. If it does, the AT pipe was
  carrying something the data plane depended on.

---

### R3 — `bulk_once` timeout path: recover the toggle (and the bytes)
**~12 lines. 0 % throughput. Prevents a permanently dead pipe.**

**Where:** `src/usbh.c:697-700`.

**Mechanism.** On timeout the function halts the channel and returns without
reading `HCTSIZ` at all:

```c
    if (wait_reg(&OTG_HCINT(ch), HCINT_CHH, HCINT_CHH, timeout_ms)) {
        ch_halt(ch);
        return USBH_ERR_TIMEOUT;
    }
```

This was harmless while every request was one packet — a timed-out single-packet
read has nothing to recover and its toggle never advanced. It stopped being
harmless the moment `usbh_bulk_frame` began asking for **25 packets in one channel
program** (`src/usbh.c:801-805`). Now:

- The single 20 ms window covers all 25 packets. A frame that starts arriving in
  the last ~1.4 ms of the window is aborted **mid-transfer**.
- The already-DMA'd packets — up to 1536 B (24 whole packets), not 1599 — are
  discarded.
- Worse: `p->toggle` is left at its pre-transfer value while the *device's* toggle
  advanced once per packet sent. **If the aborted packet count is odd, roughly
  half the time, the pipe is permanently desynced** and every later transfer on it
  fails. That, not the lost 20 ms, is the severity.

Order-of-magnitude exposure: ~1.4 ms of vulnerable window against a ~25 ms poll
period ≈ **5 % of downlink frames**, if arrival and poll phase are uncorrelated.

The trigger named in the original review — an exact-MPS-multiple frame with no
terminating ZLP — is near-impossible here: under raw IP the short packet *is* the
only frame delimiter, and bench.txt shows flat 45 ms RTTs at 1400 B with no 20 ms
stalls. The mid-window abort is the real trigger.

**Edit.**

```c
/* src/usbh.c:697 — after */
    if (wait_reg(&OTG_HCINT(ch), HCINT_CHH, HCINT_CHH, timeout_ms)) {
        ch_halt(ch);                    /* blocks until CHENA clears */

        /* HCTSIZ is only meaningful once the channel is really halted, and
         * XFRSIZ is not reliable across an abnormal halt -- PKTCNT is.
         * The toggle MUST be resynced whether or not we return data: the
         * device advanced it once per packet it actually sent, and a
         * one-packet skew kills every later transfer on this pipe. */
        uint32_t left_pkt = (OTG_HCTSIZ(ch) >> HCTSIZ_PKTCNT_SHIFT) & 0x3FFu;
        uint32_t dpid     = (OTG_HCTSIZ(ch) >> HCTSIZ_DPID_SHIFT) & 3u;
        p->toggle = (dpid == PID_DATA0) ? 0u : 1u;

        /* A short packet would have set XFRC and completed the transfer, so a
         * TIMEOUT means every packet received was a full MPS -- the byte count
         * is exact, not an estimate. */
        if (is_in && left_pkt < npkt) {
            uint32_t got = (npkt - left_pkt) * p->mps;
            if (got > len) got = len;
            barrier_dsb();
            memcpy(buf, bounce, got);
            return (int)got;            /* got > 0; TIMEOUT still on got == 0 */
        }
        return USBH_ERR_TIMEOUT;
    }
```

`usbh_bulk_frame`'s existing logic then handles it: `r < want` ends the frame
(`src/usbh.c:826`), and `got == 0` still returns `USBH_ERR_TIMEOUT`, so the
"nothing arrived" path is unchanged.

**Expected gain.** Zero measured throughput. It removes the only way the
multi-packet RX path can be *worse* than the old clamped one, and it fails
silently today.

**Tests.**
- **Add an exact-multiple size to the sweep.** IP total = 20 + 8 + payload, so
  payload **1380** gives exactly 22 × 64 = 1408 B, and payload **1316** gives
  21 × 64 = 1344 B. Both must report "payload verified". This is the test that
  would have caught the bug; the current `sizes[]` contains no multiple of 64.
- **Toggle-desync soak:** run [2] with N = 200 instead of 20. A desync shows up as
  a run of consecutive failures that never recovers — the pipe is dead, not flaky.
  After the fix: zero unrecovered failures.
- [3b] must resolve after [3].

---

### R4 — Word-wide `memcpy`, checksum without `scratch`, zero-copy raw-IP TX
**~40 lines. 2.6 % today, ~8 % at the ceiling.**

**Where:** `src/libmin.c:12-18`, `src/ipnet.c:39-47` and `:83-110`,
`src/rmnet.c:82-85`.

**Mechanism.** `memcpy` is a byte-at-a-time loop compiled at `-O2`
(`Makefile:35`). One 1400 B UDP datagram passes through it five times:

| # | Copy | Bytes | Where |
|---|---|---|---|
| 1 | payload → `txf` | 1400 | `udp_build`, ipnet.c:91 |
| 2 | datagram → `scratch` (pseudo-header assembly) | 1408 | ipnet.c:105 |
| 3 | `csum16(scratch, 1420)` — 710 iterations | — | ipnet.c:107 |
| 4 | `txf+14` → `rmnet pkt` (strip Ethernet header) | 1428 | rmnet.c:84 |
| 5 | `pkt` → `pipe_buf` bounce in SRAM3 | 1428 | usbh.c:676 |

5664 bytes of byte-copy plus one 710-iteration checksum. At 200 MHz, ~4
cycles/byte plus ~8 cycles per checksum iteration:

```
copies    5664 × 4 = 22,656 cyc = 113 µs
checksum   710 × 8 =  5,680 cyc =  28 µs
                                  ------
total                             ~141 µs = 3.6 % of 3870 µs
```

Copies 2 and 4 are pure waste and can be deleted outright.

**Edit (a) — word-wide memcpy.**

```c
/* src/libmin.c — before */
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *p = dst; const unsigned char *q = src;
    while (n--) *p++ = *q++;
    return dst;
}

/* after */
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *p = dst; const unsigned char *q = src;
    /* Word copy while both sides are 4-aligned. Every buffer on the data path
     * is: txf/rxf/pkt in .bss (DTCM), pipe_buf is aligned(4) in .usb_ram. */
    if ((((uintptr_t)p | (uintptr_t)q) & 3u) == 0u) {
        uint32_t *pw = (uint32_t *)(void *)p;
        const uint32_t *qw = (const uint32_t *)(const void *)q;
        for (; n >= 16u; n -= 16u) {
            pw[0] = qw[0]; pw[1] = qw[1]; pw[2] = qw[2]; pw[3] = qw[3];
            pw += 4; qw += 4;
        }
        for (; n >= 4u; n -= 4u) *pw++ = *qw++;
        p = (unsigned char *)(void *)pw;
        q = (const unsigned char *)(const void *)qw;
    }
    while (n--) *p++ = *q++;
    return dst;
}
```

**Edit (b) — delete the `scratch` copy.** Split `csum16` into accumulate and fold:

```c
/* src/ipnet.c — replace csum16 */
static uint32_t csum_acc(const uint8_t *d, uint16_t len, uint32_t s)
{
    for (uint16_t i = 0; (i + 1u) < len; i += 2u) s += rd16(d + i);
    if (len & 1u) s += (uint32_t)d[len - 1u] << 8;
    return s;
}
static uint16_t csum_fold(uint32_t s)
{
    while (s >> 16) s = (s & 0xFFFFu) + (s >> 16);
    return (uint16_t)~s;
}
static uint16_t csum16(const uint8_t *d, uint16_t len)   /* keep: ip_hdr, icmp */
{ return csum_fold(csum_acc(d, len, 0)); }

/* udp_build: before -- memcpy 12 B of pseudo-header + (8+len) B into scratch,
 *                      then csum16(scratch, 20+len).
 * after: */
    uint32_t s = 0;
    s  = csum_acc(src_ip, 4, s);
    s  = csum_acc(dst_ip, 4, s);
    s += (uint32_t)PROTO_UDP;                 /* zero byte high, proto low */
    s += (uint32_t)(8u + len);
    uint16_t c = csum_fold(csum_acc(u, (uint16_t)(8u + len), s));
    wr16(u + 6, c ? c : 0xFFFFu);
```

This also frees the 1514-byte `scratch[]` static (`src/ipnet.c:23`).

**Edit (c) — zero-copy raw-IP TX.** In raw-IP mode there is no QMAP header, so
staging `frame+14` into `pkt[]` copies a whole frame for nothing:

```c
/* src/rmnet.c, rmnet_tx_eth() — before */
    } else {
        if (iplen > BUF_MAX) return -1;
        memcpy(pkt, ip, iplen);
        n = iplen;
    }
    if (r->debug) dump("tx", pkt, n);
    int w = usbh_bulk(r->dev, &r->out, pkt, n, 1000);

/* after */
    const uint8_t *src = pkt;
    if (r->qmap) {
        /* ...unchanged: build the QMAP header into pkt... */
        src = pkt;
    } else {
        if (iplen > BUF_MAX) return -1;
        src = ip;            /* already contiguous at frame+14 */
        n   = iplen;
    }
    if (r->debug) dump("tx", src, n);
    int w = usbh_bulk(r->dev, &r->out, (void *)src, n, 1000);
```

**Expected gain.** Deletes 2836 of 5664 copied bytes (≈ 57 µs) and speeds the
remaining 2828 by roughly 3× (≈ 42 µs). Net **~99 µs per 1428 B datagram**:

```
today          :  99 µs / 3870 µs = 2.6 %
at the ceiling :  99 µs / 1199 µs = 8.3 %
```

Ranked here because it is cheap and it is the term that *becomes* the limiter if
the bus-utilisation question in §1.4 turns out to be ours to fix.

**Tests.**
- [3] payload verified 6/6 — the walking pattern `i ^ 0x5A` catches an off-by-one
  in the word loop instantly.
- **Checksum regression:** [3b] must still resolve `example.com` (a wrong UDP
  checksum is dropped silently by the server, so DNS *is* the UDP-checksum test),
  and the ICMP replies in [3] must still match on `seq` (a wrong IP header
  checksum is dropped by the first router).
- Add one odd-length and one non-4-aligned case: ping payload **1** (IP total 29)
  and payload **1379** (IP total 1407). Both must verify.
- [5] uplink burst must not regress.

---

### R5 — Arm the next IN before processing the previous frame
**~80 lines. 2.2 % today, ~7 % at the ceiling. Do last.**

**Where:** `src/usbh.c:666-727` (`bulk_once`), `:783-830` (`usbh_bulk_frame`).

**Mechanism.** Between `HCINT.CHH` firing and the next `HCCHAR.CHENA` write, the
IN endpoint is unarmed and the modem cannot send. Per 1428 B RX frame that gap is:

```
memcpy bounce -> caller (SRAM3 -> DTCM, 1428 B)   ~29 µs
memcpy pkt -> eth-shim buffer (rmnet.c:117)       ~21 µs
ipnet parse + 1400-iteration payload verify       ~35 µs
                                                  -------
                                                   ~85 µs
```

**Edit.** Split `bulk_once` into `bulk_arm()` (program HCCHAR/HCTSIZ/HCDMA, set
CHENA, return) and `bulk_reap()` (spin on CHH, read back, copy out). Then in
`usbh_bulk_frame`, re-arm before copying:

```
arm(IN, slot A)
loop:
    reap(slot A)
    arm(IN, slot B)          <-- the bus is busy again before we touch slot A
    copy / parse slot A
    swap(A, B)
```

This needs a second bounce slot for the IN pipe. `pipe_buf` is `[4][1600]`
(`src/usbh.c:132`) and only four pipes are ever opened, but `usbh_pipe_open` caps
at `pipe_next_ch < 6`; give the data-IN pipe two slots, or grow `pipe_buf` to
`[5][1600]` (SRAM3 is 32 KiB, of which ~6.4 KiB is used).

**Expected gain.** 85 µs of 3870 µs = **2.2 %** today; 85 µs of 1199 µs = **7.1 %**
once the bus is the limiter. It is the only change here that lets RX actually
approach 1216 kB/s. It is also the largest and riskiest — do it last, and only
after §3.2 shows the bus headroom is real.

**Tests.**
- [3] payload verified 6/6 **and** the new exact-multiple sizes from R3 (1380,
  1316). A double-buffer bug shows up as a swapped or duplicated frame, which the
  walking pattern catches.
- [3b] after [3], plus the 200-ping soak from R3: an armed-but-unreaped channel
  leaks a toggle advance, which is exactly what the soak detects.
- Add a **frame-order** check: `ipnet_ping_n` already matches on `seq`; extend it
  to fail if an earlier `seq` arrives after a later one.

---

### Summary of expected gains

| # | Change | Work | Gain today | Gain at the ceiling |
|---|---|---|---|---|
| R1 | Default `rmn.debug` to 0 | 1 line | **5.2× RX** (61× at 92 B frames) | removes a hard cap |
| R2 | `dp_sleep`; drop the AT poll and `sleep_ms(1)` | 6 lines | **up to 2.3× sustained RX**, −5 ms/RTT | same |
| R3 | Toggle + partial recovery on timeout | 12 lines | 0 % (prevents a dead pipe) | 0 % |
| R4 | Word `memcpy`, no `scratch`, zero-copy raw-IP TX | 40 lines | 2.6 % | 8.3 % |
| R5 | Arm-before-process on IN | 80 lines | 2.2 % | 7.1 % |

R1 and R2 are the whole story. R4 and R5 together are worth about 5 % today and
about 15 % if and only if §3.2 shows the 3.3× of idle bus is recoverable.

---

## 3. BENCHMARK DESIGN

### 3.1 What to measure

Four numbers. Nothing else.

1. **Sustained one-way OUT, bytes/s.** Exists: the `bench_datapath()` uplink burst
   (`app/app_modem.c:387-407`) — 200 datagrams, no reply wait, network out of the
   loop. Two required extensions in §3.2.

2. **Sustained one-way IN, bytes/s.** **Missing, and the single biggest gap.**
   Nothing in the tree has ever measured downlink throughput. Build it as a tight
   loop that calls `rmnet_rx_eth()` and *nothing else* for a fixed 2 s window,
   counting bytes and frames, fed by a UDP flood from a host you control. Print
   only after the window closes.

3. **Per-transaction µs, split four ways.** `DWT_CYCCNT` brackets inside
   `bulk_once`, accumulated into statics and printed after the run:
   `t_copy_in` (memcpy → bounce), `t_program` (register writes), `t_spin`
   (CHENA → CHH), `t_copy_out` (bounce → caller). This is the **only** instrument
   that separates host cost from bus cost. If `t_spin` is ~3700 µs of a 3870 µs
   1428 B transfer, no software change will help and §3.2 is the next move. It is
   also what turns the ~141 µs software estimate in §1.4 into a measurement.

4. **Bus utilisation, as a percentage.**
   `util = (bytes × 0.8146 µs/B) / elapsed_µs`. Report it next to every rate.
   Today: **30.3 %**. It is the number that says how much is left.

Report **min / median / p90**, never a mean and never a single sample.

### 3.2 The one experiment that decides everything

The 3.3× of idle bus is either the LTE uplink NAKing us or something in the
driver. Eight lines settle it: **split the 200-datagram burst into first-20 and
last-180.**

```c
/* app/app_modem.c, bench_datapath() uplink loop */
uint32_t t0 = ms_now(), t20 = 0;
for (int i = 0; i < n; i++) {
    if (ipnet_udp_send(&ip, dst, 9, 5354, payload, sz) == 0) sent++;
    if (i == 19) t20 = ms_now();
}
uint32_t dt = ms_now() - t0;
uint32_t d1 = (t20 - t0) ? (t20 - t0) : 1u;
uint32_t d2 = (dt - (t20 - t0)) ? (dt - (t20 - t0)) : 1u;
kprintf("    first 20: %lu kB/s   last %d: %lu kB/s\n",
        (unsigned long)((20u * (sz + 28u)) / d1),
        n - 20,
        (unsigned long)(((uint32_t)(n - 20) * (sz + 28u)) / d2));
```

- **First 20 fast (approaching 1 MB/s), last 180 at ~371 kB/s** → the modem
  buffered until its uplink queue filled, then backpressured. **Bearer-limited.
  Stop optimising the host; R4 and R5 are worth 5 % and no more.**
- **Flat at ~371 kB/s from datagram 1** → the limit is in the host or the core.
  Then run instrument 3 above, and the 3.3× is on the table.

Corroborate with the radio: log `AT+QNWINFO`, `AT+QCSQ` and `AT+CESQ` immediately
before and after each burst. If 371 kB/s moves with band and RSRP, it is the
bearer. The tree already shows the radio moving an identical host path by 100 ms
(B7 64 ms versus B28 163 ms at 1400 B).

### 3.3 What to hold constant

| Hold | Why | How to enforce |
|---|---|---|
| `rmn.debug == 0` **and** `qmi.debug == 0` | §2 R1; `qmi.debug` is set at `app_modem.c:778` | Assert and print both in the header line; do not assume |
| `g_cpu_hz == 200000000` | `clock_init()` silently falls back to HSI 64 MHz on PLL failure (`system.c:57-86`) — that is a different machine | Print `g_cpu_hz` and `g_apb1_hz` in the header line |
| `p->mps == 64` | A composition change moves the endpoint | Print `r->in.mps` and `r->out.mps` |
| Band, cell, RSRP, time of day | The bearer is the suspected limiter | `AT+QNWINFO` + `AT+QCSQ` before **and** after; void the run if they differ |
| Payload sizes, N ≥ 200, run order | Comparability | Fixed `sizes[]` |
| Build flags | `-O2` (`Makefile:35`); `-O0` roughly triples the memcpy term | Print the git hash |
| **Zero output between `t0` and `t1`** | `uart_putc` blocks on TXE | Buffer; print after |

### 3.4 Traps

**1. The hex dump dominates any measurement it touches.** 16.4 ms of hard-spinning
115200 UART per frame, both directions, inside the timed window — `t0` is taken at
`ipnet.c:292` *before* `ops.tx`, and the rx dump runs inside `ops.rx` before the
reply is matched. It is 20–67 % of every RTT in the logs and caps RX at ~49
frames/s regardless of frame size. `bench_datapath()` clears it
(`app_modem.c:382-383`); **`proof_run()` does not.** The 62/62/72/88/106/163 ms
table was taken with it live.

**2. ICMP echo measures the network, not the host.** Full attribution of the
~101 ms slope between the 8 B and 1400 B points:

| Term | ms | Share |
|---|---|---|
| Host USB (22 extra packets each way, at the tree's own measured ~168 µs/pkt) | ~7.5 | 7 % |
| UART hex dump (flat above the 48-byte cap) | 6.6 | 7 % |
| **Network** | **~87** | **86 %** |

And it does not replicate: the same sweep on the same board gives 1400 B at 75 ms
(bench.txt:440), 74 ms (proof2.txt) and 64 ms (newsim2.txt), with 1024 B sometimes
*faster* than 256 B. Twenty identical 8 B pings span 39–69 ms. The distribution is
wider than the entire effect. **Never quote a single RTT, and never characterise
the host with `ipnet_ping_n`.**

**3. `ms_now()` is a 1 ms accumulator** (`app_modem.c:35-44`). The 270 µs
per-datagram figure is only meaningful because it is 54 ms / 200. Anything under
about 50 ms must be timed with `DWT_CYCCNT` (5 ns per tick at 200 MHz).

**4. `at_sleep` poisons any RX measurement routed through `ipnet_ping_n`** — a
≥ 5 ms blind window per loop iteration (§2 R2). Measure RX with a loop that calls
`rmnet_rx_eth()` and nothing else.

**5. `bench_datapath` mislabels its own column.** `"%4lu us/pkt"`
(`app_modem.c:404`) is µs per **datagram**, not per USB packet — at 1400 B one of
those is 23 USB packets. Rename it `us/dgram` and add a real µs/packet column:
`dt × 1000 / (sent × ceil((sz + 28) / 64))`.

**6. `bench_datapath`'s kB/s counts `sz + 28`** — the IP+UDP datagram. Under raw
IP that *is* the USB byte count, so it is the right figure for bus utilisation,
but it is not the payload rate. Say which one you are quoting.

**7. `if (dt == 0) dt = 1;`** (`app_modem.c:401`) manufactures a rate from a run
too short to measure. Require `dt ≥ 100 ms` or raise N.

**8. 200 datagrams may be too short.** 200 × 1428 B = 286 kB at 371 kB/s = 774 ms.
If the modem buffers 128 kB, the first third of the run is not bearer-limited and
the average hides it. That is exactly what §3.2's split exposes.

**9. A framing or toggle regression is silent.** Bake the battery in: [3] payload
verified **6/6**, with **1380 B and 1316 B added** (exact multiples of 64); [3b]
resolving **after** [3]; and a 200-ping soak with zero unrecovered failures.

---

## 4. WHAT WILL NOT HELP

| Idea | Why not |
|---|---|
| **Move to High Speed** | Not wired. `0x40080000` is USB2_OTG_FS with no ULPI pins; USB1_OTG_HS has the pins but the Nucleo-H745ZI-Q fits no PHY and the connector is on the FS core. Needs a carrier board. §0.1. |
| **Raise the CPU from 200 MHz to 480 MHz** | Host software is ~141 µs of a 3870 µs transfer = **3.6 %**. Doubling the clock buys under 2 %, and costs the VOS1 + SMPS bring-up that `system.c:52-55` deliberately stops short of. |
| **Turn on `GAHBCFG.GINT` / move off polling** | The spin is `wait_reg` on `HCINT.CHH` in a single-threaded bare-metal loop with **no other work to run**. An ISR would not start the next transfer any sooner. Interrupts pay only *after* R5 makes arm latency the thing being hidden. |
| **Bigger `USBH_BULK_BUF` or a bigger MPS** | Full Speed allows a bulk MPS of 8/16/32/64 only (USB 2.0 §5.8.3); the device reports 64 and that is the maximum. `USBH_BULK_BUF = 1600` already covers 25 packets — a whole 1500 B MTU in one transfer. Past the MTU it buys nothing: the frame ends on a short packet regardless. |
| **QMAP aggregation** | Tempting, but the measured fixed cost is **22 µs per transfer** and the measured limit is a **byte rate**. Packing 4 × 1428 B into one transfer saves 3 × 22 µs = 66 µs out of 15,480 µs = **0.4 %**. Aggregation pays at High Speed (512 B MPS, far more transfers/s); here it costs the QMAP header path and re-opens the framing question the hex dump settled. |
| **Two channels on the same endpoint** | The DWC2 keeps the data toggle per channel. Two channels driving one bulk endpoint corrupt the DATA0/DATA1 sequence. The correct pipelining is one channel with back-to-back arming — that is R5. |
| **MDMA/DMA for the bounce copy** | The copy is ~29 µs. MDMA setup plus a completion poll is a large fraction of that, and the M7 store buffer already streams to SRAM3 near AHB bandwidth. Small gain, large correctness surface (cache state, SRAM3 arbitration against the OTG DMA). Do R4's word copy instead. |
| **Tune `GUSBCFG.TRDT`** | It is 9 (`usbh.c:281`); ST's rule for AHB ≥ 32 MHz is 6. The difference is 3 PHY clocks = **62.5 ns** per turnaround, about 0.1 % of a 52 µs transaction. Fix it for tidiness, not throughput. |
| **Enlarge the FIFOs** | `GRXFSIZ = 568` words (2272 B) and `HNPTXFSIZ = 128` words (512 B = 8 packets) are already several packets deep, with `HBSTLEN = INCR16` and a 100 MHz AHB. The core is never FIFO-starved at 64 B MPS. Total is 952 of the core's 1024 words anyway — there is nowhere to go. |
| **Optimise the ping slope** | It is 86 % network and does not replicate. §3.4 trap 2. |
| **Chase the ZLP** | `usbh_bulk` appends a ZLP only when `len % 64 == 0` (`usbh.c:764`). Under raw IP with a 1500 B MTU that essentially never fires — none of the six sizes in the sweep triggers it. Removing it saves nothing and loses the U14 hardening. |

---

## 5. Order of work

1. **R1** (1 line) — then re-run `proof_run` and remeasure the whole [2]/[3] table
   with 20 samples per point. Everything before this is measuring USART3.
2. **R2** (6 lines) — re-run; expect −5 ms on every RTT.
3. **§3.2 first-20/last-180 split** (8 lines) plus the radio log. **This decides
   whether R4 and R5 are worth writing.**
4. **R3** (12 lines) — correctness. Add payloads 1380 and 1316 to `sizes[]` and run
   the 200-ping soak.
5. **Build the missing RX throughput test** (§3.1 item 2). Until it exists, half
   the data path has never been measured.
6. **R4**, then **R5** — only if step 3 says the bus headroom is real.

---

## 6. Sources

Every number above is either derived arithmetically from the USB 2.0
specification or read from a named file in this tree. Nothing is quoted from
memory.

- **Ceiling arithmetic:** USB 2.0 spec §5.11.3 Table 5-10 (FS bulk bus-time
  formula and the 19-transactions-per-frame figure).
- **Measured throughput:** `logs/modem/bench.txt:471-486` (n = 1).
- **Measured RTT distributions:** `logs/modem/bench.txt:381,395-440`;
  `logs/modem/sim-b.txt:377-436`; `logs/modem/proof2.txt`;
  `logs/modem/newsim2.txt`.
- **Code:** `src/usbh.c`, `src/rmnet.c`, `src/ipnet.c`, `src/libmin.c`,
  `src/uart.c`, `src/kprintf.c`, `src/system.c`, `src/stm32h745.h`,
  `app/app_modem.c`, `linker/stm32h745_cm7.ld`, `Makefile`.
- **Core capabilities:** `GHWCFG1..4 = 00000000 229fe190 03b8d2e8 e3f00030`
  (`logs/modem/bench.txt:5`) → architecture = 2 (internal buffer DMA),
  16 host channels, dedicated FS PHY.
- **Estimates, explicitly marked as such:** the ~4 cycles/byte `memcpy` cost and
  the ~8 cycles/iteration `csum16` cost (§1.4, R4). Both are unmeasured; §3.1
  item 3 is the instrument that replaces them with numbers.
