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


if __name__ == "__main__":
    unittest.main()
