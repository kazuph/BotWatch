#!/usr/bin/env python3
"""Always-listening check (action mode): feed a continuous stream of room-like noise with spoken
commands to the listener ("LPCM", in place of the microphone) and check that each command is cut
out and acted on, then repeat for a soak and record the memory after every round.
Nothing is played on a speaker.

uv run --with pyserial --with numpy scripts/check_listen.py --port /dev/cu.usbmodem1101 \
  --output /absolute/path/to/evidence --wav a.wav --expect '<json>' [...] --rounds 10 --idle-seconds 120
"""
import argparse
import json
import queue
import re
import struct
import threading
import time
import wave
from pathlib import Path

import numpy as np
import serial

RATE = 16000


def read_wav(path):
    with wave.open(str(path)) as w:
        assert w.getframerate() == RATE and w.getnchannels() == 1 and w.getsampwidth() == 2
        return np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float64) / 32768


def stream(clips, speech_dbfs, noise_dbfs, gap_seconds, seed):
    """Noise for gap_seconds, then each clip at speech_dbfs RMS over the same noise, and so on."""
    rng = np.random.default_rng(seed)
    noise_rms = 10 ** (noise_dbfs / 20)
    parts = []
    for clip in clips:
        parts.append(rng.normal(0, noise_rms, int(gap_seconds * RATE)))
        speech = clip * 10 ** ((speech_dbfs - 20 * np.log10(np.sqrt(np.mean(clip ** 2)) + 1e-12)) / 20)
        parts.append(speech + rng.normal(0, noise_rms, len(speech)))
    parts.append(rng.normal(0, noise_rms, int(gap_seconds * RATE)))
    return (np.clip(np.concatenate(parts), -1, 1) * 32767).astype("<i2").tobytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--wav", action="append", default=[], type=Path)
    parser.add_argument("--expect", action="append", default=[])
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--idle-seconds", type=int, default=60, help="microphone only, before the feed")
    parser.add_argument("--speech-dbfs", type=float, default=-30)
    parser.add_argument("--noise-dbfs", type=float, default=-55)
    parser.add_argument("--gap-seconds", type=float, default=1.5)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    clips = [read_wav(p) for p in args.wav]
    expected = [json.loads(e) for e in args.expect]
    link = serial.Serial(args.port, 115200, timeout=0.5)
    log = (args.output / "serial.log").open("ab", buffering=0)
    # A reader thread drains the board all the time: while the feed is being written the board
    # keeps printing, and neither side may block on the other.
    incoming = queue.Queue()

    def reader():
        pending = bytearray()
        while True:
            raw = link.readline()
            if raw:
                log.write(raw)
                pending.extend(raw)
                if raw.endswith(b"\n"):
                    incoming.put(pending.decode(errors="replace").rstrip("\r\n"))
                    pending.clear()

    threading.Thread(target=reader, daemon=True).start()

    def lines_until(stop, seconds):
        out, end = [], time.time() + seconds
        while time.time() < end:
            try:
                line = incoming.get(timeout=0.2)
            except queue.Empty:
                continue
            out.append(line)
            if stop and line.startswith(stop):
                break
        return out

    def stat():
        link.write(b"LSTA")
        got = lines_until("MEM listening", 70)
        return [l for l in got if l.startswith(("LISTEN_STAT", "MEM listening"))]

    def mute():
        link.write(b"INFO")
        boot = lines_until("MEM serving", 30)
        assert boot and boot[-1].startswith("MEM serving"), "boot completion not confirmed"
        link.write(b"TMUT")
        response = lines_until("TTS_DIAGNOSTIC", 15)
        assert response and response[-1] == "TTS_DIAGNOSTIC muted=1 pcm=1", "mute not confirmed; no injection"

    report = {"idle": {}, "rounds": []}
    mute()
    # 1) the room only: what the microphone path cuts and whether anything fires
    stat()
    idle = lines_until(None, args.idle_seconds)
    report["idle"] = {
        "seconds": args.idle_seconds,
        "segments": [l for l in idle if l.startswith(("LISTEN_SEG", "LISTEN_IGNORED", "ACTION_DONE"))],
        "after": stat(),
    }
    print(json.dumps(report["idle"], ensure_ascii=False), flush=True)
    # 2) commands in noise, repeated for the soak
    for round_index in range(args.rounds):
        data = stream(clips, args.speech_dbfs, args.noise_dbfs, args.gap_seconds, round_index)
        mute()
        link.write(b"LPCM" + struct.pack("<I", len(data) // 2))
        started = time.time()
        got = []
        chunk = RATE  # bytes = 0.5 s; the board acknowledges each before the next is sent
        for offset in range(0, len(data), chunk):
            link.write(data[offset:offset + chunk])
            if offset + chunk < len(data):
                got += lines_until("LPCM_ACK", 60)
        got += lines_until("LPCM_END", 60)
        decided = []
        for line in got:
            if line.startswith("KOEACT_JSON"):
                decided.append({"json": line.split(" ", 1)[1] if " " in line else ""})
            elif line.startswith("KOEACT ") and decided is not None:
                accepted = re.search(r"accepted=(\d)", line).group(1) == "1"
                decided.append({"accepted": accepted, "p_act": float(re.search(r"p_act=([\d.]+)", line).group(1))})
        # pair KOEACT (accepted) with the KOEACT_JSON after it
        results = []
        for i in range(0, len(decided) - 1, 2):
            results.append({**decided[i], **decided[i + 1]})
        shown = [json.loads(r["json"]) if r["accepted"] and r["json"] else [] for r in results]
        matches = sum(a == b for a, b in zip(shown, expected)) if len(shown) == len(expected) else None
        memory = stat()
        record = {
            "round": round_index + 1, "seconds": round(time.time() - started, 1),
            "segments": sum(l.startswith("LISTEN_SEG") for l in got), "expected_segments": len(clips),
            "matches": matches, "results": results, "memory": memory,
        }
        report["rounds"].append(record)
        print(json.dumps({k: v for k, v in record.items() if k != "results"}, ensure_ascii=False), flush=True)
    (args.output / "summary.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
