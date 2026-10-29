#!/usr/bin/env python3
"""Capture actual device UI renderers without sending any print job."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import time

from PIL import Image
import serial
from capture import recognized_text, until


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--pcm", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    with serial.Serial(args.port, 115200, timeout=1, write_timeout=30) as port, (args.output / "capture.log").open("wb") as log:
        port.write(b"INFO")
        until(port, log, time.monotonic() + 90, lambda s: s.startswith("MEM serving"))

        def command(value):
            port.write(value)
            until(port, log, time.monotonic() + 30, lambda s: s == "VIEW_READY " + value.decode())

        def screenshot(name, origin):
            deadline = time.monotonic() + 90
            port.write(b"LFRM")
            header = until(port, log, deadline, lambda s: s.startswith("SCREEN ")).split()
            width, height = map(int, header[1:3])
            assert (width, height) == (468, 468)
            pixels = bytearray()
            for y in range(height):
                row = until(port, log, deadline, lambda s: s.startswith("PIXELS ")).split()
                assert int(row[1]) == y
                data = bytes.fromhex(row[2])
                assert len(data) == width * 3
                pixels.extend(data)
            until(port, log, deadline, lambda s: s == "SCREEN_END")
            image = Image.frombytes("RGB", (width, height), bytes(pixels))
            assert image.getbbox() is not None
            target = args.output / name
            image.save(target)
            with Image.open(target) as saved:
                assert saved.tobytes() == bytes(pixels)
            records.append({"file": name, "width": width, "height": height, "bytes": target.stat().st_size,
                            "sha256": hashlib.sha256(target.read_bytes()).hexdigest(), "origin": origin})
            print(name, target.stat().st_size, flush=True)

        command(b"VWAI")
        screenshot("01-waiting.png", "waiting renderer")
        command(b"VREC")
        screenshot("02-recording.png", "recording renderer; diagnostic remaining=15s")
        pcm = args.pcm.read_bytes()
        port.write(b"RLMA" + struct.pack("<IB", len(pcm) // 2, 0) + pcm)
        line = until(port, log, time.monotonic() + 30, lambda s: s.startswith("TEXT "))
        assert recognized_text(line) == "やまだたろー", line
        command(b"VPRV")
        screenshot("03-previews.png", "six actual style rasters for saved recognized やまだたろー; first selected")
        for _ in range(4):
            command(b"NEXT")
        screenshot("04-previews-scrolled.png", "same actual list scrolled four text lines")
        command(b"VPRI")
        screenshot("05-printing.png", "printing renderer before sending; no BLE connection or print job")
        command(b"VOK!")
        screenshot("06-result-ok.png", "success renderer only; unavailable battery hidden, no success event simulated")
        command(b"VERR")
        screenshot("07-result-fail.png", "failure renderer only; no failed transaction simulated")
        command(b"VPPL")
        screenshot("08-people.png", "preset people list; first person selected")
        command(b"VPST")
        screenshot("09-preset-styles.png", "first preset's nine actual style rasters; plain kanji selected")
        for _ in range(6):
            command(b"NEXT")
        screenshot("10-preset-styles-scrolled.png", "same preset list scrolled six text lines")
        command(b"VWAI")
    (args.output / "capture.json").write_text(json.dumps({"status": "PASS", "paper_output": False,
        "source": "actual device scanout framebuffer", "screens": records,
        "limitations": ["Rendering captures do not prove hardware button operation or print success/failure",
                        "Low-volume warning remains unimplemented"]}, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
