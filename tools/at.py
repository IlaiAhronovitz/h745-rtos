#!/usr/bin/env python3
"""Minimal AT console for the Quectel AT port on Windows.

    python tools/at.py COMxx "ATI" "AT+QCFG=?"

Sends each command, prints the reply, stops at OK/ERROR. Read-only by
default -- it is just a serial writer; the safety is in WHICH commands you
pass. Used for the PC-side ECM investigation, where a real host (with lsusb,
option, cdc_ether) is on the other end if a composition change goes wrong.
"""
import sys
import time

import serial


def send(sp, cmd, timeout=8.0):
    sp.reset_input_buffer()
    sp.write((cmd + "\r").encode())
    out, t0 = "", time.monotonic()
    while time.monotonic() - t0 < timeout:
        n = sp.in_waiting
        if n:
            out += sp.read(n).decode("utf-8", "replace")
            if "\r\nOK" in out or "ERROR" in out:
                break
        time.sleep(0.05)
    return out.strip()


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    port, cmds = sys.argv[1], sys.argv[2:]
    with serial.Serial(port, 115200, timeout=0.2) as sp:
        for c in cmds:
            print(f"\n>>> {c}")
            print(send(sp, c))
    return 0


if __name__ == "__main__":
    sys.exit(main())
