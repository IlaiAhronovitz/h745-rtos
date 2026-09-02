#!/usr/bin/env python3
"""Hardware-in-the-loop test harness for h745-rtos on the Nucleo-H745ZI-Q.

One command builds every app, flashes each onto the real board, watches the
VCP serial output for the evidence, and -- for the Ethernet app -- exercises
the data path from the PC side of the cable (ARP, ICMP, a UDP flood into the
RX ring, and the board's own 1 Hz broadcast counted at the PC NIC).

    python tools/hil.py                  # full run: build, test, demo, eth
    python tools/hil.py --stages eth     # just the Ethernet stage
    python tools/hil.py --stages test,demo
    python tools/hil.py --no-build       # reuse existing build/hil-* output

Wiring assumed (the bench as documented in the README):
  - Nucleo USB  -> PC  (ST-Link V3: flashing via OpenOCD + VCP serial)
  - Nucleo RJ45 -> PC Ethernet, direct cable
        board 192.168.137.50 / 02:00:00:74:50:01, PC 192.168.137.1

The COM port number is Windows-assigned and drifts; the harness finds the
port by the ST-Link's USB serial number instead. Flashing goes through
`make ... flash`, so the serial pinning and connect_assert_srst logic live
in one place (the Makefile), not two.

Exit code: 0 = every gate passed, 1 = at least one FAIL, 2 = setup error.

Requires: pyserial. Uses ping.exe / arp.exe / powershell for the PC-side
network checks (no packet-capture driver needed).
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone

import serial
import serial.tools.list_ports

# ---- bench constants -------------------------------------------------------

STLINK_SERIAL = os.environ.get("STLINK_SERIAL", "")  # pin a probe; empty = first ST-Link
BOARD_IP      = "192.168.137.50"
BOARD_MAC_PS  = "02-00-00-74-50-01"          # as arp.exe prints it
PC_IP         = "192.168.137.1"
UDP_PORT      = 5001                          # arbitrary: board counts frames,
                                              # it does not parse UDP (yet)
BAUD          = 115200
MSYS_BIN      = r"C:\msys64\usr\bin"

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# App banners: proof that the flash actually switched programs.
BANNER = {
    "test": "=== h745-rtos: APP=test build ===",
    "demo": "=== h745-rtos ===",
    "eth":  "=== h745-rtos: ethernet on the kernel ===",
    "usb":  "=== h745-rtos: usb host (APP=usb) ===",
}

COUNTER_RE = re.compile(
    r"tx (\d+) \| rx (\d+) frames, (\d+) bytes \| rx irqs (\d+)")

# ---- reporting -------------------------------------------------------------

class Report:
    def __init__(self):
        self.rows = []          # (stage, check, status, detail)
        self.t0 = time.time()

    def add(self, stage, check, status, detail=""):
        self.rows.append((stage, check, status, detail))
        print(f"  [{status:4}] {check}" + (f"  -- {detail}" if detail else ""),
              flush=True)

    def counts(self):
        n = {"PASS": 0, "FAIL": 0, "INFO": 0, "SKIP": 0}
        for _, _, s, _ in self.rows:
            n[s] = n.get(s, 0) + 1
        return n

    def save(self, path, meta):
        data = {
            "meta": meta,
            "elapsed_s": round(time.time() - self.t0, 1),
            "results": [
                {"stage": s, "check": c, "status": st, "detail": d}
                for s, c, st, d in self.rows
            ],
        }
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)

    def summary(self):
        n = self.counts()
        verdict = "PASS" if n["FAIL"] == 0 else "FAIL"
        return (f"{verdict}: {n['PASS']} passed, {n['FAIL']} failed, "
                f"{n['INFO']} info, {n['SKIP']} skipped")


class StageAbort(Exception):
    """A prerequisite inside a stage failed; skip the rest of that stage."""


# ---- serial capture --------------------------------------------------------

class SerialCapture:
    """Background reader for the VCP. wait_for() scans everything received
    since a mark, so output from the previously flashed app can never satisfy
    a check aimed at the new one."""

    def __init__(self, port, baud=BAUD):
        self.sp = serial.Serial(port, baud, timeout=0.2)
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.stop_flag = False
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        while not self.stop_flag:
            try:
                chunk = self.sp.read(4096)
            except serial.SerialException:
                break
            if chunk:
                with self.lock:
                    self.buf.extend(chunk)

    def mark(self):
        with self.lock:
            return len(self.buf)

    def text(self, since=0):
        with self.lock:
            return self.buf[since:].decode("utf-8", errors="replace")

    def wait_for(self, pattern, timeout, since=0):
        """Wait until `pattern` (regex) appears after byte offset `since`.
        Returns the match object, or None on timeout."""
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            m = rx.search(self.text(since))
            if m:
                return m
            time.sleep(0.1)
        return None

    def save(self, path, since=0):
        with open(path, "wb") as f:
            with self.lock:
                f.write(self.buf[since:])

    def close(self):
        self.stop_flag = True
        self.thread.join(timeout=1.0)
        self.sp.close()


# ---- process helpers -------------------------------------------------------

def run(cmd, timeout, cwd=None, env=None):
    """Run a command, return (rc, combined output)."""
    try:
        p = subprocess.run(cmd, cwd=cwd, env=env, timeout=timeout,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return p.returncode, p.stdout.decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", errors="replace")
        return -1, out + f"\n[hil] TIMED OUT after {timeout}s"


def make(targets, log_path=None, timeout=240):
    """Invoke msys make in the repo. Recipes use mkdir/rm, so msys bin goes
    onto PATH; the compiler and OpenOCD paths are the Makefile's business."""
    env = dict(os.environ)
    env["PATH"] = MSYS_BIN + os.pathsep + env["PATH"]
    cmd = [os.path.join(MSYS_BIN, "make.exe")] + targets
    rc, out = run(cmd, timeout=timeout, cwd=REPO, env=env)
    if log_path:
        with open(log_path, "a", encoding="utf-8") as f:
            f.write(f"$ make {' '.join(targets)}\n{out}\n")
    return rc, out


def ps(command, timeout=30):
    rc, out = run(["powershell", "-NoProfile", "-Command", command],
                  timeout=timeout)
    return rc, out.strip()


# ---- PC-side network helpers ----------------------------------------------

def nic_alias():
    rc, out = ps(f"(Get-NetIPAddress -IPAddress {PC_IP} "
                 f"-ErrorAction SilentlyContinue).InterfaceAlias")
    return out if rc == 0 and out else None


def nic_link(alias):
    rc, out = ps(f"$a = Get-NetAdapter -Name '{alias}'; "
                 f"\"$($a.Status)|$($a.LinkSpeed)\"")
    if rc != 0 or "|" not in out:
        return None, None
    status, speed = out.split("|", 1)
    return status.strip(), speed.strip()


def nic_broadcast_count(alias):
    rc, out = ps(f"(Get-NetAdapterStatistics -Name '{alias}')"
                 f".ReceivedBroadcastPackets")
    try:
        return int(out)
    except ValueError:
        return None


def ping_burst(count, timeout_ms=1000):
    """Returns (replies, rtts_ms list)."""
    rc, out = run(["ping", "-n", str(count), "-w", str(timeout_ms), BOARD_IP],
                  timeout=count * (timeout_ms / 1000.0 + 1) + 10)
    replies = re.findall(rf"Reply from {re.escape(BOARD_IP)}.*", out)
    rtts = []
    for line in replies:
        m = re.search(r"time[=<](\d+)ms", line)
        if m:
            rtts.append(int(m.group(1)))
    return len(replies), rtts, out


def arp_entry():
    rc, out = run(["arp", "-a", BOARD_IP], timeout=10)
    return BOARD_MAC_PS in out.lower().replace(":", "-") or BOARD_MAC_PS in out


def udp_flood(n, size=400, gap_s=0.002):
    """Send n UDP datagrams at the board. Nothing listens (the board has no
    UDP yet) -- the point is that each datagram is one Ethernet frame the
    promiscuous MAC must receive and count."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    payload = bytearray(size)
    for i in range(n):
        payload[0:4] = i.to_bytes(4, "big")
        s.sendto(payload, (BOARD_IP, UDP_PORT))
        if gap_s:
            time.sleep(gap_s)
    s.close()


def last_counters(cap, since):
    """Latest 'tx N | rx N frames...' line after `since`, or None."""
    matches = list(COUNTER_RE.finditer(cap.text(since)))
    if not matches:
        return None
    m = matches[-1]
    return {"tx": int(m.group(1)), "frames": int(m.group(2)),
            "bytes": int(m.group(3)), "irqs": int(m.group(4))}


def wait_fresh_counters(cap, since, timeout=6):
    """Wait for a counter line that arrives from now on (not a stale one)."""
    mark_now = cap.mark()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        c = last_counters(cap, mark_now)
        if c:
            return c
        time.sleep(0.2)
    return None


# ---- stages ----------------------------------------------------------------

def flash_and_expect_banner(rep, cap, app, log_dir):
    """Flash APP=<app> (its own build dir) and require that app's banner."""
    mark = cap.mark()
    rc, out = make([f"APP={app}", f"BUILD=build/hil-{app}", "flash"],
                   log_path=os.path.join(log_dir, f"flash-{app}.log"))
    if rc != 0:
        rep.add(app, f"flash APP={app}", "FAIL", "make flash failed -- see log")
        raise StageAbort()
    rep.add(app, f"flash APP={app}", "PASS")

    if not cap.wait_for(re.escape(BANNER[app]), timeout=15, since=mark):
        rep.add(app, "boot banner", "FAIL",
                f"'{BANNER[app]}' not seen within 15 s of reset")
        raise StageAbort()
    rep.add(app, "boot banner", "PASS", BANNER[app])
    return mark


def stage_build(rep, apps, log_dir):
    print("\n== build ==")
    ok = True
    for app in apps:
        rc, out = make([f"APP={app}", f"BUILD=build/hil-{app}", "all"],
                       log_path=os.path.join(log_dir, f"build-{app}.log"))
        if rc != 0:
            tail = "\n".join(out.strip().splitlines()[-8:])
            rep.add("build", f"make APP={app}", "FAIL", tail)
            ok = False
            continue
        m = re.search(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)", out, re.M)
        size = f"text={m.group(1)} data={m.group(2)} bss={m.group(3)}" if m else ""
        rep.add("build", f"make APP={app}", "PASS", size)
    if not ok:
        raise StageAbort()


def stage_test(rep, cap, log_dir):
    """APP=test: 6 measured kernel tests + the deliberate T7 stack panic."""
    print("\n== test suite (APP=test) ==")
    mark = flash_and_expect_banner(rep, cap, "test", log_dir)

    # The suite ends in a DELIBERATE panic; that line is the finish flag.
    m = cap.wait_for(r"KERNEL PANIC: stack overflow\s+task=victm",
                     timeout=300, since=mark)
    text = cap.text(mark)
    cap.save(os.path.join(log_dir, "test.log"), since=mark)

    if not m:
        rep.add("test", "suite ran to completion", "FAIL",
                "T7 panic line never arrived (suite hung?)")
        raise StageAbort()
    rep.add("test", "T7 stack canary panic", "PASS", "deliberate, task=victm")

    n_pass = len(re.findall(r"\[PASS\]", text))
    n_fail = len(re.findall(r"\[FAIL\]", text))
    st = "PASS" if (n_pass == 6 and n_fail == 0) else "FAIL"
    rep.add("test", "T1-T6 verdicts", st, f"{n_pass} PASS / {n_fail} FAIL")

    for pat, name in [
        (r"T1 yield round trip: (.*)", "T1 context switch"),
        (r"T2 give->preempt->wake: (.*)", "T2 preemption"),
        (r"T3 equal-prio busy tasks, 1 s: (.*)", "T3 round-robin"),
        (r"T4 \d+ tokens producer->consumer in (.*)", "T4 semaphores"),
        (r"T5 k_sleep_ms\(50\) x20: (.*)", "T5 sleep"),
        (r"T6 .*rounds each: (.*)", "T6 FPU"),
    ]:
        mm = re.search(pat, text)
        if mm:
            rep.add("test", name, "INFO", mm.group(1).strip())


def stage_demo(rep, cap, log_dir):
    """APP=demo: kernel demo -- verify the stats block and FPU integrity."""
    print("\n== kernel demo (APP=demo) ==")
    mark = flash_and_expect_banner(rep, cap, "demo", log_dir)
    t_banner = time.monotonic()

    # stats prints every 5 s and its LAST line is the fpu summary, so seeing
    # that line proves a complete block landed. (First harness run caught the
    # original demo starving stats at prio 5: two characters in 20 s.)
    mm = cap.wait_for(
        r"fpu: a=(\d+) rounds/(\d+) fail\s+b=(\d+) rounds/(\d+) fail",
        timeout=25, since=mark)
    latency = time.monotonic() - t_banner
    text = cap.text(mark)
    cap.save(os.path.join(log_dir, "demo.log"), since=mark)

    if not mm:
        rep.add("demo", "stats block (5 s cadence)", "FAIL",
                "no complete block within 25 s -- stats starved again?")
        raise StageAbort()
    rep.add("demo", "stats block (5 s cadence)", "PASS",
            f"first complete block {latency:.1f} s after boot")

    expected = ["btn", "green", "yello", "fpu-a", "fpu-b", "stats", "idle"]
    missing = [t for t in expected if t not in text]
    rep.add("demo", "all tasks registered", "FAIL" if missing else "PASS",
            f"missing: {missing}" if missing else "7 tasks incl. idle")

    fails = int(mm.group(2)) + int(mm.group(4))
    rep.add("demo", "FPU context integrity", "PASS" if fails == 0 else "FAIL",
            f"a={mm.group(1)}r/{mm.group(2)}f b={mm.group(3)}r/{mm.group(4)}f")

    mm = re.search(r"-- t=(\d+) ms, (\d+) context switches", text)
    if mm:
        rep.add("demo", "context switches", "INFO",
                f"{mm.group(2)} in first {mm.group(1)} ms")
    rep.add("demo", "button ISR latency", "SKIP", "needs a human to press B1")


def stage_eth(rep, cap, log_dir, pings, udp_n):
    """APP=eth: PHY, link, MAC start, then the data path from the PC side."""
    print("\n== ethernet (APP=eth) ==")
    mark = flash_and_expect_banner(rep, cap, "eth", log_dir)
    try:
        # -- PHY answers on MDIO
        if cap.wait_for(r"PASS: LAN8742A responding", timeout=10, since=mark):
            mm = re.search(r"ID1=([0-9a-f]{4}) ID2=([0-9a-f]{4})",
                           cap.text(mark))
            rep.add("eth", "PHY MDIO probe", "PASS",
                    f"ID1={mm.group(1)} ID2={mm.group(2)}" if mm else "")
        else:
            rep.add("eth", "PHY MDIO probe", "FAIL",
                    "no 'LAN8742A responding' -- MDIO dead?")
            raise StageAbort()

        # -- link + autonegotiation + MAC start
        m = cap.wait_for(r"negotiated (\d+) Mbps, (\w+) duplex", timeout=20,
                         since=mark)
        if not m:
            rep.add("eth", "link up + autoneg", "FAIL",
                    "no link within 20 s -- cable to the PC plugged in?")
            raise StageAbort()
        speed, duplex = m.group(1), m.group(2)
        st = "PASS" if (speed == "100" and duplex == "full") else "FAIL"
        rep.add("eth", "link up + autoneg", st, f"{speed} Mbps {duplex} duplex")

        if not cap.wait_for(r"MAC started", timeout=10, since=mark):
            rep.add("eth", "MAC started", "FAIL", "eth_start never reported")
            raise StageAbort()
        rep.add("eth", "MAC started", "PASS")

        # -- PC-side checks need the NIC
        alias = nic_alias()
        if not alias:
            rep.add("eth", "PC NIC at " + PC_IP, "FAIL",
                    "no adapter has that address -- network checks skipped")
            raise StageAbort()
        status, lspeed = nic_link(alias)
        rep.add("eth", "PC NIC at " + PC_IP, "PASS",
                f"'{alias}' {status} {lspeed}")

        # -- ICMP: arp_reply() + icmp_reply() under a real ping burst.
        # Gate is 95%, not 100%: Windows neighbor-discovery re-probes the
        # entry mid-burst (ICMP replies do not refresh reachability, only ARP
        # does), and that transition occasionally costs one ping. Measured
        # 2026-08-24: 100/100 in a long burst, 19/20 across a NUD transition.
        ping_burst(1)                     # warm-up: resolves ARP off the clock
        replies, rtts, raw = ping_burst(pings)
        with open(os.path.join(log_dir, "ping.log"), "w",
                  encoding="utf-8") as f:
            f.write(raw)
        need = max(1, (pings * 95 + 99) // 100)
        st = "PASS" if replies >= need else "FAIL"
        rtt_s = (f"rtt min/avg/max = {min(rtts)}/"
                 f"{sum(rtts)//len(rtts)}/{max(rtts)} ms" if rtts else "")
        rep.add("eth", f"ping x{pings}", st,
                f"{replies}/{pings} replies (gate >= {need})  {rtt_s}")

        rep.add("eth", "ARP entry on PC", "PASS" if arp_entry() else "FAIL",
                BOARD_MAC_PS)

        # -- RX path: flood UDP frames, board's counters must advance by >= n
        c0 = wait_fresh_counters(cap, mark)
        if not c0:
            rep.add("eth", "counter line", "FAIL", "tx task not printing")
            raise StageAbort()
        udp_flood(udp_n, size=400, gap_s=0.002)
        time.sleep(3.0)
        c1 = wait_fresh_counters(cap, mark)
        got = c1["frames"] - c0["frames"] if c1 else -1
        rep.add("eth", f"RX flood ({udp_n} UDP frames, paced 2 ms)",
                "PASS" if got >= udp_n else "FAIL",
                f"rx frames +{got} (>= {udp_n} expected)")

        # Unpaced burst: 4-descriptor ring vs line rate. Drops are allowed --
        # this is a measurement of the drain loop, not a gate.
        c2 = wait_fresh_counters(cap, mark)
        udp_flood(udp_n, size=400, gap_s=0)
        time.sleep(3.0)
        c3 = wait_fresh_counters(cap, mark)
        if c2 and c3:
            got = c3["frames"] - c2["frames"]
            rep.add("eth", f"RX burst ({udp_n} UDP frames, unpaced)", "INFO",
                    f"rx frames +{got} of {udp_n} "
                    f"({100 * got // udp_n}% kept, ring depth is 4)")

        # -- TX path: board broadcasts 1 Hz; the PC NIC counts them. No
        #    capture driver needed -- the adapter statistics are the witness.
        b0 = nic_broadcast_count(alias)
        t0 = c3 or c2 or c1 or c0
        time.sleep(8.0)
        b1 = nic_broadcast_count(alias)
        t1 = wait_fresh_counters(cap, mark)
        if b0 is None or b1 is None:
            rep.add("eth", "TX broadcasts reach PC", "FAIL",
                    "could not read NIC statistics")
        else:
            got = b1 - b0
            rep.add("eth", "TX broadcasts reach PC",
                    "PASS" if got >= 4 else "FAIL",
                    f"NIC ReceivedBroadcastPackets +{got} in 8 s "
                    f"(board sends 1 Hz)")
        if t0 and t1:
            sent = t1["tx"] - t0["tx"]
            rep.add("eth", "TX counter advances", "PASS" if sent >= 4 else "FAIL",
                    f"tx +{sent} in the same window")

        # -- IRQ path sanity: every frame came via ISR->semaphore; the drain
        #    loop means frames >= irqs, and both must be alive.
        if t1:
            st = "PASS" if (t1["irqs"] > 0 and t1["frames"] >= t1["irqs"]) \
                 else "FAIL"
            rep.add("eth", "IRQ/drain-loop sanity", st,
                    f"{t1['frames']} frames / {t1['irqs']} irqs "
                    f"(+{t1['frames'] - t1['irqs']} swept by drain loop)")
    finally:
        cap.save(os.path.join(log_dir, "eth.log"), since=mark)


def stage_usb(rep, cap, log_dir):
    """APP=usb: host-core bring-up gates. Proves clocks/power/core/host-mode/
    VBUS on every run; enumeration is exercised only if a device is attached
    to CN13 at run time (reported as PASS or INFO accordingly)."""
    print("\n== usb host (APP=usb) ==")
    mark = flash_and_expect_banner(rep, cap, "usb", log_dir)
    try:
        # -- core answers with sane silicon IDs
        m = cap.wait_for(r"GSNPSID=([0-9a-f]{8})", timeout=10, since=mark)
        if not m:
            rep.add("usb", "core ID readout", "FAIL", "no GSNPSID line")
            raise StageAbort()
        snpsid = m.group(1)
        st = "PASS" if snpsid not in ("00000000", "ffffffff") else "FAIL"
        rep.add("usb", "core ID readout", st, f"GSNPSID={snpsid}")

        # -- internal DMA architecture (GHWCFG2 bits [4:3] == 2)
        m = cap.wait_for(r"GHWCFG1\.\.4 = ([0-9a-f]{8}) ([0-9a-f]{8})",
                         timeout=5, since=mark)
        if m:
            arch = (int(m.group(2), 16) >> 3) & 3
            rep.add("usb", "internal DMA architecture",
                    "PASS" if arch == 2 else "FAIL",
                    f"GHWCFG2={m.group(2)} arch={arch} (need 2)")
        else:
            rep.add("usb", "internal DMA architecture", "FAIL",
                    "GHWCFG line not seen")

        # -- the whole init sequence survived: clocks, PWR, reset, host mode
        if not cap.wait_for(r"host mode, VBUS on, port powered",
                            timeout=10, since=mark):
            rep.add("usb", "host bring-up", "FAIL",
                    "init did not reach 'port powered'")
            raise StageAbort()
        rep.add("usb", "host bring-up", "PASS",
                "48 MHz + USB33 + core reset + forced host + VBUS")

        # -- enumeration: only provable with a device on CN13
        m = cap.wait_for(r"ENUMERATED: addr (\d+), (\w+) speed, "
                         r"VID:PID ([0-9a-f]{4}):([0-9a-f]{4})",
                         timeout=8, since=mark)
        if m:
            rep.add("usb", "device enumeration", "PASS",
                    f"{m.group(3)}:{m.group(4)} at addr {m.group(1)}, "
                    f"{m.group(2)} speed")
        elif cap.wait_for(r"waiting for a device", timeout=5, since=mark):
            rep.add("usb", "device enumeration", "INFO",
                    "no device on CN13 -- EP0 engine NOT exercised this run")
        else:
            rep.add("usb", "device enumeration", "FAIL",
                    "neither enumeration nor idle-wait seen")
    finally:
        cap.save(os.path.join(log_dir, "usb.log"), since=mark)


# ---- main ------------------------------------------------------------------

def find_port():
    for p in serial.tools.list_ports.comports():
        hwid = (p.hwid or "").upper()
        if STLINK_SERIAL and STLINK_SERIAL.upper() in hwid:
            return p.device
        if not STLINK_SERIAL and "VID:PID=0483:" in hwid:   # any ST-Link VCP
            return p.device
    return None


def git_rev():
    rc, out = run(["git", "rev-parse", "--short", "HEAD"], timeout=10, cwd=REPO)
    return out.strip() if rc == 0 else "?"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--stages", default="test,demo,eth,usb",
                    help="comma list from: test,demo,eth,usb (default: all; "
                         "the last stage's app stays flashed)")
    ap.add_argument("--no-build", action="store_true",
                    help="skip the explicit build pass (flash still rebuilds)")
    ap.add_argument("--port", default=None,
                    help="VCP COM port (default: autodetect by ST-Link serial)")
    ap.add_argument("--pings", type=int, default=20)
    ap.add_argument("--udp", type=int, default=200,
                    help="frames per UDP flood")
    args = ap.parse_args()

    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    stages = [s.strip() for s in args.stages.split(",") if s.strip()]
    bad = [s for s in stages if s not in ("test", "demo", "eth", "usb")]
    if bad:
        print(f"unknown stage(s): {bad}")
        return 2

    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%SZ")
    log_dir = os.path.join(REPO, "logs", "hil", stamp)
    os.makedirs(log_dir, exist_ok=True)

    rep = Report()
    print(f"h745-rtos HIL  ({stamp}, git {git_rev()})")
    print(f"logs -> {os.path.relpath(log_dir, REPO)}")

    # -- discovery
    print("\n== discover ==")
    port = args.port or find_port()
    if not port:
        rep.add("discover", "ST-Link VCP", "FAIL",
                f"no ST-Link VCP found (STLINK_SERIAL={STLINK_SERIAL or 'any'}) -- board plugged in?")
        print("\n" + rep.summary())
        return 2
    rep.add("discover", "ST-Link VCP", "PASS", f"{port} (serial matched)")

    try:
        cap = SerialCapture(port)
    except serial.SerialException as e:
        rep.add("discover", "open " + port, "FAIL",
                f"{e} -- another program holds the port?")
        print("\n" + rep.summary())
        return 2
    rep.add("discover", "open " + port, "PASS", f"{BAUD} baud")

    try:
        if not args.no_build:
            stage_build(rep, stages, log_dir)

        for name, fn in (("test", stage_test), ("demo", stage_demo)):
            if name in stages:
                try:
                    fn(rep, cap, log_dir)
                except StageAbort:
                    pass
        if "eth" in stages:
            try:
                stage_eth(rep, cap, log_dir, args.pings, args.udp)
            except StageAbort:
                pass
        if "usb" in stages:
            try:
                stage_usb(rep, cap, log_dir)
            except StageAbort:
                pass
    except StageAbort:
        pass                                  # build failed; results recorded
    finally:
        cap.close()

    meta = {"stamp": stamp, "git": git_rev(), "port": port,
            "stages": stages, "board_ip": BOARD_IP}
    rep.save(os.path.join(log_dir, "report.json"), meta)

    n = rep.counts()
    print(f"\n== result ==\n{rep.summary()}")
    print(f"report: {os.path.relpath(os.path.join(log_dir, 'report.json'), REPO)}")
    return 0 if n["FAIL"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
