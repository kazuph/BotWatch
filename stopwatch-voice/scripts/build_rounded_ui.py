#!/usr/bin/env python3
"""Build native M PLUS UI fonts and reuse the approved design's raster icons."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import urllib.request
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "BotWatch-font-build"}), timeout=120) as response:
        return response.read()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--design", required=True, type=Path)
    args = parser.parse_args()
    folder = "ofl/mplusrounded1c"
    cache = ROOT / "models/fonts/rounded"
    cache.mkdir(parents=True, exist_ok=True)
    target = ROOT / "assets/rounded-source.json"
    manifest = json.loads(target.read_text()) if target.exists() else {}
    if "commit" not in manifest:
        manifest["commit"] = json.loads(get(f"https://api.github.com/repos/google/fonts/commits?path={folder}&per_page=1"))[0]["sha"]
    origin = f"https://raw.githubusercontent.com/google/fonts/{manifest['commit']}/{folder}"
    sizes = {size: set() for size in (15, 17, 20, 22, 24, 32, 34, 38, 40, 56)}
    html = (args.design / "mock.html").read_text()
    for size, text in re.findall(r'<text[^>]*font-size="(\d+)"[^>]*>([^<]*)</text>', html):
        if int(size) in sizes: sizes[int(size)].update(text)
    presets = "".join(re.findall(r'\{"[^"]*", "([^"]*)", "[^"]*"\}', (ROOT / "main/presets_local.example.h").read_text()))
    names = "しろ・ひらがなカタカナわくはんてんかんじ選択" + presets
    names+="".join(chr(c) for c in range(0x3041,0x3097))+"".join(chr(c) for c in range(0x30a1,0x30f7))
    sizes[15].update(names)
    sizes[17].update(names + "つぎ／長おしで印刷もどるえらぶおして話す")
    sizes[20].update("おして話すはなすと終わりもどるやめるもういちど中止できません電源を見てね")
    for size in (22, 24): sizes[size].update("0123456789.%あとびょうプリンターの電池声が小さいようですだれにする？")
    # The action mode's home panel labels its tiles at 24px.
    for literal in re.findall(r'"(?:[^"\\]|\\.)*"', (ROOT / "main/home.cpp").read_text()):
        if any(ord(char) > 127 for char in literal): sizes[24].update(json.loads(literal))
    sizes[24].update("BCS ")
    sizes[32].update("".join(re.findall(r'^\s*"([^"]*)"', (ROOT / "main/vocabulary.inc").read_text(), re.M)[2:]))
    sizes[32].update("0123456789ABCDEF .-dBFSms()%/プリンターにつながらないよ電源を見てね声が小さいようです")
    sizes[32].update("話しかけてね")
    sizes[34].update("左上のボタンを右上おしながら話してね")
    for path in [*(ROOT / "main").glob("*.cpp"), ROOT / "main/presets_local.example.h", ROOT / "main/talk_commands.h"]:
        for literal in re.findall(r'"(?:[^"\\]|\\.)*"', path.read_text()):
            if any(ord(char) > 127 for char in literal): sizes[32].update(json.loads(literal))
    records, embeds = [], []
    for size, charset in sizes.items():
        charset.discard("\n")
        weight = 800 if size in (32, 34, 38, 40, 56) else 700
        path = cache / f"MPLUSRounded1c-{weight}.ttf"
        filename = f"MPLUSRounded1c-{'ExtraBold' if weight == 800 else 'Bold'}.ttf"
        if not path.exists(): path.write_bytes(get(origin + "/" + filename))
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        previous = manifest.get("fonts", {}).get(str(weight))
        assert not previous or previous == digest
        manifest.setdefault("fonts", {})[str(weight)] = digest
        font = ImageFont.truetype(str(path), size)
        cmap = TTFont(path).getBestCmap()
        assert all(ord(char) in cmap for char in charset)
        glyphs, bitmaps = [], []
        for char in sorted(charset):
            left, top, right, bottom = font.getbbox(char, anchor="ls")
            width, height = right - left, bottom - top
            bitmap = Image.new("L", (width, height))
            ImageDraw.Draw(bitmap).text((-left, -top), char, font=font, anchor="ls", fill=255)
            assert char == " " or bitmap.getbbox()
            glyphs.append((ord(char), height, width, math.ceil(font.getlength(char)), -top, left, 0))
            bitmaps.append(bitmap.tobytes())
        ascent = max(g[4] for g in glyphs); descent = max(g[1] - g[4] for g in glyphs)
        data = struct.pack(">6i", len(glyphs), 11, size, 0, ascent, descent)
        data += b"".join(struct.pack(">7i", *glyph) for glyph in glyphs) + b"".join(bitmaps)
        (ROOT / f"assets/ui{size}.vlw").write_bytes(data)
        embeds.append(f'extern const uint8_t ui{size}_start[] asm("_binary_ui{size}_vlw_start");')
        records.append({"size": size, "weight": weight, "ascent": ascent, "descent": descent, "glyphs": len(glyphs), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "characters": "".join(sorted(charset))})
    embeds += ["struct UiFont { int size, ascent; const uint8_t *data; };", "constexpr UiFont kUiFonts[] = {"]
    embeds += [f"    {{{row['size']}, {row['ascent']}, ui{row['size']}_start}}," for row in records]
    (ROOT / "main/ui_fonts.inc").write_text("\n".join(embeds + ["};", ""]))
    crops = {"Mic": ("s01.png", (157, 139, 309, 291)), "Wave": ("s02.png", (170, 215, 306, 287)), "Dots": ("s05.png", (190, 299, 279, 331)), "Success": ("s06.png", (170, 117, 296, 243))}
    lines = []
    for name, (filename, box) in crops.items():
        image = Image.open(args.design / filename).convert("RGB")
        assert image.height == 466 and image.width in (466, 467), image.size
        image = image.crop(box)
        values = [((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) for r, g, b in image.getdata()]
        lines += [f"constexpr int k{name}Width = {image.width}, k{name}Height = {image.height};", f"constexpr uint16_t k{name}[] = {{"]
        lines += ["    " + ",".join(hex(v) for v in values[i:i + 24]) + "," for i in range(0, len(values), 24)]
        lines += ["};"]
    (ROOT / "main/ui_icons.inc").write_text("\n".join(lines) + "\n")
    # Google Fonts omits OFL.txt in this family. The font's own name records
    # declare OFL 1.1; retain its copyright and the identical standard terms.
    names = TTFont(cache / "MPLUSRounded1c-700.ttf")["name"]
    copyright_text = names.getDebugName(0)
    declaration = names.getDebugName(13)
    assert "SIL Open Font License, Version 1.1" in declaration
    standard = (ROOT / "licenses/NotoSansJP-OFL.txt").read_text()
    standard = "SIL OPEN FONT LICENSE" + standard.split("SIL OPEN FONT LICENSE", 1)[1]
    (ROOT / "licenses/MPLUSRounded1c-OFL.txt").write_text(copyright_text + "\n\n" + declaration + "\n\n" + standard)
    manifest.update({"repository": "https://github.com/google/fonts", "directory": folder, "fonts_by_size": records, "design_sha256": hashlib.sha256(html.encode()).hexdigest(), "icon_sources": crops})
    target.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"status": "PASS", "commit": manifest["commit"], "font_bytes": sum(row["bytes"] for row in records)}))


if __name__ == "__main__": main()
