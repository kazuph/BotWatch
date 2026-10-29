#!/usr/bin/env python3
"""Compare gains and peak normalization on six saved PCMs; no new-recording claim."""
import argparse
import json
import hashlib
from pathlib import Path
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "reference"))
from inference import RalomiRecognizer

# Mic_Class.inl saturation rails. Existing clipped input cannot be repaired by scaling.
LOW, HIGH = -32768 + 16, 32767 - 16


def distance(left, right):
    row = list(range(len(right) + 1))
    for i, a in enumerate(left, 1):
        next_row = [i]
        for j, b in enumerate(right, 1):
            next_row.append(min(next_row[-1] + 1, row[j] + 1, row[j - 1] + (a != b)))
        row = next_row
    return row[-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old-session", type=Path, required=True)
    parser.add_argument("--unity-session", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(exist_ok=True)
    recognizer = RalomiRecognizer(ROOT / "reference")
    sources = []
    for session, gain, count in [(args.old_session, 16, 4), (args.unity_session, 1, 2)]:
        paths = sorted(session.glob("recording-*/microphone.pcm"))
        if len(paths) != count:
            raise ValueError("Expected four old and two unity recordings")
        for path in paths:
            raw = np.frombuffer(path.read_bytes(), dtype="<i2").astype(np.float64)
            if len(raw) != 64000:
                raise ValueError("Expected exactly four seconds")
            baseline = json.loads((path.parent / "comparison.json").read_text())
            # Only the latest two utterances have unambiguous human-specified labels.
            sources.append((path, gain, "やまだたろう" if gain == 1 else None, raw, baseline))
    summaries = []
    for candidate in [f"gain-{gain}" for gain in range(1, 17)] + ["trim-zero-peak", "chunk-peak", "chunk-peak-cap16"]:
        records = []
        candidate_dir = args.output / candidate
        candidate_dir.mkdir(exist_ok=True)
        for index, (path, original_gain, expected, raw, baseline) in enumerate(sources, 1):
            nonzero = np.flatnonzero(raw)
            leading = int(nonzero[0]) if len(nonzero) else len(raw)
            if candidate.startswith("chunk-peak"):
                trimmed = np.pad(raw[leading:], (0, leading))
                values = np.zeros_like(trimmed)
                scale = []
                # One stream100 model chunk spans 100 hops of 10 ms.
                config = json.loads((ROOT / "reference/inference_config.json").read_text())
                chunk_samples = int(100 * config["features"]["hop_ms"] * 16000 / 1000)
                for first in range(0, len(trimmed), chunk_samples):
                    part = trimmed[first:first + chunk_samples]
                    factor = min(HIGH / max(1, float(part.max())), LOW / min(-1, float(part.min())))
                    if candidate == "chunk-peak-cap16":
                        # Retain the original library's 16x gain ceiling, avoiding
                        # unlimited amplification of quiet/background-only chunks.
                        factor = min(factor, 16 / original_gain)
                    scale.append(factor)
                    values[first:first + chunk_samples] = np.trunc(part * factor)
            elif candidate == "trim-zero-peak":
                trimmed = np.pad(raw[leading:], (0, leading))
                scale = min(HIGH / max(1, float(trimmed.max())), LOW / min(-1, float(trimmed.min())))
                values = np.trunc(trimmed * scale)
            else:
                scale = int(candidate.split("-")[1]) / original_gain
                values = np.trunc(raw * scale)
            saturation = int(np.count_nonzero((values < LOW) | (values > HIGH)))
            pcm = np.clip(values, LOW, HIGH).astype("<i2")
            destination = candidate_dir / f"recording-{index}.pcm"
            destination.write_bytes(pcm.tobytes())
            text = recognizer.transcribe(16000, pcm)
            records.append({"source": str(path), "original_gain": original_gain, "expected": expected,
                "original_text": baseline["microphone_text"], "text": text, "scale": scale,
                "leading_exact_zeros": leading,
                "source_clipped_samples": int(np.count_nonzero((raw <= LOW) | (raw >= HIGH))),
                "new_saturated_samples": saturation,
                "rms_dbfs": float(20 * np.log10(max(np.sqrt(np.mean(pcm.astype(float)**2)), 1e-12) / 32768)),
                "pcm": str(destination), "sha256": hashlib.sha256(pcm.tobytes()).hexdigest(),
                "edit_distance": distance(text, expected) if expected else None})
        summary = {"candidate": candidate, "empty": sum(not r["text"] for r in records),
            "human_label_edit_distance": sum(r["edit_distance"] or 0 for r in records),
            "new_saturated_samples": sum(r["new_saturated_samples"] for r in records), "records": records}
        summaries.append(summary)
        print(json.dumps(summary, ensure_ascii=False), flush=True)
        (args.output / "comparison.json").write_text(json.dumps(summaries, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
