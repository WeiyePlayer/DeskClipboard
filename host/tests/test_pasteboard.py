import unittest

from clipboard_agent.pasteboard import (
    SAMPLE_TEXT,
    read_png,
    read_text,
    read_tiff,
    restore,
    set_image_png,
    set_text,
    snapshot,
)
from clipboard_agent.pngutil import sample_png


class PasteboardTest(unittest.TestCase):
    def setUp(self):
        self.original = snapshot()

    def tearDown(self):
        restore(self.original)

    def test_text_and_image_round_trip_then_restore(self):
        set_text("restore-sentinel")
        saved = snapshot()
        set_text(SAMPLE_TEXT)
        self.assertEqual(read_text(), SAMPLE_TEXT)

        png = sample_png()
        set_image_png(png)
        self.assertEqual(read_png(), png)
        self.assertTrue(read_tiff())
        self.assertIsNone(read_text())

        restore(saved)
        self.assertEqual(read_text(), "restore-sentinel")


if __name__ == "__main__":
    unittest.main()
