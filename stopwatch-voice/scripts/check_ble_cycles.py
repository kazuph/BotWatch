#!/usr/bin/env python3
"""Run USB cycles, arm battery cycles, or retrieve their preserved PSRAM trace."""
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
parser.add_argument("--operation", choices=["run", "arm", "dump"], required=True)
parser.add_argument("--power", choices=["usb", "battery"])
args = parser.parse_args()
args.output.mkdir(exist_ok=True)
if args.operation != "arm" and not args.power:
    parser.error("run/dump require the expected physical power source")
with serial.Serial(args.port, 115200, timeout=1, write_timeout=args.seconds) as port, (args.output / "serial.log").open("wb") as log:
    deadline = time.monotonic() + args.seconds
    port.write(b"INFO")
    until(port, log, deadline, lambda s: s.startswith("MEM serving"))
    port.write({"run": b"BLET", "arm": b"BARM", "dump": b"BLED"}[args.operation])
    if args.operation == "arm":
        until(port, log, deadline, lambda s: s == "BATTERY_TEST ARMED")
        print("ARMED: battery test has not run; human must unplug USB and press A")
        raise SystemExit(0)
    if args.operation == "dump":
        until(port, log, deadline, lambda s: s == "CYCLE_TRACE_BEGIN")
        ending = until(port, log, deadline, lambda s: s == "CYCLE_TRACE_END" or s.startswith("ERROR "))
        if ending != "CYCLE_TRACE_END":
            raise SystemExit(f"FAIL retained trace transfer: {ending}; raw evidence retained")
    else:
        until(port, log, deadline, lambda s: s.startswith("CYCLES_END "))


def fields(line):
    return dict(field.split("=", 1) for field in line.split() if "=" in field)


lines = (args.output / "serial.log").read_text(errors="replace").splitlines()
begin = next(i for i, line in enumerate(lines) if line.startswith("CYCLES_BEGIN "))
lines = lines[begin:]
records = []
phases = []
expected_phases = ["cycle_model_loaded", "recording", "after_recording", "ble_before_init", "ble_after_init",
                   "ble_started", "ble_connected", "ble_sent", "ble_stopped", "recording", "after_recording"]
for line in lines:
    if line.startswith("MEM "):
        phases.append({"stage": line.split()[1], **{key: int(value) for key, value in fields(line).items()}})
    elif line.startswith("CYCLE index="):
        record = fields(line)
        record["phases"] = phases
        phases = []
        records.append(record)
end = fields(next(line for line in lines if line.startswith("CYCLES_END ")))
minima = [int(record["minimum"]) for record in records]
declines = [right < left for left, right in zip(minima, minima[1:])]
continued_decline = any(left and right for left, right in zip(declines, declines[1:]))
power = "1" if args.power == "usb" else "0"
passed = (len(records) == 10 and end["completed"] == end["required"] == "10" and end["trace_overflow"] == "0"
          and not continued_decline and fields(lines[0])["usb"] == power
          and all(record["status"] == "PASS" and record["usb"] == power
                  and [phase["stage"] for phase in record["phases"]] == expected_phases for record in records))
summary = {"status": "PASS" if passed else "NOT_MET", "physical_power_source": args.power,
           "completed": end["completed"], "required": 10, "minima_per_cycle": minima,
           "continued_minimum_decline": continued_decline,
           "end_free_per_cycle": [int(record["end_free"]) for record in records],
           "largest_free_per_cycle": [int(record["end_largest"]) for record in records], "cycles": records}
(args.output / "comparison.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))
if not passed:
    raise SystemExit("FAIL actual cycles, power source, retained trace or memory trend; do not retry silently")
