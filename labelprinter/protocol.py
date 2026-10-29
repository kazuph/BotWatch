"""Command builders for the P12-family protocol (PrintL11 / Printer_ESC in the stock app)."""
from __future__ import annotations

from PIL import Image

WAKEUP = bytes(15)                         # 15 x 0x00
ENABLE = bytes([0x10, 0xFF, 0xF1, 0x02])
STOP = bytes([0x10, 0xFF, 0xF1, 0x45])     # end of job
POSITION = bytes([0x1D, 0x0C])             # GS FF: feed to next label gap (die-cut labels)
STATUS = bytes([0x10, 0xFF, 0x40])
BATTERY = bytes([0x10, 0xFF, 0x50, 0xF1])
VERSION = bytes([0x10, 0xFF, 0x20, 0xF1])
MODEL = bytes([0x10, 0xFF, 0x20, 0xF0])
SERIAL = bytes([0x10, 0xFF, 0x20, 0xF2])
LEARN_GAP = bytes([0x10, 0xFF, 0x03])


def feed(dots: int) -> bytes:
    """ESC J n : feed n dot lines (0-255)."""
    return bytes([0x1B, 0x4A, max(0, min(255, dots))])


def set_density(level: int) -> bytes:
    return bytes([0x10, 0xFF, 0x10, 0x00, level & 0xFF])


def raster(img: Image.Image, mode: int = 0) -> bytes:
    """GS v 0 m xL xH yL yH d...  (1 = black, MSB = leftmost pixel)."""
    bw = img.convert("L").point(lambda v: 0 if v < 128 else 255, "1")
    w, h = bw.size
    wb = (w + 7) // 8
    # PIL "1" mode packs 1 = white; invert so 1 = black.
    data = bytes(b ^ 0xFF for b in bw.tobytes())
    if w % 8:  # clear padding bits that became black after inversion
        mask = (0xFF << (8 - w % 8)) & 0xFF
        data = bytes(b & mask if (i % wb) == wb - 1 else b for i, b in enumerate(data))
    header = bytes([0x1D, 0x76, 0x30, mode & 3, wb & 0xFF, wb >> 8, h & 0xFF, h >> 8])
    return header + data


LEAD_ROWS = 16  # 2mm blank before the image; see print_job


def print_job(img: Image.Image, tail_feed: int = 100, lead_rows: int = LEAD_ROWS) -> bytes:
    """Same byte sequence as ContinuousPaperPrintActivity.P12CoPaPrint.

    lead_rows blank rows are prepended: when the first rows are dense (e.g. a full-width
    border line) the paper feed stalls at start-up and ~30 rows overprint (measured).
    """
    if lead_rows > 0:
        padded = Image.new("L", (img.size[0], img.size[1] + lead_rows), 255)
        padded.paste(img.convert("L"), (0, lead_rows))
        img = padded
    return WAKEUP + ENABLE + raster(img) + feed(tail_feed) + STOP
