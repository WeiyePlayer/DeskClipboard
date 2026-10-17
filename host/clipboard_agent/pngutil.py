"""Small RGB PNGs with no third-party image library."""

from __future__ import annotations

import struct
import zlib


def encode_png(width: int, height: int, rgb: bytes) -> bytes:
    if width <= 0 or height <= 0:
        raise ValueError("png size")
    if len(rgb) != width * height * 3:
        raise ValueError("rgb size")
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw.extend(rgb[y * stride : (y + 1) * stride])

    def chunk(tag: bytes, data: bytes) -> bytes:
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    compressed = zlib.compress(bytes(raw), 9)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", compressed) + chunk(b"IEND", b"")


def sample_png() -> bytes:
    """A 48×48 page icon: blue border, blue clip, cream page."""
    width = 48
    height = 48
    blue = (20, 90, 180)
    cream = (255, 248, 230)
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            border = x < 2 or y < 2 or x >= width - 2 or y >= height - 2
            clip = 8 <= y <= 14 and 16 <= x <= 32
            color = blue if border or clip else cream
            pixels.extend(color)
    return encode_png(width, height, bytes(pixels))
