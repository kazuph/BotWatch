# /// script
# dependencies = ["bleak>=0.22", "pillow>=10"]
# ///
"""Print a staircase to find which head columns land on the paper.

Column block k (8 dots wide, k = 0..N-1) is black for (k+1)*STEP rows.
Count the visible steps: the shortest visible step = first printable block,
the longest = last printable block.
"""
import argparse
import asyncio
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from labelprinter import protocol  # noqa: E402
from labelprinter.ble import Printer  # noqa: E402


def staircase(blocks: int, step: int) -> Image.Image:
    img = Image.new("L", (blocks * 8, blocks * step + 16), 255)
    d = ImageDraw.Draw(img)
    for k in range(blocks):
        d.rectangle([k * 8, 0, k * 8 + 6, (k + 1) * step - 1], fill=0)  # 1-dot gap between blocks
    return img


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--blocks", type=int, default=16)
    ap.add_argument("--step", type=int, default=16, help="rows per step (8 rows = 1mm)")
    ap.add_argument("--preview")
    a = ap.parse_args()
    img = staircase(a.blocks, a.step)
    if a.preview:
        img.save(a.preview)
    p = await Printer.find()
    await p.write(protocol.print_job(img))
    try:
        print("reply:", (await asyncio.wait_for(p.rx.get(), 10)).hex())
    except asyncio.TimeoutError:
        print("no reply")
    await p.close()


asyncio.run(main())
