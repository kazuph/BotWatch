#!/usr/bin/env python3
"""Record serial continuously across USB re-enumeration using /dev kqueue events."""
import argparse
from contextlib import closing
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import time

import serial
from serial.tools import list_ports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--show-maximum", action="store_true", help="Show the maximum-length fixture once, before the human power-cycle test")
    parser.add_argument("--reset", action="store_true", help="Use esptool chip-id --after hard-reset once, then reconnect")
    args = parser.parse_args()
    identity = next(p for p in list_ports.comports() if p.device == args.port)
    if not identity.serial_number:
        raise RuntimeError("No stable USB serial identity; cannot safely choose a reconnect device")
    args.output.mkdir(parents=True, exist_ok=True)
    maximum_pending = args.show_maximum
    reset_pending = args.reset
    signal.signal(signal.SIGTERM, lambda *_: exit(0))
    directory = os.open("/dev", os.O_RDONLY)
    with closing(select.kqueue()) as queue, (args.output / "serial.log").open("ab", buffering=0) as raw, (args.output / "events.jsonl").open("a", buffering=1) as events:
        queue.control([select.kevent(directory, filter=select.KQ_FILTER_VNODE, flags=select.KQ_EV_ADD | select.KQ_EV_CLEAR, fflags=select.KQ_NOTE_WRITE | select.KQ_NOTE_EXTEND | select.KQ_NOTE_ATTRIB)], 0)

        def event(kind, **data):
            record = {"time": time.time(), "event": kind, **data}
            events.write(json.dumps(record) + "\n")
            print(json.dumps(record), flush=True)

        event("identity", serial_number=identity.serial_number, vid=identity.vid, pid=identity.pid)
        try:
            while True:
                candidates = [p for p in list_ports.comports() if p.serial_number == identity.serial_number and p.vid == identity.vid and p.pid == identity.pid]
                if not candidates:
                    event("waiting_for_usb")
                    queue.control(None, 1)
                    continue
                port = candidates[0].device
                try:
                    with serial.Serial(port, 115200, timeout=1) as connection:
                        event("connected", port=port)
                        connection.write(b"INFO")
                        while True:
                            line = connection.readline()
                            if line:
                                raw.write(line)
                                if reset_pending and line.startswith(b"MEM serving"):
                                    reset_pending = False
                                    event("hard_reset_requested", method="esptool chip-id --after hard-reset")
                                    connection.close()
                                    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
                                                    "--after", "hard-reset", "chip-id"], check=True)
                                    event("hard_reset_completed")
                                    break
                                if maximum_pending and line.startswith(b"MEM serving"):
                                    connection.write(b"VMAX")
                                    event("maximum_display_requested", characters=198)
                                    maximum_pending = False
                                if len(line) < 1024:
                                    print(line.decode("utf-8", errors="replace").rstrip(), flush=True)
                                else:
                                    event("bulk_line", bytes=len(line), prefix=line[:24].decode("ascii", errors="replace"))
                except (serial.SerialException, OSError) as error:
                    event("disconnected", error=str(error))
                    # The directory notification is queued before enumeration, so a
                    # device arriving during the failure cannot be missed.
                    if not any(p.serial_number == identity.serial_number for p in list_ports.comports()):
                        queue.control(None, 1)
        finally:
            os.close(directory)


if __name__ == "__main__":
    main()
