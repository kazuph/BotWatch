#!/usr/bin/env python3
"""Replay saved raw PCM at the real input cadence with one-second gain chunks."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time

import serial
from capture import recognized_text, until

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--session", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--port", required=True)
parser.add_argument("--seconds", type=float, required=True)
args = parser.parse_args()
args.output.mkdir(exist_ok=True)
results = []
with serial.Serial(args.port, 115200, timeout=1, write_timeout=args.seconds) as port:
    for path in sorted(args.session.glob("recording-*/comparison.json")):
        record = json.loads(path.read_text())
        raw = (path.parent / "raw-microphone.pcm").read_bytes()
        if len(raw) != 4 * 16000 * 2:
            raise ValueError("Stream diagnostic requires exactly four seconds")
        with (args.output / f"{path.parent.name}.log").open("wb") as log:
            deadline = time.monotonic() + args.seconds
            port.write(b"INFO")
            until(port, log, deadline, lambda s: s.startswith("MEM serving"))
            port.write(b"RLMS" + raw)
            raw_line = until(port, log, deadline, lambda s: s.startswith("RAW_PCM "))
            assert bytes.fromhex(raw_line.split(" ", 2)[2]) == raw
            pcm_line = until(port, log, deadline, lambda s: s.startswith("PCM "))
            pcm = bytes.fromhex(pcm_line.split(" ", 2)[2])
            mic = until(port, log, deadline, lambda s: s.startswith("MIC "))
            result = until(port, log, deadline, lambda s: s.startswith("RESULT "))
            text = until(port, log, deadline, lambda s: s.startswith("TEXT "))
        tail = int(re.search(r"tail_us=(\d+)", mic)[1])
        actual = {"recording": path.parent.name, "source_raw_pcm": str(path.parent / "raw-microphone.pcm"),
                  "pcm_sha256": hashlib.sha256(pcm).hexdigest(),
                  "identical_normalized_pcm": pcm == (path.parent / "microphone.pcm").read_bytes(),
                  "identical_frames_and_tokens": result.split()[1:2] + result.split()[3:] == record["microphone_result"].split()[1:2] + record["microphone_result"].split()[3:],
                  "identical_text": recognized_text(text) == recognized_text(record["microphone_text"]),
                  "text": recognized_text(text), "tail_us_including_screen": tail,
                  "tail_within_requested_500ms": tail <= 500000,
                  "source": "saved PCM made available every 500ms, one-second normalization, same processor as microphone"}
        results.append(actual)
        (args.output / "comparison.json").write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps(actual, ensure_ascii=False), flush=True)
if not results or not all(r["identical_normalized_pcm"] and r["identical_frames_and_tokens"] and r["identical_text"] and r["tail_within_requested_500ms"] for r in results):
    raise SystemExit("FAIL streaming equality or requested latency; evidence retained")
