#!/usr/bin/env python3
"""Compare packed print glyphs and a device raster with the pinned Noto source."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", required=True, type=Path)
    parser.add_argument("--raster-only", action="store_true", help="Check a device render without a print transaction")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    data = (root / "assets/noto64.vlw").read_bytes()
    manifest = json.loads((root / "assets/print-font.json").read_text())
    assert hashlib.sha256(data).hexdigest() == manifest["source_vlw_sha256"]
    count, _, pixels, _, ascent, descent = struct.unpack_from(">6i", data)
    assert pixels == 64 and ascent + descent <= 96
    offset = 24 + count * 28
    glyphs = {}
    for index in range(count):
        code, height, width, advance, top, left, _ = struct.unpack_from(">7i", data, 24 + index * 28)
        alpha = data[offset:offset + width * height]
        offset += width * height
        glyphs[chr(code)] = (width, height, advance, top, left, alpha)
    source = (root / "main/print_font.inc").read_text()
    packed = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", source))
    entries = [tuple(map(int, row.split(","))) for row in re.findall(r"\{([\d, -]+)\},", source)]
    vocabulary = set("".join(re.findall(r'^\s*"([^"]*)"', (root / "main/vocabulary.inc").read_text(), re.M)[2:]))
    assert len(vocabulary) == 82
    vocabulary |= {chr(ord(char) + 0x60) if 0x3041 <= ord(char) <= 0x3096 else char for char in vocabulary}
    assert {chr(row[0]) for row in entries} == vocabulary and len(entries) == 163
    for code, width, height, advance, top, left, start in entries:
        assert glyphs[chr(code)][:5] == (width, height, advance, top, left)
        alpha = glyphs[chr(code)][5]
        stride = (width + 7) // 8
        expected = bytearray(stride * height)
        for y in range(height):
            for x in range(width):
                if alpha[y * width + x] >= 128:
                    expected[y * stride + x // 8] |= 128 >> (x % 8)
        assert any(expected) and packed[start:start + len(expected)] == expected
    result = json.loads((args.capture / "print-result.json").read_text())
    text = result["text"]
    cursor = 0
    points = set()
    min_x = max_x = 0
    min_y, max_y = 1 << 30, -(1 << 30)
    for char in text:
        width, height, advance, top, left, alpha = glyphs[char]
        min_x, max_x = min(min_x, cursor + left), max(max_x, cursor + left + width)
        min_y, max_y = min(min_y, -top), max(max_y, height - top)
        for y in range(height):
            for x in range(width):
                if alpha[y * width + x] >= 128:
                    points.add((cursor + left + x, y - top))
        cursor += advance
    rows = 16 + max_x - min_x + 48
    baseline = (96 - (max_y - min_y)) // 2 - min_y
    expected = bytearray(rows * 12)
    for x, y in points:
        px, py = 95 - (baseline + y), 16 + 24 - min_x + x
        assert 0 <= px < 96 and 16 <= py < rows
        expected[py * 12 + px // 8] |= 128 >> (px % 8)
    actual = (args.capture / "raster.bin").read_bytes()
    assert actual == expected, "Device raster differs from Noto source reconstruction"
    assert not any(actual[:16 * 12])
    job = (args.capture / "print-job.bin").read_bytes() if not args.raster_only else b""
    expected_job = (bytes(15) + bytes.fromhex("10fff1021d7630000c00")
                    + struct.pack("<H", rows) + actual + bytes.fromhex("1b4a6410fff145"))
    if not args.raster_only:
        assert job == expected_job
    log = (args.capture / "serial.log").read_text()
    packets = [tuple(map(int, match)) for match in re.findall(
        r"BLE_PACKET bytes=(\d+) sent=(\d+) total=(\d+) credits=(-?\d+) time_us=(\d+)", log)]
    assert packets or args.raster_only
    sent, previous_time = 0, None
    gaps = []
    for length, cumulative, total, credits, timestamp in packets:
        assert 0 < length <= 90 and credits >= 0
        sent += length
        assert cumulative == sent and total == len(job)
        if previous_time is not None:
            gaps.append(timestamp - previous_time)
        previous_time = timestamp
    if not args.raster_only:
        assert sent == len(job) and min(gaps) >= 30000
        assert "completion=aa uncertain=0" in log
        assert log.index("BLE_STATUS code=00") < log.index("BLE_PACKET ") < log.index("BLE_PRINT success=1")
    image = Image.frombytes("1", (96, rows), actual).convert("L").point(lambda value: 255 - value)
    image.save(args.capture / "raster.png")
    image.transpose(Image.Transpose.ROTATE_90).save(args.capture / "label-readable.png")
    report = {"status": "PASS", "font_glyphs": len(entries), "packed_bitmap_bytes": len(packed),
              "source_mask_differences": 0, "device_raster_differences": 0,
              "text": text, "width": 96, "rows": rows, "bytes": len(actual),
              "job_bytes": len(job), "packets": len(packets), "minimum_packet_gap_us": min(gaps) if gaps else None,
              "packet_credits_nonnegative": True if packets else None, "completion": "aa" if packets else None,
              "unverified": ["all-163-glyph device raster", "maximum-length device raster"]}
    (args.capture / "raster-proof.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False))


if __name__ == "__main__":
    main()
