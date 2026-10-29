# /// script
# dependencies = ["bleak>=0.22"]
# ///
"""Connect to the printer and dump its GATT table (read-only)."""
import asyncio
import sys
from bleak import BleakClient, BleakScanner

NAME = sys.argv[1] if len(sys.argv) > 1 else "GHLP"


async def main():
    dev = await BleakScanner.find_device_by_filter(lambda d, a: NAME in (a.local_name or d.name or ""), timeout=10)
    if not dev:
        sys.exit("printer not found")
    async with BleakClient(dev) as c:
        print("connected", dev.address, "mtu", c.mtu_size)
        for s in c.services:
            print("SERVICE", s.uuid, s.description)
            for ch in s.characteristics:
                val = ""
                if "read" in ch.properties:
                    try:
                        val = (await c.read_gatt_char(ch)).hex()
                    except Exception as e:  # noqa: BLE001
                        val = f"<{e}>"
                print("  CHAR", ch.uuid, ch.properties, val)


asyncio.run(main())
