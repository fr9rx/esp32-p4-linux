#!/usr/bin/env python3
"""Drive the board's serial console non-interactively.

espflash monitor is read-only from a script's point of view, so this talks to
the port directly: wait for the login prompt, log in, then run a list of
commands and print everything that comes back.

  shell.py COM17 "uname -a" "cat /proc/cpuinfo"

Reset is done with DTR/RTS the same way espflash does it, so the board starts
from the bootloader banner rather than wherever it happened to be.
"""
import os
import sys
import time

import serial

BAUD = 4000000
# Per-command wait. mkfs and a whole-rootfs copy over a 20 MHz SD bus take
# far longer than anything else this script runs.
CMD_TIMEOUT = int(os.environ.get("CMD_TIMEOUT", "45"))
PROMPT = b"# "


def reset(ser):
    """DTR/RTS reset, matching espflash's default-reset sequence."""
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(0.05)
    ser.setDTR(False)


def read_until(ser, needles, timeout, echo=True):
    """Read until any of `needles` appears or `timeout` elapses.

    Returns (matched_needle_or_None, everything_read)."""
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            if echo:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
            for n in needles:
                if n in buf:
                    return n, buf
        else:
            time.sleep(0.01)
    return None, buf


def main():
    # The console here is cp1252, and serial output contains bytes that do not
    # survive that encoding -- the replacement char alone is enough to raise
    # UnicodeEncodeError on write. Same fix this project already uses in
    # hypmon.py.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    port = sys.argv[1]
    cmds = sys.argv[2:]

    with serial.Serial(port, BAUD, timeout=0.1) as ser:
        reset(ser)

        print("--- waiting for login prompt ---", flush=True)
        hit, _ = read_until(ser, [b"login:"], 120)
        if not hit:
            print("\n!!! no login prompt within 120 s", flush=True)
            return 1

        ser.write(b"root\r\n")
        # Buildroot's root account has no password, so the shell prompt should
        # follow directly; accept a Password: prompt too and answer with an
        # empty line rather than hanging on it.
        hit, _ = read_until(ser, [PROMPT, b"Password:"], 20)
        if hit == b"Password:":
            ser.write(b"\r\n")
            hit, _ = read_until(ser, [PROMPT], 20)
        if not hit:
            print("\n!!! no shell prompt after login", flush=True)
            return 1

        print("\n--- logged in, running commands ---", flush=True)
        for c in cmds:
            print(f"\n===== $ {c}", flush=True)
            ser.write(c.encode() + b"\r\n")
            read_until(ser, [PROMPT], CMD_TIMEOUT)

    print("\n--- done ---", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
