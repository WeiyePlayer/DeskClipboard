import unittest

from clipboard_agent.frames import REGION_CHIP, REGION_MENU
from clipboard_agent.pngutil import sample_png
from clipboard_agent.render import CARD_WIDTH, render_image_card, render_label, render_text_card


class RenderTest(unittest.TestCase):
    def test_text_card_has_phone_chip(self):
        card = render_text_card("地址 13558801181")
        self.assertEqual(card.width, CARD_WIDTH)
        self.assertEqual(len(card.rgb565), card.width * card.height * 2)
        chips = [region for region in card.regions if region.kind == REGION_CHIP]
        self.assertEqual(len(chips), 1)
        self.assertTrue(any(region.kind == REGION_MENU for region in card.regions))

    def test_image_card_and_label(self):
        card = render_image_card(sample_png(), 48, 48)
        self.assertEqual(len(card.rgb565), card.width * card.height * 2)
        label = render_label("剪贴板", (255, 255, 255))
        self.assertGreater(label.width, 10)
        self.assertEqual(len(label.rgb565), label.width * label.height * 2)

    def test_glyphs_and_images_are_upright(self):
        # Regression: the flipped CTM without a flipped context drew every
        # glyph and thumbnail upside down on the board.
        def pixel(raster, x, y):
            index = (y * raster.width + x) * 2
            return raster.rgb565[index] | (raster.rgb565[index + 1] << 8)

        def dark(value):
            return ((value >> 11) & 31) < 12 and ((value >> 5) & 63) < 24

        label = render_label("T", (255, 255, 255), 20)
        rows = [sum(dark(pixel(label, x, y)) for x in range(label.width)) for y in range(label.height)]
        inked = [y for y, count in enumerate(rows) if count]
        widest = max(inked, key=lambda y: rows[y])
        self.assertLess(widest, (inked[0] + inked[-1]) / 2, "T crossbar should be at the top")

        card = render_image_card(sample_png(), 48, 48)
        # sample_png has its blue clip near the top (rows 8-14 of 48).
        blue = pixel(card, 80, 33)
        mirrored = pixel(card, 80, 83)
        self.assertLess((blue >> 11) & 31, 8)
        self.assertGreater((mirrored >> 11) & 31, 24)

    def test_toast_text_is_visible(self):
        import struct

        from clipboard_agent.catalog import toast_frame
        from clipboard_agent.frames import Decoder

        frames, _ = Decoder().feed(toast_frame("已粘贴"))
        payload = frames[0].payload
        pixels = payload[6:]
        reds = {(pixels[i] | (pixels[i + 1] << 8)) >> 11 for i in range(0, len(pixels), 2)}
        # Background is dark; the glyphs must be clearly lighter.
        self.assertLess(min(reds), 8)
        self.assertGreater(max(reds), 24)


if __name__ == "__main__":
    unittest.main()
