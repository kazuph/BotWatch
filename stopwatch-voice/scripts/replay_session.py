#!/usr/bin/env python3
"""Replay each complete human microphone recording after the monitor is stopped."""
import argparse
import array
import hashlib
import json
from pathlib import Path
import struct
import sys
import time
import wave

import serial

from capture import recognized_text, until


def recordings(path):
    pending = None
    raw_pcm = None
    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("RAW_PCM "):
            _, samples, data = line.split(" ", 2)
            raw_pcm = bytes.fromhex(data)
            if len(raw_pcm) != int(samples) * 2:
                raise ValueError("Truncated raw microphone PCM")
        elif line.startswith("PCM "):
            _, samples, data = line.split(" ", 2)
            pending = {"samples": int(samples), "pcm": bytes.fromhex(data)}
            if raw_pcm is not None:
                pending["raw_pcm"] = raw_pcm
                raw_pcm = None
            if len(pending["pcm"]) != pending["samples"] * 2:
                raise ValueError("Truncated microphone PCM")
        elif pending and line.startswith("RESULT "):
            pending["microphone_result"] = line
        elif pending and line.startswith("TEXT "):
            pending["microphone_text"] = line
            if "microphone_result" not in pending:
                raise ValueError("Missing microphone RESULT")
            yield pending
            pending = None
    if pending:
        raise ValueError("Incomplete final recording")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", type=Path, required=True)
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=float, required=True)
    args = parser.parse_args()
    entries = list(recordings(args.session / "serial.log"))
    if not entries:
        raise ValueError("No complete microphone recordings")
    passed = True
    with serial.Serial(args.port, 115200, timeout=1, write_timeout=args.seconds) as port:
        for index, entry in enumerate(entries, 1):
            target = args.session / f"recording-{index}"
            target.mkdir(exist_ok=True)
            pcm = entry.pop("pcm")
            raw = entry.pop("raw_pcm", pcm)
            (target / "raw-microphone.pcm").write_bytes(raw)
            if entry["samples"] != 4 * 16000:
                raise ValueError("Recording is not exactly four seconds")
            (target / "microphone.pcm").write_bytes(pcm)
            with wave.open(str(target / "microphone.wav"), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(16000)
                wav.writeframes(pcm)
            with (target / "replay.log").open("wb") as log:
                deadline = time.monotonic() + args.seconds
                # Do not overrun the USB receive ring while a final touch is recording.
                port.write(b"INFO")
                until(port, log, deadline, lambda line: line.startswith("MEM serving"))
                port.write(b"RLMA" + struct.pack("<IB", entry["samples"], 1) + pcm)
                features = until(port, log, deadline, lambda line: line.startswith("FEATURES ")).split(" ", 2)
                feature_bytes = bytes.fromhex(features[2])
                if len(feature_bytes) != int(features[1]) * 4:
                    raise ValueError("Truncated replay feature dump")
                (target / "replay-features.f32").write_bytes(feature_bytes)
                replay_result = until(port, log, deadline, lambda line: line.startswith("RESULT "))
                replay_text = until(port, log, deadline, lambda line: line.startswith("TEXT "))
            samples = array.array("h", pcm)
            if sys.byteorder != "little":
                samples.byteswap()
            entry.update({
                "pcm_sha256": hashlib.sha256(pcm).hexdigest(), "duration_seconds": len(samples) / 16000,
                "leading_exact_zero_samples": next((i for i, value in enumerate(samples) if value), len(samples)),
                "clipped_samples": sum(value <= -32768 + 16 or value >= 32767 - 16 for (value,) in struct.iter_unpack("<h", raw)),
                "raw_pcm_sha256": hashlib.sha256(raw).hexdigest(),
                "pcm_role": "normalized inference PCM" if raw != pcm else "unmodified microphone PCM",
                "min": min(samples), "max": max(samples),
                "replay_result": replay_result, "replay_text": replay_text,
                "identical_frames_and_tokens": entry["microphone_result"].split()[1:2] + entry["microphone_result"].split()[3:] == replay_result.split()[1:2] + replay_result.split()[3:],
                "identical_text": recognized_text(entry["microphone_text"]) == recognized_text(replay_text),
            })
            (target / "comparison.json").write_text(json.dumps(entry, ensure_ascii=False, indent=2) + "\n")
            print(json.dumps({"recording": index, **entry}, ensure_ascii=False), flush=True)
            passed &= entry["identical_frames_and_tokens"] and entry["identical_text"]
    if not passed:
        raise SystemExit("FAIL microphone versus RLMA; use saved features for diagnosis")


if __name__ == "__main__":
    main()
