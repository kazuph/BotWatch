"""Render text into a label bitmap oriented for the print head."""
from __future__ import annotations

from PIL import Image, ImageDraw, ImageFont

DOTS_PER_MM = 8
HEAD_DOTS = 96  # measured: only the first 96 dots (12mm) of each raster row reach the paper
FONT_CANDIDATES = [
    "/System/Library/Fonts/ヒラギノ角ゴシック W6.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
]


def _font(size: int) -> ImageFont.FreeTypeFont:
    for path in FONT_CANDIDATES:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default(size)


def text_label(text: str, border: bool = False, pad_mm: float = 3.0, head_dots: int = HEAD_DOTS) -> Image.Image:
    """Return an image whose width is the printable band in dots (text runs along the tape).

    pad_mm is the blank space between the label ends (or the border) and the text.
    """
    h = head_dots
    font = _font(int(h * (0.62 if border else 0.7)))
    l, t, r, b = font.getbbox(text)
    length = (r - l) + int(pad_mm * DOTS_PER_MM) * 2
    img = Image.new("L", (length, h), 255)
    d = ImageDraw.Draw(img)
    d.text(((length - (r - l)) // 2 - l, (h - (b - t)) // 2 - t), text, font=font, fill=0)
    if border:
        d.rectangle([0, 0, length - 1, h - 1], outline=0, width=2)
    return img.rotate(-90, expand=True)  # app rotates 90deg clockwise before sending
