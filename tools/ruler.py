# /// script
# dependencies = ["bleak>=0.22", "pillow>=10"]
# ///
"""Print a lengthwise ruler (tick every 1mm, number every 5mm) to measure lost leading rows."""
import argparse
import asyncio
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from labelprinter import protocol  # noqa: E402
from labelprinter.ble import Printer  # noqa: E402
from labelprinter.render import DOTS_PER_MM, HEAD_DOTS, _font  # noqa: E402


def ruler(length_mm: int) -> Image.Image:
    w, h = length_mm * DOTS_PER_MM, HEAD_DOTS
    img = Image.new("L", (w, h), 255)
    d = ImageDraw.Draw(img)
    font = _font(24)
    for mm in range(length_mm + 1):
        x = min(mm * DOTS_PER_MM, w - 2)
        tall = mm % 5 == 0
        d.rectangle([x, 0, x + 1, 40 if tall else 16], fill=0)
        if tall and mm < length_mm:
            d.text((x + 4, 46), str(mm), font=font, fill=0)
    d.rectangle([0, h - 3, w - 1, h - 1], fill=0)  # baseline along the bottom
    return img.rotate(-90, expand=True)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--length", type=int, default=40, help="mm")
    ap.add_argument("--preview")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    img = ruler(a.length)
    if a.preview:
        img.rotate(90, expand=True).save(a.preview)
    if a.dry_run:
        return
    p = await Printer.find()
    await p.write(protocol.print_job(img))
    try:
        print("reply:", (await asyncio.wait_for(p.rx.get(), 10)).hex())
    except asyncio.TimeoutError:
        print("no reply")
    await p.close()


asyncio.run(main())
