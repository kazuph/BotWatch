#!/usr/bin/env python3
"""Capture all requested 48px view renderers from the device framebuffer."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--pcm", type=Path, required=True, help="Actual saved recognition PCM")
parser.add_argument("--seconds", type=float, required=True)
args = parser.parse_args()
args.output.mkdir(exist_ok=True)
results = []
for view in ["waiting", "recording", "result", "confirmation", "long"]:
    output = args.output / view
    command = [sys.executable, str(Path(__file__).with_name("screen_proof.py")), "--port", args.port,
               "--output", str(output), "--seconds", str(args.seconds)]
    if view == "long": command += ["--maximum-text"]
    else: command += ["--view", view]
    if view == "result": command += ["--pcm", str(args.pcm)]
    subprocess.run(command, check=True)
    proof = json.loads((output / "screen-proof.json").read_text())
    results.append({"view": view, "status": proof["status"], "pages": len(proof["pages"]),
                    "evidence": str(output / "screen-proof.json"),
                    "scope": "actual device renderer and scanout framebuffer; confirmation actions remain stage 4"})
(args.output / "all-screens.json").write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n")
print(json.dumps(results, ensure_ascii=False, indent=2))
