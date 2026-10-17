import unittest
from pathlib import Path
import tempfile

from clipboard_agent.pngutil import sample_png
from clipboard_agent.store import HISTORY_LIMIT, Store


class StoreTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.tmp.name))

    def tearDown(self):
        self.store.close()
        self.tmp.cleanup()

    def test_dedup_favorite_and_clear(self):
        first = self.store.add_text("同一句")
        second = self.store.add_text("同一句")
        self.assertEqual(first.id, second.id)
        self.assertEqual(len(self.store.history()), 1)
        self.store.set_favorite(first.id, True)
        self.store.add_text("另一句")
        removed = self.store.clear_history()
        self.assertEqual(len(removed), 1)
        self.assertEqual([clip.text for clip in self.store.favorites()], ["同一句"])
        self.assertTrue(self.store.get(first.id).favorite)

    def test_image_round_trip_and_size_limit(self):
        png = sample_png()
        clip = self.store.add_image(png, 48, 48)
        self.assertEqual(self.store.image_bytes(clip), png)
        self.assertIsNone(self.store.add_image(b"x" * (20 * 1024 * 1024 + 1), 1, 1))

    def test_history_limit_keeps_favorites(self):
        kept = self.store.add_text("收藏")
        self.store.set_favorite(kept.id, True)
        for index in range(HISTORY_LIMIT + 5):
            self.store.add_text(f"普通{index}")
        ids = {clip.id for clip in self.store.history()}
        self.assertLessEqual(len(self.store.history()), HISTORY_LIMIT)
        self.assertIn(kept.id, {clip.id for clip in self.store.favorites()})
        self.assertTrue(kept.id in ids or self.store.get(kept.id) is not None)


if __name__ == "__main__":
    unittest.main()
