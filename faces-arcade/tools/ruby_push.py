#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["bleak==0.22.3"]
# ///
"""Compile a Ruby game with mruby 4's mrbc and send it to FACES ARCADE."""
import argparse
import asyncio
import codecs
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import zlib

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

SERVICE = "47e40001-9b5d-48d0-b54a-627af022e548"
WRITE = "47e40002-9b5d-48d0-b54a-627af022e548"
NOTIFY = "47e40003-9b5d-48d0-b54a-627af022e548"


def compile_game(path: Path) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        output = Path(tmp) / "game.mrb"
        try:
            result = subprocess.run(["mrbc", "-o", str(output), str(path)], capture_output=True, text=True)
        except FileNotFoundError as exc:
            raise RuntimeError("mruby 4.0.0 の mrbc が PATH にありません") from exc
        if result.returncode:
            raise RuntimeError(result.stderr or result.stdout or "mrbc failed")
        bytecode = output.read_bytes()
        if bytecode[:8] != b"RITE0400":
            raise RuntimeError("mrbc の版が不一致です。mruby 4.0.0 を使ってください")
        return bytecode


async def wait_status(queue: asyncio.Queue, expected: str):
    while True:
        message = await asyncio.wait_for(queue.get(), 30)
        if message.startswith("error "):
            raise RuntimeError(message)
        if message == expected:
            return


async def send(client: BleakClient, queue: asyncio.Queue, bytecode: bytes):
    while not queue.empty():
        queue.get_nowait()
    start = struct.pack("<BII", 1, len(bytecode), zlib.crc32(bytecode))
    await client.write_gatt_char(WRITE, start, response=True)
    await wait_status(queue, "ready")
    char = client.services.get_characteristic(WRITE)
    chunk_size = min(500, max(1, char.max_write_without_response_size - 1), max(1, client.mtu_size - 4))
    for offset in range(0, len(bytecode), chunk_size):
        chunk = bytecode[offset:offset + chunk_size]
        await client.write_gatt_char(WRITE, b"\x02" + chunk, response=True)
        await wait_status(queue, f"recv {offset + len(chunk)}")
    await client.write_gatt_char(WRITE, b"\x03", response=True)
    await wait_status(queue, "running")


async def run(path: Path, watch: bool):
    started = time.monotonic()
    try:
        bytecode = compile_game(path)
    except RuntimeError as exc:
        print(exc, file=sys.stderr, flush=True)
        if not watch:
            return 1
        bytecode = None
    timestamp = path.stat().st_mtime_ns
    while True:
        print("Ruby の Bluetooth を探しています…", flush=True)
        device = await BleakScanner.find_device_by_filter(
            lambda _device, adv: SERVICE.lower() in [s.lower() for s in adv.service_uuids], timeout=5)
        if not device:
            print("本体をメニューか Ruby の画面にしてください", file=sys.stderr, flush=True)
            if not watch:
                return 1
            await asyncio.sleep(3)
            continue
        queue = asyncio.Queue()
        try:
            async with BleakClient(device) as client:
                decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
                status_parts = bytearray()
                def notification(_sender, data):
                    if len(data) < 2:
                        return
                    kind, last = data[:2]
                    if kind == 2:
                        print(decoder.decode(bytes(data[2:])), end="", flush=True)
                    elif kind == 1:
                        status_parts.extend(data[2:])
                        if last:
                            message = status_parts.decode("utf-8", errors="replace")
                            status_parts.clear()
                            print(message, flush=True)
                            queue.put_nowait(message)
                await client.start_notify(NOTIFY, notification)
                current = path.stat().st_mtime_ns
                if current != timestamp or bytecode is None:
                    timestamp = current
                    try:
                        bytecode = compile_game(path)
                    except RuntimeError as exc:
                        print(exc, file=sys.stderr, flush=True)
                        bytecode = None
                if bytecode is not None:
                    await send(client, queue, bytecode)
                    print(f"変換開始から running まで {time.monotonic() - started:.3f} 秒", flush=True)
                if not watch:
                    return 0
                while client.is_connected:
                    await asyncio.sleep(0.1)
                    current = path.stat().st_mtime_ns
                    if current == timestamp:
                        continue
                    timestamp = current
                    saved_at = current / 1_000_000_000
                    try:
                        bytecode = compile_game(path)
                        await send(client, queue, bytecode)
                        print(f"保存から running まで {time.time() - saved_at:.3f} 秒", flush=True)
                    except (RuntimeError, asyncio.TimeoutError) as exc:
                        bytecode = None
                        print(f"送信できません: {exc}", file=sys.stderr, flush=True)
        except (BleakError, asyncio.TimeoutError) as exc:
            if not watch:
                raise
            print(f"接続が切れました: {exc}", file=sys.stderr, flush=True)
        if not watch:
            return 1
        bytecode = None
        print("本体をメニューか Ruby の画面にしてください", file=sys.stderr, flush=True)
        await asyncio.sleep(3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--watch", action="store_true")
    parser.add_argument("game", type=Path)
    args = parser.parse_args()
    try:
        return asyncio.run(run(args.game, args.watch))
    except (OSError, RuntimeError, BleakError, asyncio.TimeoutError) as exc:
        print(f"エラー: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
