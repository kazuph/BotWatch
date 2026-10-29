"""BLE transport for Deli/YXQ "P12-family" label printers (e.g. 2523/GHLP_BLE).

Protocol reverse-engineered from com.feioou.deliprint.yxq V3.9.1
(com.yxqapp.sdk.BluetoothPort / PrintL11 / Printer_ESC).

GATT (service 0xFF00):
  ff02  write-without-response : command / print data (TX)
  ff01  notify                 : printer replies (RX)
  ff03  notify                 : flow control. `01 NN` = grant NN credits, `02 lo hi` = MTU
Each write consumes one credit; the printer grants an initial 4 credits.
The stock app also waits 30 ms between 90-byte packets (getDataPacketDelay); without
that pacing the printer silently drops early packets (measured: ~30 rows lost).
"""
from __future__ import annotations

import asyncio
import logging

from bleak import BleakClient, BleakScanner

SERVICE = "0000ff00-0000-1000-8000-00805f9b34fb"
RX = "0000ff01-0000-1000-8000-00805f9b34fb"
TX = "0000ff02-0000-1000-8000-00805f9b34fb"
CX = "0000ff03-0000-1000-8000-00805f9b34fb"

log = logging.getLogger(__name__)


class Printer:
    def __init__(self, client: BleakClient, chunk: int = 90, packet_delay: float = 0.03):
        self.client = client
        self.chunk = chunk
        self.packet_delay = packet_delay
        self.credits = 0
        self._credit_event = asyncio.Event()
        self.rx: asyncio.Queue[bytes] = asyncio.Queue()

    @classmethod
    async def find(cls, name: str = "GHLP", timeout: float = 10.0) -> "Printer":
        dev = await BleakScanner.find_device_by_filter(
            lambda d, a: name in (a.local_name or d.name or ""), timeout=timeout
        )
        if dev is None:
            raise RuntimeError(f"printer matching {name!r} not found")
        client = BleakClient(dev)
        await client.connect()
        p = cls(client)
        await client.start_notify(CX, p._on_cx)
        await client.start_notify(RX, p._on_rx)
        # the stock app sends nothing to obtain credits; printer pushes `01 04` on subscribe.
        await asyncio.sleep(0.3)
        if p.credits == 0:
            p.credits = 4
        return p

    def _on_cx(self, _ch, data: bytearray):
        log.debug("CX %s", data.hex())
        if len(data) == 2 and data[0] == 1:
            self.credits = 4 if data[1] == 4 and self.credits <= 0 else self.credits + data[1]
            self._credit_event.set()
        elif len(data) == 3 and data[0] == 2:
            log.debug("printer MTU %d", data[1] | data[2] << 8)

    def _on_rx(self, _ch, data: bytearray):
        log.debug("RX %s", data.hex())
        self.rx.put_nowait(bytes(data))

    async def write(self, data: bytes) -> None:
        for i in range(0, len(data), self.chunk):
            while self.credits <= 0:
                self._credit_event.clear()
                try:
                    await asyncio.wait_for(self._credit_event.wait(), 2.0)
                except asyncio.TimeoutError:
                    log.warning("no credit for 2s, forcing one (same as stock app)")
                    self.credits = 1
            await self.client.write_gatt_char(TX, data[i : i + self.chunk], response=False)
            self.credits -= 1
            await asyncio.sleep(self.packet_delay)

    async def request(self, cmd: bytes, timeout: float = 2.0) -> bytes | None:
        while not self.rx.empty():
            self.rx.get_nowait()
        await self.write(cmd)
        try:
            return await asyncio.wait_for(self.rx.get(), timeout)
        except asyncio.TimeoutError:
            return None

    async def close(self) -> None:
        await self.client.disconnect()
