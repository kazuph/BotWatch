# /// script
# dependencies = ["bleak>=0.22"]
# ///
"""Send non-printing query commands and show replies."""
import asyncio
import logging
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from labelprinter.ble import Printer  # noqa: E402

QUERIES = {
    "status   10ff40": bytes([0x10, 0xFF, 0x40]),
    "battery  10ff50f1": bytes([0x10, 0xFF, 0x50, 0xF1]),
    "version  10ff20f1": bytes([0x10, 0xFF, 0x20, 0xF1]),
    "model    10ff20f0": bytes([0x10, 0xFF, 0x20, 0xF0]),
    "sn       10ff20f2": bytes([0x10, 0xFF, 0x20, 0xF2]),
    "btver    10ff3010": bytes([0x10, 0xFF, 0x30, 0x10]),
    "btname   10ff3011": bytes([0x10, 0xFF, 0x30, 0x11]),
    "shutdown 10ff13": bytes([0x10, 0xFF, 0x13]),
}


async def main():
    logging.basicConfig(level=logging.DEBUG if "-v" in sys.argv else logging.INFO)
    p = await Printer.find()
    print("credits after subscribe:", p.credits)
    for label, cmd in QUERIES.items():
        r = await p.request(cmd)
        txt = r.decode("gb2312", "replace") if r else ""
        print(f"{label:20} -> {r.hex() if r else None}  {txt!r}")
    await p.close()


asyncio.run(main())
