#!/usr/bin/env python3
"""Voice printing check: feed spoken steps (WAV, in room-like noise) to the board's listener in
place of the microphone ("LPCM") and follow the flow across the reboot into the kana mode.
Nothing is played on a speaker. With --print the last step really prints one label.

uv run --with pyserial --with numpy --with pillow scripts/check_voice_print.py --port /dev/cu.usbmodem1101 \
  --output /absolute/path/to/evidence --step start.wav=MODE_SWITCH --step name.wav=voice_print_confirm ...
Each --step is <wav>=<line prefix or UI state the board must print next>.
"""
import argparse
import json
import queue
import struct
import threading
import time
import wave
from pathlib import Path

import numpy as np
import serial
from PIL import Image

RATE = 16000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--step", action="append", default=[])
    parser.add_argument("--noise-dbfs", type=float, default=-60)
    parser.add_argument("--speech-dbfs", type=float, default=-30)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    log = (args.output / "serial.log").open("ab")
    lines = queue.Queue()
    state = {"link": None}

    def connect(seconds=60):
        end = time.time() + seconds
        while True:
            try:
                state["link"] = serial.Serial(args.port, 115200, timeout=0.5)
                return
            except serial.SerialException:
                if time.time() > end:
                    raise
                time.sleep(0.5)

    def reader():
        pending = bytearray()
        while True:
            try:
                raw = state["link"].readline()
            except (serial.SerialException, OSError, TypeError):
                pending.clear()
                time.sleep(0.5)
                continue
            if raw:
                log.write(raw)
                pending.extend(raw)
                if raw.endswith(b"\n"):
                    lines.put(pending.decode(errors="replace").rstrip("\r\n"))
                    pending.clear()

    seen = []  # every line taken this step, so waiting for an ACK cannot swallow the marker

    def until(accept, seconds=60):
        end = time.time() + seconds
        while time.time() < end:
            try:
                line = lines.get(timeout=0.2)
            except queue.Empty:
                continue
            seen.append(line)
            if accept(line):
                return line
        raise TimeoutError

    def screenshot(path):
        for _ in range(3):
            try:
                state["link"].write(b"LFRM")
                width, height = map(int, until(lambda l: l.startswith("SCREEN ")).split()[1:3])
                pixels = bytearray()
                for y in range(height):
                    row = until(lambda l: l.startswith("PIXELS ")).split()
                    assert int(row[1]) == y
                    pixels.extend(bytes.fromhex(row[2]))
                until(lambda l: l == "SCREEN_END")
                Image.frombytes("RGB", (width, height), bytes(pixels)).save(path)
                return True
            except (ValueError, AssertionError, TimeoutError):
                time.sleep(1)
                while not lines.empty():
                    lines.get()
        return False

    def utterance(path):
        with wave.open(str(path)) as w:
            x = np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768
        rng = np.random.default_rng(len(x))
        noise = 10 ** (args.noise_dbfs / 20)
        x = x * 10 ** ((args.speech_dbfs - 20 * np.log10(np.sqrt(np.mean(x ** 2)))) / 20)
        y = np.concatenate([rng.normal(0, noise, RATE), x + rng.normal(0, noise, len(x)), rng.normal(0, noise, 2 * RATE)])
        return (np.clip(y, -1, 1) * 32767).astype("<i2").tobytes()

    def hold():
        # The board hears the room all the time; only the fed words may drive the test.
        state["link"].write(b"LHLD")
        try:
            until(lambda l: l.startswith("LISTEN_HOLD 1"), 15)
        except TimeoutError:
            print("WARN no LISTEN_HOLD", flush=True)

    def mute():
        state["link"].write(b"INFO")
        until(lambda l: l.startswith("MEM serving"), 60)
        state["link"].write(b"TMUT")
        until(lambda l: l == "TTS_DIAGNOSTIC muted=1 pcm=1", 15)

    connect()
    threading.Thread(target=reader, daemon=True).start()
    mute()
    hold()
    results = []
    for index, spec in enumerate(args.step, 1):
        wav, expect = spec.split("=", 1)
        data = utterance(Path(wav))
        while not lines.empty():
            lines.get()
        seen.clear()
        mute()
        link = state["link"]
        link.write(b"LPCM" + struct.pack("<I", len(data) // 2))
        got, reboot = [], False
        for offset in range(0, len(data), RATE):
            try:
                link.write(data[offset:offset + RATE])
            except (serial.SerialException, OSError):
                reboot = True
                break
            if offset + RATE < len(data):
                try:
                    got.append(until(lambda l: l.startswith(("LPCM_ACK", "MODE_SWITCH")), 60))
                except TimeoutError:
                    break
                if got[-1].startswith("MODE_SWITCH"):
                    reboot = True
                    break
        matched = next((l for l in seen if expect in l), None)
        if matched is None and not reboot:
            try:
                matched = until(lambda l: expect in l, 90)
            except TimeoutError:
                matched = None
            reboot = any(l.startswith("MODE_SWITCH") for l in seen)
        heard = [l for l in seen if l.startswith(("VOICE_HEARD", "KOEACT_KANA", "VOICE_PRINT", "BLE_PRINT"))]
        seen.clear()
        record = {"step": index, "wav": wav, "expect": expect, "matched": matched, "reboot": reboot, "heard": heard}
        if reboot:
            # the board restarts: reconnect and wait until it is listening again
            try:
                link.close()
            except Exception:
                pass
            time.sleep(4)
            connect()
            try:
                record["after_boot"] = until(lambda l: l.startswith(("UI_STATE", "I (")) and "READY" in l or l.startswith("UI_STATE"), 60)
            except TimeoutError:
                record["after_boot"] = None
            time.sleep(2)
            hold()
        png = args.output / f"{index:02d}-{Path(wav).stem}.png"
        record["png"] = str(png) if screenshot(png) else "screenshot failed"
        results.append(record)
        print(json.dumps(record, ensure_ascii=False), flush=True)
    state["link"].write(b"LRUN")  # leave the board listening to the room again
    (args.output / "summary.json").write_text(json.dumps(results, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
