"""Binary frames between the Mac and the board.

Layout: magic 0xC1, type u8, length u32 little-endian, payload, CRC32 of
type+length+payload.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

MAGIC = 0xC1
MAX_PAYLOAD = 512 * 1024

CARD_PUT = 0x01
CARD_DEL = 0x02
LIST_ORDER = 0x03
ASSET_PUT = 0x04
TOAST = 0x05
ENTER_BOOT = 0x06
VERIFY_TAP = 0x07

TAP = 0x10
MENU = 0x11
PAGE_ACTION = 0x12
HELLO = 0x13
ACK = 0x14
NACK = 0x15
CARD_DROP = 0x16

LIST_CLIPBOARD = 0
LIST_FAVORITES = 1

REGION_BODY = 0
REGION_CHIP = 1
REGION_MENU = 2

PAGE_TOP = 1
PAGE_CLEAR = 2
PAGE_PAUSE = 3
PAGE_RESUME = 4

MENU_FAVORITE = 1
MENU_UNFAVORITE = 2
MENU_DELETE = 3

ASSET_TAB_CLIP_ON = 1
ASSET_TAB_CLIP_OFF = 2
ASSET_TAB_FAV_ON = 3
ASSET_TAB_FAV_OFF = 4
ASSET_CLEAR = 5
ASSET_PAUSE = 6
ASSET_RESUME = 7
ASSET_FAVORITE = 8
ASSET_UNFAVORITE = 9
ASSET_DELETE = 10
ASSET_CONFIRM = 11
ASSET_CANCEL = 12
ASSET_CONFIRM_TITLE = 13

PROTOCOL_VERSION = 3


@dataclass(frozen=True)
class Frame:
    type: int
    payload: bytes


@dataclass(frozen=True)
class Region:
    kind: int
    chip_index: int
    x: int
    y: int
    w: int
    h: int


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def encode_frame(frame_type: int, payload: bytes = b"") -> bytes:
    if frame_type < 0 or frame_type > 255:
        raise ValueError("frame type")
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    body = struct.pack("<BI", frame_type, len(payload)) + payload
    return bytes([MAGIC]) + body + struct.pack("<I", crc32(body))


class Decoder:
    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, data: bytes) -> tuple[list[Frame], int]:
        """Return decoded frames and how many frames failed CRC."""
        self._buffer.extend(data)
        frames: list[Frame] = []
        errors = 0
        while self._buffer:
            if self._buffer[0] != MAGIC:
                del self._buffer[0]
                continue
            if len(self._buffer) < 6:
                break
            frame_type, length = struct.unpack_from("<BI", self._buffer, 1)
            if length > MAX_PAYLOAD:
                del self._buffer[0]
                continue
            total = 6 + length + 4
            if len(self._buffer) < total:
                break
            body = bytes(self._buffer[1 : 6 + length])
            digest = struct.unpack_from("<I", self._buffer, 6 + length)[0]
            del self._buffer[:total]
            if crc32(body) != digest:
                errors += 1
                continue
            frames.append(Frame(frame_type, body[5:]))
        return frames, errors


def encode_region(region: Region) -> bytes:
    return struct.pack(
        "<BBHHHH",
        region.kind,
        region.chip_index,
        region.x,
        region.y,
        region.w,
        region.h,
    )


def encode_card_put(
    clip_id: int,
    list_id: int,
    width: int,
    height: int,
    regions: list[Region],
    pixels: bytes,
) -> bytes:
    if len(pixels) != width * height * 2:
        raise ValueError("rgb565 size")
    if len(regions) > 8:
        raise ValueError("too many regions")
    payload = struct.pack("<IBHHB", clip_id, list_id, width, height, len(regions))
    payload += b"".join(encode_region(region) for region in regions)
    payload += pixels
    return encode_frame(CARD_PUT, payload)


def decode_card_put(payload: bytes) -> tuple[int, int, int, int, list[Region], bytes]:
    clip_id, list_id, width, height, count = struct.unpack_from("<IBHHB", payload)
    offset = 10
    regions: list[Region] = []
    for _ in range(count):
        kind, chip_index, x, y, w, h = struct.unpack_from("<BBHHHH", payload, offset)
        regions.append(Region(kind, chip_index, x, y, w, h))
        offset += 10
    pixels = payload[offset:]
    if len(pixels) != width * height * 2:
        raise ValueError("rgb565 size")
    return clip_id, list_id, width, height, regions, pixels


def encode_order(list_id: int, ids: list[int]) -> bytes:
    payload = struct.pack("<BH", list_id, len(ids)) + b"".join(
        struct.pack("<I", clip_id) for clip_id in ids
    )
    return encode_frame(LIST_ORDER, payload)


def decode_order(payload: bytes) -> tuple[int, list[int]]:
    list_id, count = struct.unpack_from("<BH", payload)
    ids = [
        struct.unpack_from("<I", payload, 3 + index * 4)[0] for index in range(count)
    ]
    return list_id, ids


def encode_asset(asset_id: int, width: int, height: int, pixels: bytes) -> bytes:
    payload = struct.pack("<BHH", asset_id, width, height) + pixels
    return encode_frame(ASSET_PUT, payload)


def encode_toast(duration_ms: int, width: int, height: int, pixels: bytes) -> bytes:
    payload = struct.pack("<HHH", duration_ms, width, height) + pixels
    return encode_frame(TOAST, payload)


def encode_tap(clip_id: int, kind: int, chip_index: int) -> bytes:
    return encode_frame(TAP, struct.pack("<IBB", clip_id, kind, chip_index))


def decode_tap(payload: bytes) -> tuple[int, int, int]:
    return struct.unpack("<IBB", payload)


def encode_menu(clip_id: int, action: int) -> bytes:
    return encode_frame(MENU, struct.pack("<IB", clip_id, action))


def decode_menu(payload: bytes) -> tuple[int, int]:
    return struct.unpack("<IB", payload)


def encode_page(action: int) -> bytes:
    return encode_frame(PAGE_ACTION, struct.pack("<B", action))


def decode_page(payload: bytes) -> int:
    return struct.unpack("<B", payload)[0]
