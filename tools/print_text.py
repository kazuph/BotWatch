# /// script
# dependencies = ["bleak>=0.22", "pillow>=10"]
# ///
"""Print a text label:  uv run tools/print_text.py "テキスト" [--border] [--preview out.png]"""
import argparse
import asyncio
import logging
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from labelprinter import protocol  # noqa: E402
from labelprinter.ble import Printer  # noqa: E402
from labelprinter.render import text_label  # noqa: E402


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("text")
    ap.add_argument("--border", action="store_true")
    ap.add_argument("--preview")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.v else logging.INFO)
    img = text_label(a.text, a.border)
    if a.preview:
        img.rotate(90, expand=True).save(a.preview)
    job = protocol.print_job(img)
    print(f"bitmap {img.size[0]}x{img.size[1]} dots, job {len(job)} bytes")
    if a.dry_run:
        return
    p = await Printer.find()
    st = await p.request(protocol.STATUS)
    print("status before:", st.hex() if st else None)
    await p.write(job)
    try:
        while True:
            r = await asyncio.wait_for(p.rx.get(), 8)
            print("reply:", r.hex())
    except asyncio.TimeoutError:
        pass
    await p.close()


asyncio.run(main())
