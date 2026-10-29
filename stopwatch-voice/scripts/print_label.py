#!/usr/bin/env python3
"""Recognize saved real PCM and explicitly print the expected recognition once."""
import argparse
import json
from pathlib import Path
import struct
import time

import serial
from capture import recognized_text, until

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", required=True)
parser.add_argument("--pcm", type=Path, required=True)
parser.add_argument("--expected-text", required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--seconds", type=float, required=True)
args = parser.parse_args()
args.output.mkdir(exist_ok=True)
deadline = time.monotonic() + args.seconds
with serial.Serial(args.port, 115200, timeout=1, write_timeout=args.seconds) as port, (args.output / "serial.log").open("wb") as log:
    port.write(b"INFO")
    until(port, log, deadline, lambda s: s.startswith("MEM serving"))
    pcm = args.pcm.read_bytes()
    port.write(b"RLMA" + struct.pack("<IB", len(pcm) // 2, 0) + pcm)
    text = recognized_text(until(port, log, deadline, lambda s: s.startswith("TEXT ")))
    if text != args.expected_text:
        raise SystemExit(f"NOT PRINTED: expected {args.expected_text!r}, recognized {text!r}")
    port.write(b"PRNT")
    label = until(port, log, deadline, lambda s: s.startswith(("LABEL ", "ERROR ")))
    if label.startswith("ERROR "):
        raise SystemExit(label)
    until(port, log, deadline, lambda s: s.startswith("LABEL_TEXT "))
    raster = bytes.fromhex(until(port, log, deadline, lambda s: s.startswith("RASTER ")).split()[1])
    (args.output / "raster.bin").write_bytes(raster)
    job = bytes.fromhex(until(port, log, deadline, lambda s: s.startswith(("PRINT_JOB ", "ERROR "))).split()[1])
    (args.output / "print-job.bin").write_bytes(job)
    result = until(port, log, deadline, lambda s: s.startswith("BLE_PRINT "))
    fields = dict(part.split("=", 1) for part in result.split()[1:])
    passed = fields["success"] == "1" and fields["completion"] == "aa" and fields["sent"] == fields["total"]
    evidence = {"status": "PASS" if passed else "NOT_MET", "text": text, "label": label,
                "result": result, "physical_paper_confirmation": "NOT_MET: human review required"}
    (args.output / "print-result.json").write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(evidence, ensure_ascii=False), flush=True)
    if not passed:
        raise SystemExit("Print not confirmed by AA; do not retry automatically")
