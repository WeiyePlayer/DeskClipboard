import struct
import unittest
import zlib

from clipboard_agent.pngutil import encode_png, sample_png


class PngUtilTest(unittest.TestCase):
    def test_sample_is_48_square_png(self):
        png = sample_png()
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        length = struct.unpack(">I", png[8:12])[0]
        self.assertEqual(png[12:16], b"IHDR")
        width, height = struct.unpack(">II", png[16:24])
        self.assertEqual((width, height), (48, 48))
        self.assertEqual(length, 13)

    def test_round_trip_pixels(self):
        raw = bytes([10, 20, 30, 40, 50, 60])
        png = encode_png(2, 1, raw)
        idat = png.split(b"IDAT", 1)[1]
        size = struct.unpack(">I", png[png.index(b"IDAT") - 4 : png.index(b"IDAT")])[0]
        payload = idat[:size]
        decoded = zlib.decompress(payload)
        self.assertEqual(decoded, b"\x00" + raw)


if __name__ == "__main__":
    unittest.main()
