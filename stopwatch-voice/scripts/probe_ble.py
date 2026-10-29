#!/usr/bin/env python3
"""Measure the real NimBLE lifecycle with the model already loaded."""
import argparse
import json
from pathlib import Path
import time

import serial
from capture import until

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--seconds", type=float, required=True)
parser.add_argument("--query", action="store_true", help="Actually discover/connect/subscribe/query the physical printer")
args = parser.parse_args()
args.output.mkdir(exist_ok=True)
with serial.Serial(args.port, 115200, timeout=1, write_timeout=args.seconds) as port, (args.output / "serial.log").open("wb") as log:
    deadline = time.monotonic() + args.seconds
    port.write(b"INFO")
    until(port, log, deadline, lambda s: s.startswith("MEM serving"))
    port.write(b"BLEQ" if args.query else b"BLEI")
    result = until(port, log, deadline, lambda s: s.startswith("BLE_QUERY " if args.query else "BLE_PROBE "))
lines = (args.output / "serial.log").read_text(errors="replace").splitlines()
summary = {"result": result, "memory": [line for line in lines if line.startswith("MEM ")],
           "lifecycle": [line for line in lines if line.startswith(("BLE_INIT", "BLE_STOP", "BLE_DEINIT", "BLE_ERROR"))],
           "connection_send_cycles": 1 if args.query and result == "BLE_QUERY PASS" else "NOT_MET"}
(args.output / "comparison.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
print(json.dumps(summary, ensure_ascii=False, indent=2))
if result != ("BLE_QUERY PASS" if args.query else "BLE_PROBE PASS"):
    raise SystemExit("FAIL actual controller/host lifecycle; raw evidence retained")
