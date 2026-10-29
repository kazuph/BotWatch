#!/usr/bin/env python3
"""Compare saved real microphone recordings with the published FP32 inference."""
import argparse
import json
from pathlib import Path
import sys

import soundfile as sf

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "reference"))
from inference import RalomiRecognizer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    args = parser.parse_args()
    recognizer = RalomiRecognizer(ROOT / "reference")
    summaries = []
    for path in sorted(args.session.glob("recording-*/comparison.json")):
        record = json.loads(path.read_text())
        samples, rate = sf.read(path.parent / "microphone.wav", dtype="float32")
        text = recognizer.transcribe(rate, samples)
        record["fp32_text"] = text
        path.write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n")
        summaries.append({"recording": path.parent.name, "microphone_text": record["microphone_text"], "fp32_text": text, "same_tokens_on_replay": record["identical_frames_and_tokens"]})
    if not summaries:
        raise ValueError("No recorded evidence")
    (args.session / "fp32-comparison.json").write_text(json.dumps(summaries, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(summaries, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
