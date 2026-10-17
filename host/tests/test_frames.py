import unittest

from clipboard_agent.frames import (
    CARD_PUT,
    LIST_CLIPBOARD,
    REGION_BODY,
    REGION_CHIP,
    Decoder,
    Region,
    decode_card_put,
    decode_order,
    decode_tap,
    encode_card_put,
    encode_frame,
    encode_order,
    encode_tap,
)


class FrameTest(unittest.TestCase):
    def test_round_trip_and_crc_rejection(self):
        regions = [Region(REGION_BODY, 0, 0, 0, 10, 10), Region(REGION_CHIP, 0, 1, 2, 3, 4)]
        pixels = bytes([0x12, 0x34]) * 4
        encoded = encode_card_put(7, LIST_CLIPBOARD, 2, 2, regions, pixels)
        decoder = Decoder()
        frames, errors = decoder.feed(encoded)
        self.assertEqual(errors, 0)
        self.assertEqual(frames[0].type, CARD_PUT)
        clip_id, list_id, width, height, got, raw = decode_card_put(frames[0].payload)
        self.assertEqual((clip_id, list_id, width, height, raw), (7, LIST_CLIPBOARD, 2, 2, pixels))
        self.assertEqual(got, regions)

        broken = bytearray(encoded)
        broken[-1] ^= 0xFF
        frames, errors = Decoder().feed(bytes(broken))
        self.assertEqual(frames, [])
        self.assertEqual(errors, 1)

    def test_order_and_tap(self):
        encoded = encode_order(1, [3, 9])
        frames, _ = Decoder().feed(encoded)
        self.assertEqual(decode_order(frames[0].payload), (1, [3, 9]))
        frames, _ = Decoder().feed(encode_tap(4, REGION_CHIP, 2))
        self.assertEqual(decode_tap(frames[0].payload), (4, REGION_CHIP, 2))

    def test_split_chunks(self):
        blob = encode_frame(9, b"abc")
        decoder = Decoder()
        first, errors = decoder.feed(blob[:3])
        self.assertEqual((first, errors), ([], 0))
        second, errors = decoder.feed(blob[3:])
        self.assertEqual(errors, 0)
        self.assertEqual(second[0].payload, b"abc")


if __name__ == "__main__":
    unittest.main()
