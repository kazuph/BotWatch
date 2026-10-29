#!/usr/bin/env python3
"""Derive the requested normalized-RMS boundary from labeled saved device PCM."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", required=True, type=Path)
    args = parser.parse_args()
    sources = [
        ("empty", "stage1-bounded-human/recording-1"),
        ("speech", "stage1-bounded-human/recording-3"),
        ("speech", "stage1-bounded-human/recording-4"),
        ("empty", "stage2-half-input-after-ble-mic"),
        ("empty", "stage2-screen48-after-ble-mic"),
    ]
    records = []
    for label, name in sources:
        path = args.artifacts / name
        metadata = json.loads((path / "comparison.json").read_text())
        text = metadata["microphone_text"].split(" ", 4)
        text = text[4] if len(text) == 5 else ""
        assert (text == "やまだたろー") if label == "speech" else not text
        raw_data = (path / "raw-microphone.pcm").read_bytes()
        raw = [value for value, in struct.iter_unpack("<h", raw_data)]
        leading = next((i for i, value in enumerate(raw) if value), len(raw))
        raw = raw[leading:] + [0] * leading
        normalized, chunks = [], []
        for first in range(0, len(raw), 16000):
            part = raw[first:first + 16000]
            gain = min(16, -32752 / min(-1, min(part)), 32751 / max(1, max(part)))
            result = [int(value * gain) for value in part]
            normalized.extend(result)
            chunks.append({"gain": gain, "normalized_rms_dbfs": 10 * math.log10(sum(v*v for v in result) / len(result) / 32768**2 + 1e-12)})
        actual = (path / "microphone.pcm").read_bytes()
        assert struct.pack(f"<{len(normalized)}h", *normalized) == actual
        rms = 10 * math.log10(sum(v*v for v in normalized) / len(normalized) / 32768**2 + 1e-12)
        records.append({"label": label, "source": str(path), "text": text,
                        "raw_sha256": hashlib.sha256(raw_data).hexdigest(),
                        "normalized_rms_dbfs": rms, "chunks": chunks})
    empty_max = max(row["normalized_rms_dbfs"] for row in records if row["label"] == "empty")
    speech_min = min(row["normalized_rms_dbfs"] for row in records if row["label"] == "speech")
    assert empty_max < speech_min, "Labeled classes overlap; no separating midpoint is supported"
    threshold = (empty_max + speech_min) / 2
    report = {"status": "PASS", "records": records, "highest_empty_dbfs": empty_max,
              "lowest_success_dbfs": speech_min, "threshold_dbfs": threshold,
              "derivation": "arithmetic midpoint in dBFS between closest empty and successful recordings",
              "pending_interpretation": "Identify speech-bearing seconds by existing recognizer nonblank token positions; do not invent a raw-energy VAD threshold",
              "unverified": ["warning display on current device", "new 15-second human recordings"]}
    (args.artifacts / "stage4-volume-study.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False))


if __name__ == "__main__":
    main()
