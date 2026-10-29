#!/usr/bin/env python3
"""Collect real serial evidence and compare microphone PCM with its RLMA replay.

Run with uv run --with pyserial scripts/capture.py ... . No audio playback.
"""
import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys
import time
import wave

import serial


def recognized_text(line):
    match = re.fullmatch(r"TEXT frames=\d+ feature_us=\d+ run_us=\d+(?: (.*))?", line)
    if not match:
        raise ValueError(f"Malformed TEXT line: {line}")
    return match.group(1) or ""


def until(port, log, deadline, predicate):
    pending = bytearray()
    while time.monotonic() < deadline:
        line = port.readline()
        if not line:
            continue
        log.write(line)
        log.flush()
        pending.extend(line)
        if not line.endswith(b"\n"):
            continue
        text = pending.decode("utf-8", errors="replace").strip()
        pending.clear()
        if text.startswith(("MEM ", "TEXT ", "MIC ", "ERROR ", "READY", "BOARD", "BUTTON", "TOUCH")):
            print(text, flush=True)
        if predicate(text):
            return text
    raise TimeoutError("Serial observation deadline exceeded; raw evidence retained")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--seconds", required=True, type=float, help="Host observation time limit")
    parser.add_argument("--mic", action="store_true", help="Request MIC; otherwise wait for a human tap")
    parser.add_argument("--info-only", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.seconds
    with serial.Serial(args.port, 115200, timeout=1, write_timeout=args.seconds) as port, (args.output / "serial.log").open("wb") as log:
        port.write(b"INFO")
        until(port, log, deadline, lambda s: s.startswith("MEM serving"))
        if args.info_only:
            return
        if args.mic:
            port.write(b"MIC")
        pcm_line = until(port, log, deadline, lambda s: s.startswith("PCM "))
        _, count, encoded = pcm_line.split(" ", 2)
        pcm = bytes.fromhex(encoded)
        if len(pcm) != int(count) * 2 or int(count) != 4 * 16000:
            raise ValueError(f"Incomplete or non-4-second PCM: {len(pcm)} bytes, {count} samples")
        (args.output / "microphone.pcm").write_bytes(pcm)
        with wave.open(str(args.output / "microphone.wav"), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(pcm)
        mic_result = until(port, log, deadline, lambda s: s.startswith("RESULT "))
        mic_text = until(port, log, deadline, lambda s: s.startswith("TEXT "))
        # dump=1 saves features for diagnosis if microphone/replay differ.
        port.write(b"RLMA" + struct.pack("<IB", int(count), 1) + pcm)
        feature_line = until(port, log, deadline, lambda s: s.startswith("FEATURES "))
        _, feature_count, feature_hex = feature_line.split(" ", 2)
        feature_bytes = bytes.fromhex(feature_hex)
        if len(feature_bytes) != int(feature_count) * 4:
            raise ValueError("Incomplete feature dump")
        (args.output / "replay-features.f32").write_bytes(feature_bytes)
        replay_result = until(port, log, deadline, lambda s: s.startswith("RESULT "))
        replay_text = until(port, log, deadline, lambda s: s.startswith("TEXT "))
        samples = array.array("h", pcm)
        if sys.byteorder != "little":
            samples.byteswap()
        leading = next((i for i, v in enumerate(samples) if v), len(samples))
        raw_lines = [line for line in (args.output / "serial.log").read_text().splitlines() if line.startswith("RAW_PCM ")]
        raw = bytes.fromhex(raw_lines[-1].split(" ", 2)[2]) if raw_lines else pcm
        if len(raw) != len(pcm):
            raise ValueError("Incomplete raw PCM")
        (args.output / "raw-microphone.pcm").write_bytes(raw)
        raw_samples = array.array("h", raw)
        if sys.byteorder != "little":
            raw_samples.byteswap()
        summary = {
            "samples": len(samples), "sample_rate": 16000, "duration_seconds": len(samples) / 16000,
            "pcm_sha256": hashlib.sha256(pcm).hexdigest(), "leading_exact_zero_samples": leading,
            "min": min(samples), "max": max(samples),
            # Mic_Class.inl clamps to INT16_MIN+16 / INT16_MAX-16.
            "clipped_samples": sum(v <= -32768 + 16 or v >= 32767 - 16 for v in raw_samples),
            "raw_min": min(raw_samples), "raw_max": max(raw_samples),
            "raw_pcm_sha256": hashlib.sha256(raw).hexdigest(),
            "pcm_role": "normalized inference PCM" if raw != pcm else "unmodified microphone PCM",
            "per_second_rms_dbfs": [10 * math.log10(sum((v / 32768) ** 2 for v in samples[i:i+16000]) / 16000 + 1e-12) for i in range(0, len(samples), 16000)],
            "microphone_result": mic_result, "microphone_text": mic_text,
            "replay_result": replay_result, "replay_text": replay_text,
            "identical_frames_and_tokens": mic_result.split()[1:2] + mic_result.split()[3:] == replay_result.split()[1:2] + replay_result.split()[3:],
            "identical_text": recognized_text(mic_text) == recognized_text(replay_text),
            "human_utterance": "NOT_RECORDED: write actual spoken words and cold/retry context separately",
        }
        (args.output / "comparison.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps(summary, ensure_ascii=False, indent=2))
        if not summary["identical_frames_and_tokens"] or not summary["identical_text"]:
            raise SystemExit("FAIL microphone versus RLMA; inspect dumped features")


if __name__ == "__main__":
    main()
