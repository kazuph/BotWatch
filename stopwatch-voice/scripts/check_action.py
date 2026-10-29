#!/usr/bin/env python3
"""Action mode check: send 16 kHz mono WAVs to the StopWatch as raw PCM ("RLMA") and record what
KoeAct-JP decoded and played, with a screen PNG per utterance. Nothing is played on a speaker.

uv run --with pyserial --with pillow scripts/check_action.py --port /dev/cu.usbmodem1101 \
  --output /absolute/path/to/evidence --wav a.wav [--wav b.wav ...] [--expect '<json>' ...]
"""
import argparse
import json
import struct
import time
import wave
from pathlib import Path

import serial
from PIL import Image


def connect(port, seconds=30):
    deadline = time.time() + seconds
    while True:
        try:
            return serial.Serial(port, 115200, timeout=0.5)
        except serial.SerialException:
            if time.time() > deadline:
                raise
            time.sleep(0.5)


def until(link, log, deadline, accept):
    if not hasattr(link, "received_lines"):
        link.received_lines = bytearray()
    pending = link.received_lines
    while time.time() < deadline:
        while b"\n" in pending:
            end = pending.index(b"\n") + 1
            line = pending[:end].decode(errors="replace").rstrip("\r\n")
            del pending[:end]
            if accept(line):
                return line
        raw = link.read(max(1, link.in_waiting))
        if raw:
            log.write(raw)
            pending.extend(raw)
    raise TimeoutError("device did not answer in time")


def info(link, log):
    link.reset_input_buffer()
    if hasattr(link, "received_lines"):
        link.received_lines.clear()
    link.write(b"INFO")
    lines = []
    deadline = time.time() + 15
    while True:
        line = until(link, log, deadline, lambda _: True)
        lines.append(line)
        if line.startswith("MEM serving"):
            return lines


def mode_of(lines):
    for line in lines:
        if line.startswith("MODE "):
            return line.split()[1]
    return "kana"  # firmware before the action mode reported no MODE line


def screenshot(link, log, path, tries=3):
    # The pixel rows go straight to USB, so a status line printed meanwhile can land inside a row:
    # take the screen again when one does not parse.
    for attempt in range(tries):
        try:
            return screenshot_once(link, log, path)
        except (ValueError, AssertionError, TimeoutError) as error:
            log.write(f"HOST_SCREEN_RETRY attempt={attempt + 1} reason={error}\n".encode())
            if attempt == tries - 1:
                raise
            # Finish the in-flight transfer before requesting another; clearing its
            # buffer midway can mix its remaining rows with the next capture.
            try:
                until(link, log, time.time() + 10, lambda line: line == "SCREEN_END")
            except TimeoutError:
                link.reset_input_buffer()
                link.received_lines.clear()


def screenshot_once(link, log, path):
    link.write(b"LFRM")
    deadline = time.time() + 60
    header = until(link, log, deadline, lambda line: line.startswith("SCREEN "))
    width, height = map(int, header.split()[1:3])
    pixels = bytearray()
    for y in range(height):
        row = until(link, log, deadline, lambda line: line.startswith("PIXELS ")).split()
        assert int(row[1]) == y, f"row {row[1]} instead of {y}"
        data = bytes.fromhex(row[2])
        assert len(data) == width * 3, f"truncated row {y}"
        pixels.extend(data)
    until(link, log, deadline, lambda line: line == "SCREEN_END")
    Image.frombytes("RGB", (width, height), bytes(pixels)).save(path)


def read_wav(path):
    with wave.open(str(path)) as w:
        assert w.getframerate() == 16000 and w.getnchannels() == 1 and w.getsampwidth() == 2, f"{path}: need 16 kHz mono int16"
        return w.readframes(w.getnframes())


def send_rlma(link, pcm):
    link.write(b"RLMA" + struct.pack("<IB", len(pcm) // 2, 0))
    # Match the 16kHz frontend's 160-sample hop instead of bursting an entire
    # utterance into USB while the device also renders and records microphone hops.
    hop_bytes = 160 * 2
    started = time.monotonic()
    for offset in range(0, len(pcm), hop_bytes):
        chunk = pcm[offset:offset + hop_bytes]
        link.write(chunk)
        remaining = (offset + len(chunk)) / (16000 * 2) - (time.monotonic() - started)
        if remaining > 0:
            time.sleep(remaining)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--wav", action="append", default=[], type=Path)
    parser.add_argument("--label", action="append", default=[], help="what each WAV says (for the report)")
    parser.add_argument("--expect", action="append", default=[], help="expected Action JSON per WAV (PC KoeAct-JP)")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    with (args.output / "serial.log").open("ab") as log:
        link = connect(args.port)
        lines = info(link, log)
        mode = mode_of(lines)
        if mode != "action":
            link.write(b"MODE")
            until(link, log, time.time() + 10, lambda line: line.startswith("MODE_SWITCH"))
            link.close()
            time.sleep(3)
            link = connect(args.port, 60)
            time.sleep(8)  # model load and microphone warm-up
            lines = info(link, log)
            mode = mode_of(lines)
        assert mode == "action", f"device stayed in {mode} mode"
        # TTS initialization resets RTC diagnostics after a hardware reset. Set
        # mute only after INFO confirms the recognizer and listener are serving.
        link.write(b"TMUT")
        until(link, log, time.time() + 15, lambda line: line == "TTS_DIAGNOSTIC muted=1 pcm=1")
        boot = [line for line in lines if line.startswith(("MEM ", "LOAD", "IO ", "A2 ", "BENCH", "MODE"))]
        screenshot(link, log, args.output / "00-waiting.png")
        # Room speech must not be processed while a clip streams in (USB buffers overflow then).
        link.write(b"LHLD")
        until(link, log, time.time() + 10, lambda line: line.startswith("LISTEN_HOLD 1"))
        for index, path in enumerate(args.wav, 1):
            pcm = read_wav(path)
            info(link, log)
            link.write(b"TMUT")
            until(link, log, time.time() + 15, lambda line: line == "TTS_DIAGNOSTIC muted=1 pcm=1")
            send_rlma(link, pcm)
            deadline = time.time() + 60
            koeact = until(link, log, deadline, lambda line: line.startswith("KOEACT "))
            decoded = until(link, log, deadline, lambda line: line.startswith("KOEACT_JSON")).split(" ", 1)
            kana = until(link, log, deadline, lambda line: line.startswith("KOEACT_KANA")).split(" ", 1)
            played = []
            done = until(link, log, deadline, lambda line: line.startswith(("ACTION_DONE", "ACTION_PLAYED")) or line.startswith("ERROR"))
            while done.startswith("ACTION_PLAYED"):
                played.append(done)
                done = until(link, log, deadline, lambda line: line.startswith(("ACTION_DONE", "ACTION_PLAYED")) or line.startswith("ERROR"))
            png = args.output / f"{index:02d}-{path.stem}.png"
            screenshot(link, log, png)
            fields = dict(item.split("=", 1) for item in koeact.split()[1:] if "=" in item)
            board_json = decoded[1] if len(decoded) > 1 else ""
            record = {
                "wav": str(path), "label": args.label[index - 1] if index <= len(args.label) else "",
                "board_json": board_json, "kana": kana[1] if len(kana) > 1 else "",
                "p_act": float(fields["p_act"]), "accepted": fields["accepted"] == "1",
                "total_us": int(fields["total_us"]), "played": played, "done": done, "png": str(png),
            }
            if index <= len(args.expect):
                expected = json.loads(args.expect[index - 1])
                shown = json.loads(board_json) if record["accepted"] and board_json else []
                record["expected"] = expected
                record["matches_pc"] = shown == expected
            results.append(record)
            print(json.dumps(record, ensure_ascii=False), flush=True)
        link.write(b"LRUN")
        until(link, log, time.time() + 10, lambda line: line.startswith("LISTEN_HOLD 0"))
        after = info(link, log)
    summary = {"boot": boot, "serving_memory": [line for line in after if line.startswith("MEM serving")], "results": results}
    if any("matches_pc" in r for r in results):
        summary["matches_pc"] = sum(r.get("matches_pc", False) for r in results)
        summary["compared"] = sum("matches_pc" in r for r in results)
    (args.output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2))
    print(json.dumps({k: v for k, v in summary.items() if k != "results"}, ensure_ascii=False))


if __name__ == "__main__":
    main()
