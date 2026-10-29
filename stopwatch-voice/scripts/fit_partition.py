#!/usr/bin/env python3
"""Grow factory from the measured binary, aligning model to its required 64KiB boundary."""
import csv
from pathlib import Path

root = Path(__file__).resolve().parents[1]
table = root / "partitions.csv"
rows = list(csv.reader(line for line in table.read_text().splitlines() if line and not line.startswith("#")))
factory = next(row for row in rows if row[0].strip() == "factory")
model = next(row for row in rows if row[0].strip() == "model")
app_offset = int(factory[3].strip(), 0)
app_bytes = (root / "build/stopwatch_voice.bin").stat().st_size
alignment = 0x10000  # ESP-DL flash partition requires 64KiB alignment (upstream partition table).
model_offset = ((app_offset + app_bytes + alignment - 1) // alignment) * alignment
if model_offset + int(model[4].strip(), 0) > 16 * 1024 * 1024:
    raise ValueError("Measured app and model partition exceed the specified 16MB flash")
original = table.read_text()
updated = []
for line in original.splitlines():
    if line.startswith("factory,"):
        line = f"factory,  app,  factory, {app_offset:#x}, {model_offset - app_offset:#x},"
    elif line.startswith("model,"):
        line = f"model,    data, spiffs,  {model_offset:#x}, {int(model[4].strip(), 0):#x},"
    updated.append(line)
table.write_text("\n".join(updated) + "\n")
print(f"Measured app={app_bytes} bytes; factory={model_offset - app_offset:#x}; model={model_offset:#x}")
