# /// script
# dependencies = ["bleak>=0.22"]
# ///
"""Scan for nearby BLE devices and list printer candidates (service 0xFF00)."""
import asyncio
from bleak import BleakScanner


async def main():
    found = await BleakScanner.discover(timeout=8.0, return_adv=True)
    for dev, adv in sorted(found.values(), key=lambda x: -x[1].rssi):
        uuids = ",".join(u[4:8] for u in adv.service_uuids)
        print(f"{dev.address}  rssi={adv.rssi:4d}  name={adv.local_name or dev.name!s:30}  svc={uuids}  mfr={ {k: v.hex() for k, v in adv.manufacturer_data.items()} }")


asyncio.run(main())
