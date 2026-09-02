#!/usr/bin/env python3
"""Timestamped serial capture for the Nucleo's ST-Link VCP.

    python tools/capture.py COMxx logs/run.txt 240

Open this BEFORE flashing. The board starts printing the moment the reset at
the end of `program ... verify reset exit` releases, and anything written
before the port is open is simply lost -- that has cost a whole flash cycle
more than once.
"""
import sys
import time

import serial


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: capture.py <port> [log] [seconds]")
    port = sys.argv[1]
    path = sys.argv[2] if len(sys.argv) > 2 else "logs/capture.txt"
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 180.0

    sp = serial.Serial(port, 115200, timeout=0.1)
    sp.reset_input_buffer()
    t0 = time.monotonic()
    buf = bytearray()
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# capture {port} for {secs:.0f}s\n")
        f.flush()
        while time.monotonic() - t0 < secs:
            data = sp.read(4096)
            if not data:
                continue
            buf.extend(data)
            while True:
                nl = buf.find(b"\n")
                if nl < 0:
                    break
                line = bytes(buf[:nl]).decode("utf-8", "replace").rstrip("\r")
                del buf[:nl + 1]
                stamp = f"[{time.monotonic() - t0:7.2f}] "
                f.write(stamp + line + "\n")
                f.flush()
        if buf:
            f.write(f"[{time.monotonic() - t0:7.2f}] "
                    + bytes(buf).decode("utf-8", "replace") + "\n")
    sp.close()
    print(f"capture done: {path}")


if __name__ == "__main__":
    main()
