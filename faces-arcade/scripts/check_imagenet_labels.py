#!/usr/bin/env python3
"""Check Japanese labels against ESP-DL 3.3.13's model category order."""
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "managed_components/espressif__esp-dl/vision/classification/imagenet_category_name.hpp"
TABLE = ROOT / "assets/imagenet_ja.json"
SOURCE_SHA256 = "0a47e570c9813e270889d09a8b2ee7e636f4987e7e431fa83e6ec785e6fa2dc8"


def main():
    data = SOURCE.read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError("ESP-DL category source differs from 3.3.13")
    source = re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', data.decode("utf-8"))
    table = json.loads(TABLE.read_text(encoding="utf-8"))
    if len(source) != 1000 or not isinstance(table, list) or len(table) != 1000:
        raise ValueError("expected exactly 1000 classes")
    names = []
    for index, (entry, expected) in enumerate(zip(table, source)):
        if not isinstance(entry, dict) or set(entry) != {"source", "ja"}:
            raise ValueError(f"entry {index}: expected source and ja")
        if entry["source"] != expected:
            raise ValueError(f"entry {index}: wrong model order")
        name = entry["ja"]
        if not isinstance(name, str) or not name.strip() or name != name.strip():
            raise ValueError(f"entry {index}: empty or padded Japanese name")
        names.append(name)
    if len(set(names)) != 1000:
        raise ValueError("Japanese names must be unique")
    print("PASS: 1000 ordered, nonempty, unique Japanese labels")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
