#!/usr/bin/env python3
"""Copy every photo on the CoreS3 SD card to this computer over USB serial.

The SD card stays in the device. Files already copied with the same size are skipped.
Usage: uv run --with pyserial faces-arcade/tools/backup_photos.py --port /dev/cu.usbmodemXXXX
"""
import argparse
import pathlib
import sys
import time

import serial


def read_header(port, deadline):
    while time.monotonic() < deadline:
        line = port.readline()
        if line.startswith(b"PHOTO "):
            return line.decode(errors="replace").split()
    raise TimeoutError("本体から写真の返事がありません")


def fetch(port, index):
    port.reset_input_buffer()
    port.write(b"J%d\n" % index)
    port.flush()
    header = read_header(port, time.monotonic() + 10)
    if header[1] == "error":
        return int(header[2]), None, " ".join(header[3:])
    name, total, size = header[1], int(header[2]), int(header[3])
    data = bytearray()
    deadline = time.monotonic() + 30
    while len(data) < size:
        if time.monotonic() > deadline:
            raise TimeoutError(f"{name} の受信が途中で止まりました ({len(data)}/{size})")
        data += port.read(size - len(data))
    if not (data.startswith(b"\xff\xd8") and data.rstrip(b"\x00").endswith(b"\xff\xd9")):
        raise ValueError(f"{name} が JPEG の形になっていません")
    return total, (name, bytes(data)), None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--out", default=str(pathlib.Path.home() / "Downloads" / "BotWatch-photos"))
    args = parser.parse_args()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    port = serial.Serial(port=None, baudrate=460800, timeout=1)
    port.dtr = False  # opening the port must not reset the device
    port.rts = False
    port.port = args.port
    port.open()
    copied = skipped = 0
    index, total = 0, 1
    try:
        while index < total:
            total, photo, error = fetch(port, index)
            if error:
                if total == 0:
                    print(f"写真がありません: {error}")
                    break
                raise RuntimeError(f"{index}枚目を読めません: {error}")
            name, data = photo
            path = out / f"{name}.JPG"
            if path.exists() and path.stat().st_size == len(data):
                skipped += 1
            else:
                tmp = path.with_suffix(".part")
                tmp.write_bytes(data)
                tmp.replace(path)
                copied += 1
            print(f"{index + 1}/{total} {name}.JPG {len(data)} bytes")
            index += 1
    finally:
        port.close()
    print(f"保存先 {out}  新しく保存 {copied} 枚、保存済み {skipped} 枚")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, TimeoutError, serial.SerialException) as exc:
        print(f"中止しました: {exc}", file=sys.stderr)
        sys.exit(1)
