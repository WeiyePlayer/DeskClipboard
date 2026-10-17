"""Windows card drawing with the same geometry/RGB565 as the Mac renderer."""
from __future__ import annotations

import io
import os
from functools import lru_cache
from PIL import Image, ImageDraw, ImageFont


@lru_cache(maxsize=16)
def font(size: int):
    choices = [os.environ.get("DESKCLIPBOARD_FONT", ""),
               "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
               "/System/Library/Fonts/PingFang.ttc",
               "/System/Library/Fonts/STHeiti Light.ttc"]
    for path in choices:
        if path and os.path.isfile(path):
            return ImageFont.truetype(path, size)
    raise RuntimeError("缺少中文字体，请设置 DESKCLIPBOARD_FONT 指向可用的中文字体")


def text_width(text: str, size: int) -> int:
    return int(font(size).getlength(text)) + 8


class PillowCanvas:
    def __init__(self, width: int, height: int, background) -> None:
        self.width, self.height = width, height
        self.picture = Image.new("RGB", (width, height), background)
        self.draw = ImageDraw.Draw(self.picture)

    def round_rect(self, x, y, width, height, radius, color):
        self.draw.rounded_rectangle((x, y, x + width - 1, y + height - 1), radius, fill=color)

    def text(self, text, x, y, width, height, size, color):
        face = font(size)
        step = size + 5
        count = max(1, height // step)
        remaining = text.replace("\r\n", "\n").replace("\r", "\n")
        for row in range(count):
            if not remaining:
                break
            paragraph, separator, tail = remaining.partition("\n")
            final = row == count - 1
            needs_ellipsis = final and (bool(separator) or face.getlength(paragraph) > width)
            suffix = "…" if needs_ellipsis else ""
            low, high = 0, len(paragraph)
            while low < high:
                middle = (low + high + 1) // 2
                if face.getlength(paragraph[:middle] + suffix) <= width:
                    low = middle
                else:
                    high = middle - 1
            # A single wide glyph still consumes input; previews never loop.
            take = low if low or not paragraph else 1
            line = paragraph[:take] + suffix
            self.draw.text((x, y + row * step), line, font=face, fill=color, anchor="lt")
            remaining = (paragraph[take:] + ("\n" + tail if separator else "")) if take < len(paragraph) else tail

    def dots(self, x, y):
        for offset in (-6, 0, 6):
            self.draw.ellipse((x - 1, y + offset - 1, x + 1, y + offset + 1), fill=(64, 64, 64))

    def image(self, png, x, y, width, height):
        with Image.open(io.BytesIO(png)) as source:
            thumb = source.convert("RGBA").resize((width, height), Image.Resampling.LANCZOS)
            self.picture.paste(thumb, (x, y), thumb.getchannel("A"))

    def rgb565(self):
        raw = self.picture.tobytes()
        result = bytearray(self.width * self.height * 2)
        for i in range(self.width * self.height):
            r, g, b = raw[i * 3:i * 3 + 3]
            value = ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3)
            result[i * 2:i * 2 + 2] = value.to_bytes(2, "little")
        return bytes(result)
