#!/usr/bin/env python3
"""Select all recognition glyphs from the pinned Noto64 VLW, packed for printing."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sample", action="store_true", help="ignore private local presets")
args=parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = root / "assets/noto64.vlw"
data = source.read_bytes()
manifest = json.loads((root / "assets/noto64-source.json").read_text())
assert hashlib.sha256(data).hexdigest() == manifest["vlw_sha256"]
count, _, pixels, _, ascent, descent = struct.unpack_from(">6i", data)
vocabulary = set("".join(re.findall(r'^\s*"([^"]*)"', (root / "main/vocabulary.inc").read_text(), re.M)[2:]))
assert len(vocabulary) == 82 and pixels == 64
vocabulary |= {chr(ord(char) + 0x60) if 0x3041 <= ord(char) <= 0x3096 else char for char in vocabulary}
# Include only the selected preset rows; generated output is private and ignored.
vocabulary |= set("".join(re.findall(r'\{"[^"]*", "[^"]*", "([^"]*)"\}', (root / "main" / ("presets_local.h" if not args.sample and (root / "main/presets_local.h").exists() else "presets_local.example.h")).read_text()))) - {" "}
offset = 24 + count * 28
glyphs, bits = [], bytearray()
for index in range(count):
    code, height, width, advance, top, left, _ = struct.unpack_from(">7i", data, 24 + index * 28)
    alpha = data[offset:offset + width * height]
    offset += width * height
    if chr(code) not in vocabulary:
        continue
    stride = (width + 7) // 8
    packed = bytearray(stride * height)
    for y in range(height):
        for x in range(width):
            # Same 128 cutoff as labelprinter.protocol.raster.
            if alpha[y * width + x] >= 128:
                packed[y * stride + x // 8] |= 0x80 >> (x % 8)
    assert any(packed), f"Empty glyph {chr(code)}"
    glyphs.append((code, width, height, advance, top, left, len(bits)))
    bits.extend(packed)
assert {chr(g[0]) for g in glyphs} == vocabulary
lines = [f"constexpr int kPrintAscent = {ascent};", f"constexpr int kPrintDescent = {descent};",
         "constexpr Glyph kPrintGlyphs[] = {"]
lines += ["    {" + ", ".join(map(str, glyph)) + "}," for glyph in glyphs]
lines += ["};", "constexpr uint8_t kPrintBits[] = {"]
lines += ["    " + ",".join(f"0x{x:02x}" for x in bits[i:i + 24]) + "," for i in range(0, len(bits), 24)]
lines += ["};", ""]
(root / "main/print_font.inc").write_text("\n".join(lines))
record = {"source_vlw_sha256": manifest["vlw_sha256"], "noto_source_sha256": manifest["sha256"],
          "pixels": pixels, "ascent": ascent, "descent": descent, "glyphs": len(glyphs),
          "bitmap_bytes": len(bits), "alpha_cutoff": 128, "characters": "".join(sorted(vocabulary))}
(root / "assets/print-font.json").write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n")
print(json.dumps(record, ensure_ascii=False, indent=2))
