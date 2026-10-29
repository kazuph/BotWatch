#!/usr/bin/env python3
"""Save the device's actual canvas pixels and check circle/centering/scroll bounds."""
import argparse
import json
from pathlib import Path
import struct
import time

from PIL import Image
import serial

from capture import until

ROOT = Path(__file__).resolve().parents[1]


def glyph_ink_proof(image, lines, metrics):
    """Compare every visible glyph with its native VLW mask, not only line labels."""
    data = (ROOT / "assets/noto48.vlw").read_bytes()
    count, _, pixels, _, ascent, _ = struct.unpack_from(">6i", data)
    assert pixels == 48
    offset = 24 + count * 28
    glyphs = {}
    for index in range(count):
        code, height, width, advance, top, left, _ = struct.unpack_from(">7i", data, 24 + index * 28)
        glyphs[chr(code)] = (height, width, advance, top, left, data[offset:offset + width * height])
        offset += width * height
    expected = set()
    visible_glyphs = 0
    for line in lines:
        if line["y"] < 0:
            continue
        cursor = line["x"]
        for character in line["text"]:
            assert character in glyphs, f"Missing native glyph: {character}"
            height, width, advance, top, left, alpha = glyphs[character]
            for y in range(height):
                for x in range(width):
                    # M5GFX VLW alpha blend followed by RGB565 quantization.
                    # Green has the finest precision (6 bits), so this derives
                    # whether the pixel survives conversion without a test threshold.
                    value = (255 * (alpha[y * width + x] + 1)) >> 8
                    if value >> 2:
                        point = (cursor + left + x, line["y"] + ascent - top + y)
                        assert abs(point[0] + .5 - image.width / 2) <= line["capacity"] / 2
                        assert abs(point[1] + .5 - image.height / 2) <= metrics["safe_side"] / 2
                        expected.add(point)
            cursor += advance
            visible_glyphs += 1
    observed = {(x, y) for y in range(image.height) for x in range(image.width) if image.getpixel((x, y)) != (0, 0, 0)}
    missing = expected - observed
    extra = observed - expected
    assert not missing and not extra, f"Glyph pixels differ: missing={len(missing)} extra={len(extra)} examples={list(missing)[:5]}"
    return {"visible_glyphs": visible_glyphs, "missing_ink_pixels": len(missing), "extra_ink_pixels": len(extra), "all_ink_inside_safe_square": True}


def shot(port, log, deadline, output):
    port.write(b"SHOT")
    layout = until(port, log, deadline, lambda line: line.startswith("LAYOUT "))
    metrics = {k: int(v) for k, v in (field.split("=") for field in layout.split()[1:])}
    source = bytes.fromhex(until(port, log, deadline, lambda line: line.startswith("SOURCE ")).split(" ", 1)[1]).decode()
    lines = []
    for _ in range(metrics["total"]):
        line = until(port, log, deadline, lambda value: value.startswith("LINE ")).split(" ", 5)
        # Empty wrapped lines carry no hex after trailing whitespace is stripped.
        span = until(port, log, deadline, lambda value: value.startswith("SPAN ")).split()
        lines.append({"index": int(line[1]), "x": int(line[2]), "y": int(line[3]), "width": int(line[4]), "text": bytes.fromhex(line[5] if len(line) > 5 else "").decode(),
                      "begin": int(span[2]), "end": int(span[3]), "capacity": int(span[4])})
    header = until(port, log, deadline, lambda line: line.startswith("SCREEN ")).split()
    width, height = int(header[1]), int(header[2])
    pixels = bytearray()
    for y in range(height):
        row = until(port, log, deadline, lambda line: line.startswith("PIXELS ")).split()
        assert int(row[1]) == y, f"Missing or reordered display row: expected={y}, actual={row[1]}"
        data = bytes.fromhex(row[2])
        assert len(data) == width * 3, f"Truncated display row {y}"
        pixels.extend(data)
    assert len(pixels) == width * height * 3, "Truncated screen pixels"
    until(port, log, deadline, lambda line: line == "SCREEN_END")
    image = Image.frombytes("RGB", (width, height), bytes(pixels))
    image.save(output)
    assert metrics["scale"] == 1 and metrics["font_pixels"] == 48
    assert metrics["framebuffer"] == 1
    assert len(set(image.getdata())) > 2, "No intermediate antialias shades"
    for line in lines:
        assert line["text"] == source[line["begin"]:line["end"]], "Text differs from source span"
        paragraph_start = source.rfind("\n", 0, line["begin"]) + 1
        paragraph_end = line["end"] == len(source) or source[line["end"]] == "\n"
        assert not (len(line["text"]) == 1 and paragraph_end and line["begin"] > paragraph_start), "Wrapped paragraph ends with a lone character"
    visible = [line for line in lines if line["y"] >= 0]
    radius = min(width, height) / 2
    for line in visible:
        dy = max(abs(line["y"] - height / 2), abs(line["y"] + metrics["font_height"] - height / 2))
        capacity = int(2 * max(0, radius ** 2 - dy ** 2) ** .5)
        assert line["capacity"] == capacity and line["width"] <= capacity, "Line does not fit its circle chord"
        assert abs(2 * line["x"] + line["width"] - width) <= 1, "Not horizontally centered"
        for x in [line["x"], line["x"] + line["width"]]:
            for y in [line["y"], line["y"] + metrics["font_height"]]:
                assert (x - width / 2) ** 2 + (y - height / 2) ** 2 <= radius ** 2, "Glyph rectangle outside circle"
    assert abs(visible[0]["y"] + visible[-1]["y"] + metrics["font_height"] - height) <= 1, "Block not vertically centered"
    outside = sum(1 for y in range(height) for x in range(width) if image.getpixel((x, y)) != (0, 0, 0) and (x + 0.5 - width / 2) ** 2 + (y + 0.5 - height / 2) ** 2 > radius ** 2)
    assert outside == 0, "Rendered pixels outside physical circle"
    ink = glyph_ink_proof(image, lines, metrics)
    return {"metrics": metrics, "source": source, "lines": lines, "nonblack_outside_circle": outside, "glyph_ink": ink, "png": str(output)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pcm", type=Path)
    parser.add_argument("--maximum-text", action="store_true", help="Exercise actual renderer with explicit maximum-length display fixture")
    parser.add_argument("--view", choices=["waiting", "recording", "result", "confirmation"], help="Exercise the same device view renderer without triggering an action")
    parser.add_argument("--seconds", type=float, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.seconds
    proofs = []
    with serial.Serial(args.port, 115200, timeout=1, write_timeout=args.seconds) as port, (args.output / "screen.log").open("wb") as log:
        port.write(b"INFO")
        until(port, log, deadline, lambda line: line.startswith("MEM serving"))
        if args.pcm:
            pcm = args.pcm.read_bytes()
            port.write(b"RLMA" + struct.pack("<IB", len(pcm) // 2, 0) + pcm)
            until(port, log, deadline, lambda line: line.startswith("TEXT "))
        if args.maximum_text:
            port.write(b"VMAX")
        elif args.view:
            port.write({"waiting": b"VWAI", "recording": b"VREC", "result": b"VRES", "confirmation": b"VCON"}[args.view])
        proof = shot(port, log, deadline, args.output / "screen-0.png")
        if args.maximum_text:
            maximum_steps = ((64000 - 400) // 160 + 1) // 4
            assert proof["source"].split("\n", 1)[1] == "きゃ" * maximum_steps
        proofs.append(proof)
        if args.view == "waiting":
            assert [line["text"] for line in proof["lines"]] == ["タップして話す", "4秒間 録音します"], "Fixed waiting wording split unnaturally"
        covered = {index for line in proof["lines"] for index in range(line["begin"], line["end"])}
        first = proof["metrics"]["first"]
        last = proof["metrics"]["views"] - 1
        while first < last:
            step = 1
            port.write(b"NEXT" * step)
            first += step
            proof = shot(port, log, deadline, args.output / f"screen-{first}.png")
            assert proof["metrics"]["first"] == first
            covered.update(index for line in proof["lines"] for index in range(line["begin"], line["end"]))
            proofs.append(proof)
        assert covered == {index for index, char in enumerate(proof["source"]) if char != "\n"}, "Some source characters inaccessible by scrolling"
    (args.output / "screen-proof.json").write_text(json.dumps({"status": "PASS", "pages": proofs}, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"status": "PASS", "pages": len(proofs), "source": proof["source"], "all_lines_accessible": True}, ensure_ascii=False))


if __name__ == "__main__":
    main()
