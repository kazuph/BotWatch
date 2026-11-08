#!/usr/bin/env python3
"""Capture the device's serial screenshot or latest saved JPEG."""
import argparse
import re
import time
from pathlib import Path

import serial
from PIL import Image


def read_exact(port, count, deadline):
    out = bytearray()
    while len(out) < count and time.monotonic() < deadline:
        out.extend(port.read(count - len(out)))
    if len(out) != count:
        raise TimeoutError(f"received {len(out)} of {count} bytes")
    return bytes(out)


def read_marker(port, pattern, deadline):
    while time.monotonic() < deadline:
        line = port.readline().strip()
        match = pattern.fullmatch(line)
        if match:
            return match
    raise TimeoutError("serial marker not received")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("kind", choices=("screen", "jpeg"))
    args = parser.parse_args()
    port = serial.Serial(port=None, baudrate=460800, timeout=0.2)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    try:
        port.reset_input_buffer()
        deadline = time.monotonic() + args.timeout
        port.write(b"s\n" if args.kind == "screen" else b"j\n")
        if args.kind == "screen":
            match = read_marker(port, re.compile(rb"SHOT (\d+) (\d+)"), deadline)
            width, height = map(int, match.groups())
            if (width, height) != (320, 240):
                raise ValueError(f"unexpected screen size: {width}x{height}")
            pixels = read_exact(port, width * height * 3, deadline)
            read_marker(port, re.compile(rb"ENDSHOT"), deadline)
            Image.frombytes("RGB", (width, height), pixels).save(args.output)
        else:
            match = read_marker(port, re.compile(rb"JPEG (\d+)"), deadline)
            data = read_exact(port, int(match.group(1)), deadline)
            with Image.open(__import__("io").BytesIO(data)) as image:
                image.load()
                print(f"decoded JPEG: {image.width}x{image.height}")
            args.output.write_bytes(data)
        print(f"saved {args.kind}: {args.output}")
    finally:
        port.close()


if __name__ == "__main__":
    main()
