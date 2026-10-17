"""Draw clipboard cards and chrome labels into little-endian RGB565."""

from __future__ import annotations

from dataclasses import dataclass

from clipboard_agent.extract import extract_chips
from clipboard_agent.frames import REGION_BODY, REGION_CHIP, REGION_MENU, Region

CARD_WIDTH = 224
_PAD = 10
_TEXT_SIZE = 15
_CHIP_SIZE = 13
_LINE = 20
_MAX_LINES = 2
_THUMB_H = 96
_THUMB_W = 140


@dataclass(frozen=True)
class Raster:
    width: int
    height: int
    rgb565: bytes
    regions: tuple[Region, ...] = ()


def render_text_card(text: str, chips: list[str] | None = None) -> Raster:
    pieces = extract_chips(text) if chips is None else chips
    text_height = _MAX_LINES * _LINE
    chip_block = 0
    if pieces:
        chip_block = 6 + len(pieces) * 28
    height = _PAD + text_height + chip_block + _PAD
    canvas = _Canvas(CARD_WIDTH, height, (255, 255, 255))
    canvas.round_rect(0, 0, CARD_WIDTH, height, 14, (255, 255, 255))
    canvas.text(text, _PAD, _PAD, CARD_WIDTH - 36, text_height, _TEXT_SIZE, (20, 20, 20))
    canvas.dots(CARD_WIDTH - 18, 18)
    regions = [
        Region(REGION_BODY, 0, 0, 0, CARD_WIDTH - 32, height),
        Region(REGION_MENU, 0, CARD_WIDTH - 32, 0, 32, 36),
    ]
    y = _PAD + text_height + 6
    for index, chip in enumerate(pieces):
        label = chip if len(chip) <= 28 else chip[:26] + "…"
        chip_w = min(CARD_WIDTH - 2 * _PAD, 12 + _text_width(label, _CHIP_SIZE))
        canvas.round_rect(_PAD, y, chip_w, 24, 8, (233, 233, 238))
        canvas.text(label, _PAD + 8, y + 3, chip_w - 16, 18, _CHIP_SIZE, (60, 60, 60))
        regions.append(Region(REGION_CHIP, index, _PAD, y, chip_w, 24))
        y += 28
    return Raster(CARD_WIDTH, height, canvas.rgb565(), tuple(regions))


def render_image_card(png: bytes, pixel_width: int, pixel_height: int) -> Raster:
    height = _PAD + _THUMB_H + 22 + _PAD
    canvas = _Canvas(CARD_WIDTH, height, (255, 255, 255))
    canvas.round_rect(0, 0, CARD_WIDTH, height, 14, (255, 255, 255))
    canvas.image(png, _PAD, _PAD, _THUMB_W, _THUMB_H)
    canvas.text(
        f"{pixel_width}×{pixel_height}",
        _PAD + _THUMB_W + 8,
        _PAD + 8,
        CARD_WIDTH - _THUMB_W - 40,
        24,
        _CHIP_SIZE,
        (90, 90, 90),
    )
    canvas.dots(CARD_WIDTH - 18, 18)
    regions = (
        Region(REGION_BODY, 0, 0, 0, CARD_WIDTH - 32, height),
        Region(REGION_MENU, 0, CARD_WIDTH - 32, 0, 32, 36),
    )
    return Raster(CARD_WIDTH, height, canvas.rgb565(), regions)


def render_label(text: str, background: tuple[int, int, int], size: int = 14) -> Raster:
    width = min(200, 16 + _text_width(text, size))
    height = size + 10
    canvas = _Canvas(width, height, background)
    canvas.text(text, 8, 4, width - 16, height - 6, size, (20, 20, 20))
    return Raster(width, height, canvas.rgb565())


def _text_width(text: str, size: int) -> int:
    from AppKit import NSFont, NSFontAttributeName, NSString

    font = NSFont.fontWithName_size_("PingFangSC-Regular", size) or NSFont.systemFontOfSize_(size)
    width = NSString.stringWithString_(text).sizeWithAttributes_({NSFontAttributeName: font}).width
    return int(width) + 8


class _Canvas:
    def __init__(self, width: int, height: int, background: tuple[int, int, int]) -> None:
        from AppKit import (
            NSBitmapImageRep,
            NSCalibratedRGBColorSpace,
            NSGraphicsContext,
        )

        self.width = width
        self.height = height
        self._rep = NSBitmapImageRep.alloc().initWithBitmapDataPlanes_pixelsWide_pixelsHigh_bitsPerSample_samplesPerPixel_hasAlpha_isPlanar_colorSpaceName_bytesPerRow_bitsPerPixel_(
            None,
            width,
            height,
            8,
            4,
            True,
            False,
            NSCalibratedRGBColorSpace,
            width * 4,
            32,
        )
        red, green, blue = (channel / 255 for channel in background)
        self._rep.bitmapData()  # ensure buffer exists
        # The CTM below is flipped so y grows downward like the device screen.
        # AppKit only draws glyphs and images upright in a flipped CTM when the
        # context itself is marked flipped; without it every glyph is upside down.
        import Quartz  # noqa: F401  (bridges CGContextRef for the call below)

        bitmap_context = NSGraphicsContext.graphicsContextWithBitmapImageRep_(self._rep)
        context = NSGraphicsContext.graphicsContextWithCGContext_flipped_(
            bitmap_context.CGContext(), True
        )
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.setCurrentContext_(context)
        from AppKit import NSColor

        NSColor.colorWithCalibratedRed_green_blue_alpha_(red, green, blue, 1).set()
        from AppKit import NSBezierPath, NSMakeRect

        NSBezierPath.fillRect_(NSMakeRect(0, 0, width, height))
        self._flip(height)
        self._open = True

    def _flip(self, height: int) -> None:
        from AppKit import NSAffineTransform

        transform = NSAffineTransform.transform()
        transform.translateXBy_yBy_(0, height)
        transform.scaleXBy_yBy_(1, -1)
        transform.concat()

    def round_rect(
        self,
        x: int,
        y: int,
        width: int,
        height: int,
        radius: int,
        color: tuple[int, int, int],
    ) -> None:
        from AppKit import NSBezierPath, NSColor, NSMakeRect

        red, green, blue = (channel / 255 for channel in color)
        NSColor.colorWithCalibratedRed_green_blue_alpha_(red, green, blue, 1).set()
        path = NSBezierPath.bezierPathWithRoundedRect_xRadius_yRadius_(
            NSMakeRect(x, y, width, height), radius, radius
        )
        path.fill()

    def text(
        self,
        text: str,
        x: int,
        y: int,
        width: int,
        height: int,
        size: int,
        color: tuple[int, int, int],
    ) -> None:
        from AppKit import (
            NSColor,
            NSFont,
            NSFontAttributeName,
            NSForegroundColorAttributeName,
            NSMakeRect,
            NSMutableParagraphStyle,
            NSParagraphStyleAttributeName,
            NSString,
        )

        style = NSMutableParagraphStyle.alloc().init()
        style.setLineBreakMode_(4)  # truncating tail
        style.setMaximumLineHeight_(size + 5)
        font = NSFont.fontWithName_size_("PingFangSC-Regular", size) or NSFont.systemFontOfSize_(size)
        red, green, blue = (channel / 255 for channel in color)
        attributes = {
            NSFontAttributeName: font,
            NSForegroundColorAttributeName: NSColor.colorWithCalibratedRed_green_blue_alpha_(red, green, blue, 1),
            NSParagraphStyleAttributeName: style,
        }
        NSString.stringWithString_(text).drawInRect_withAttributes_(
            NSMakeRect(x, y, width, height), attributes
        )

    def dots(self, x: int, y: int) -> None:
        from AppKit import NSBezierPath, NSColor

        NSColor.colorWithCalibratedWhite_alpha_(0.25, 1).set()
        for offset in (-6, 0, 6):
            NSBezierPath.bezierPathWithOvalInRect_(_rect(x - 1.5, y + offset - 1.5, 3, 3)).fill()

    def image(self, png: bytes, x: int, y: int, width: int, height: int) -> None:
        from AppKit import NSData, NSImage, NSMakeRect

        picture = NSImage.alloc().initWithData_(NSData.dataWithBytes_length_(png, len(png)))
        if picture is None:
            return
        picture.drawInRect_fromRect_operation_fraction_respectFlipped_hints_(
            NSMakeRect(x, y, width, height),
            _zero_rect(),
            1,
            1.0,
            True,
            None,
        )

    def rgb565(self) -> bytes:
        from AppKit import NSGraphicsContext

        if self._open:
            NSGraphicsContext.restoreGraphicsState()
            self._open = False
        raw = bytes(self._rep.bitmapData()[: self.width * self.height * 4])
        out = bytearray(self.width * self.height * 2)
        for index in range(self.width * self.height):
            red = raw[index * 4]
            green = raw[index * 4 + 1]
            blue = raw[index * 4 + 2]
            pixel = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
            out[index * 2] = pixel & 0xFF
            out[index * 2 + 1] = pixel >> 8
        return bytes(out)


def _rect(x: float, y: float, width: float, height: float):
    from AppKit import NSMakeRect

    return NSMakeRect(x, y, width, height)


def _zero_rect():
    from AppKit import NSZeroRect

    return NSZeroRect
