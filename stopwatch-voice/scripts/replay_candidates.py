#!/usr/bin/env python3
"""Check a measured preprocessing candidate against the unchanged board model."""
import argparse
import json
from pathlib import Path
import struct
import time

import serial
from capture import recognized_text, until

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--study", type=Path, required=True)
parser.add_argument("--candidate", required=True)
parser.add_argument("--port", required=True)
parser.add_argument("--seconds", type=float, required=True)
args = parser.parse_args()
candidate = next(row for row in json.loads(args.study.read_text()) if row["candidate"] == args.candidate)
results = []
with serial.Serial(args.port, 115200, timeout=1, write_timeout=args.seconds) as port:
    for record in candidate["records"]:
        pcm = Path(record["pcm"]).read_bytes()
        with Path(record["pcm"]).with_suffix(".replay.log").open("wb") as log:
            deadline = time.monotonic() + args.seconds
            port.write(b"INFO")
            until(port, log, deadline, lambda line: line.startswith("MEM serving"))
            port.write(b"RLMA" + struct.pack("<IB", len(pcm) // 2, 0) + pcm)
            result = until(port, log, deadline, lambda line: line.startswith("RESULT "))
            text = recognized_text(until(port, log, deadline, lambda line: line.startswith("TEXT ")))
        results.append({"pcm": record["pcm"], "fp32_text": record["text"], "board_text": text,
                        "same_text": text == record["text"], "result": result})
        print(json.dumps(results[-1], ensure_ascii=False), flush=True)
output = args.study.parent / args.candidate / "board-comparison.json"
output.write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n")
if not all(row["same_text"] for row in results):
    raise SystemExit("Quantized and FP32 differences retained in board-comparison.json")
