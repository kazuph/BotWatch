#!/usr/bin/env python3
"""Run a command while holding the StopWatch USB lock, so parallel work never flashes or talks to
the one board at the same time. Waits for the lock (up to --wait seconds), then runs the command.

python3 scripts/device_lock.py -- uvx --from esptool==5.6.0 esptool ... write-flash ...
python3 scripts/device_lock.py --wait 1800 -- uv run --with pyserial scripts/check_listen.py ...
"""
import argparse
import fcntl
import os
import subprocess
import sys
import time

LOCK = "/tmp/stopwatch-usbmodem.lock"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wait", type=int, default=3600, help="seconds to wait for the lock")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("no command")
    with open(LOCK, "a+") as handle:
        deadline = time.time() + args.wait
        while True:
            try:
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.time() > deadline:
                    sys.exit(f"DEVICE_LOCK timeout after {args.wait}s")
                time.sleep(2)
        handle.seek(0)
        handle.truncate()
        handle.write(f"pid={os.getpid()} cwd={os.getcwd()} since={time.strftime('%H:%M:%S')}\n")
        handle.flush()
        sys.exit(subprocess.run(command).returncode)


if __name__ == "__main__":
    main()
