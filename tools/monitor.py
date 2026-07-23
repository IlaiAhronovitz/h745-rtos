#!/usr/bin/env python3
"""Raw serial monitor for the Nucleo VCP. Byte-true passthrough + optional log.

    python tools/monitor.py --port COM44 --seconds 12 --log logs/run.txt
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM44")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=0, help="0 = run forever")
    ap.add_argument("--log", default=None)
    args = ap.parse_args()

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
