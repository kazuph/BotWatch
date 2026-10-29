#!/usr/bin/env python3
"""Fetch the published FP32 comparator; firmware still uses only the ESP-DL model."""
import hashlib
import json
from pathlib import Path
import urllib.request

root = Path(__file__).resolve().parents[1] / "reference"
manifest = json.loads((root / "release_metadata.json").read_text())["files"]
for name in ["inference.py", "features.py", "vocabulary.json", "inference_config.json"]:
    if hashlib.sha256((root / name).read_bytes()).hexdigest() != manifest[name]["SHA256"]:
        raise RuntimeError(f"Reference source differs from released hash: {name}")
target = root / "model.onnx"
expected = manifest["model.onnx"]
if not target.exists() or hashlib.sha256(target.read_bytes()).hexdigest() != expected["SHA256"]:
    temporary = target.with_suffix(".download")
    try:
        with urllib.request.urlopen("https://huggingface.co/ayousanz/ralomi/resolve/main/model.onnx?download=true", timeout=120) as response, temporary.open("wb") as output:
            while block := response.read(65536):
                output.write(block)
        if temporary.stat().st_size != expected["bytes"] or hashlib.sha256(temporary.read_bytes()).hexdigest() != expected["SHA256"]:
            raise RuntimeError("FP32 model differs from the published release")
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
print(f"FP32 PASS bytes={target.stat().st_size} sha256={expected['SHA256']}")
