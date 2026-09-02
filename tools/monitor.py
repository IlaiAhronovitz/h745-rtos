#!/usr/bin/env python3
"""Raw serial monitor for the Nucleo VCP. Byte-true passthrough + optional log.

    python tools/monitor.py --port COMxx --seconds 12 --log logs/run.txt

The ST-Link VCP number is assigned by Windows and moves between sessions
Check it with:
    [System.IO.Ports.SerialPort]::GetPortNames()
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=0, help="0 = run forever")
    ap.add_argument("--log", default=None)
    args = ap.parse_args()

    # The board can emit a partial byte mid-reset, which decodes to U+FFFD.
    # A cp1252 console cannot encode that and the whole monitor dies, so
    # force stdout to UTF-8 and never let an encoding error kill a capture.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    log = open(args.log, "wb") if args.log else None
    t_end = time.monotonic() + args.seconds if args.seconds else None

    with serial.Serial(args.port, args.baud, timeout=0.2) as sp:
        sys.stderr.write(f"[monitor] {args.port} @ {args.baud}\n")
        try:
            while t_end is None or time.monotonic() < t_end:
                chunk = sp.read(4096)
                if not chunk:
                    continue
                sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                sys.stdout.flush()
                if log:
                    log.write(chunk)
                    log.flush()
        except KeyboardInterrupt:
            pass
    if log:
        log.close()


if __name__ == "__main__":
    main()
