#!/usr/bin/env python3
"""Create native Noto antialiased VLW glyphs at the requested size.

uv run --with pillow --with fonttools scripts/build_font.py
"""
import hashlib
import argparse
import json
import math
from pathlib import Path
import re
import struct
import urllib.request

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "BotWatch-font-build"}), timeout=120) as response:
        return response.read()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pixels", type=int, choices=[16, 48, 64], default=48)
    args = parser.parse_args()
    pixels = args.pixels
    assets = ROOT / "assets"
    assets.mkdir(exist_ok=True)
    cache = ROOT / "models/fonts"
    cache.mkdir(parents=True, exist_ok=True)
    manifest_path = assets / "font-source.json"
    if manifest_path.exists():
        source = json.loads(manifest_path.read_text())
    else:
        commits = json.loads(get("https://api.github.com/repos/google/fonts/commits?path=ofl/notosansjp&per_page=1"))
        source = {"repository": "https://github.com/google/fonts", "commit": commits[0]["sha"]}
    origin = f"https://raw.githubusercontent.com/google/fonts/{source['commit']}/ofl/notosansjp"
    font_path = cache / "NotoSansJP.ttf"
    if not font_path.exists():
        font_path.write_bytes(get(origin + "/NotoSansJP%5Bwght%5D.ttf"))
    digest = hashlib.sha256(font_path.read_bytes()).hexdigest()
    if "sha256" in source and digest != source["sha256"]:
        raise ValueError("Font differs from the recorded source")
    source.update({"sha256": digest, "pixels": pixels, "weight": 400, "license_url": origin + "/OFL.txt"})
    (ROOT / "licenses/NotoSansJP-OFL.txt").write_bytes(get(source["license_url"]))
    tokens = re.findall(r'^\s*"([^"]*)"', (ROOT / "main/vocabulary.inc").read_text(), re.M)[2:]
    vocabulary = set("".join(tokens))
    assert len(vocabulary) == 82
    katakana = {chr(ord(char) + 0x60) if 0x3041 <= ord(char) <= 0x3096 else char for char in vocabulary}
    charset = vocabulary | katakana | set("0123456789ABCDEF .-dBFSms()%/")
    for path in (ROOT / "main").glob("*.cpp"):
        for literal in re.findall(r'"(?:[^"\\]|\\.)*"', path.read_text()):
            if any(ord(character) > 127 for character in literal):
                charset.update(json.loads(literal))
    charset.discard("\n")
    charset.update("これで印刷しますか？やり直し印刷中残り聞き取り")
    cmap = TTFont(font_path).getBestCmap()
    missing = [char for char in sorted(charset) if ord(char) not in cmap]
    if missing:
        raise ValueError(f"Missing Noto glyphs: {missing}")
    # These are the confirmed plan's confirmation/printing labels. Include their
    # actual Noto glyphs even before stage 4 wires their transitions.
    font = ImageFont.truetype(str(font_path), pixels)
    font.set_variation_by_axes([source["weight"]])
    glyphs = []
    bitmaps = []
    for char in sorted(charset):
        left, top, right, bottom = font.getbbox(char, anchor="ls")
        width, height = right - left, bottom - top
        bitmap = Image.new("L", (width, height))
        ImageDraw.Draw(bitmap).text((-left, -top), char, font=font, anchor="ls", fill=255)
        if char != " " and bitmap.getbbox() is None:
            raise ValueError(f"Empty glyph: {char}")
        glyphs.append({"character": char, "codepoint": ord(char), "width": width, "height": height, "advance": math.ceil(font.getlength(char)), "top_extent": -top, "left_extent": left})
        bitmaps.append(bitmap.tobytes())
    ascent = max(glyph["top_extent"] for glyph in glyphs)
    descent = max(glyph["height"] - glyph["top_extent"] for glyph in glyphs)
    data = bytearray(struct.pack(">6i", len(glyphs), 11, pixels, 0, ascent, descent))
    for glyph in glyphs:
        data.extend(struct.pack(">7i", glyph["codepoint"], glyph["height"], glyph["width"], glyph["advance"], glyph["top_extent"], glyph["left_extent"], 0))
    for bitmap in bitmaps:
        data.extend(bitmap)
    (assets / f"noto{pixels}.vlw").write_bytes(data)
    source.update({"glyphs": len(glyphs), "vocabulary_characters": 82, "ascent": ascent, "descent": descent, "vlw_bytes": len(data), "vlw_sha256": hashlib.sha256(data).hexdigest(), "charset": "".join(sorted(charset))})
    (assets / f"noto{pixels}-source.json").write_text(json.dumps(source, ensure_ascii=False, indent=2) + "\n")
    (assets / f"noto{pixels}-metrics.json").write_text(json.dumps(glyphs, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(source, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
