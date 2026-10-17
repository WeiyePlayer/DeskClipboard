import unittest

from clipboard_agent.store import Store
from clipboard_agent.watcher import Capture, Watcher
import tempfile
from pathlib import Path


class FakeSource:
    def __init__(self):
        self.count = 1
        self.capture = None

    def change_count(self):
        return self.count

    def read(self):
        return self.capture


class WatcherTest(unittest.TestCase):
    def test_pause_and_suppress_own_write(self):
        tmp = tempfile.TemporaryDirectory()
        store = Store(Path(tmp.name))
        source = FakeSource()
        watcher = Watcher(store, source)
        source.count = 2
        source.capture = Capture("text", "新的一句", b"", 0, 0)
        clip = watcher.poll()
        self.assertEqual(clip.text, "新的一句")

        watcher.paused = True
        source.count = 3
        source.capture = Capture("text", "暂停期间", b"", 0, 0)
        self.assertIsNone(watcher.poll())
        self.assertIsNone(store.get(2) if False else next((c for c in store.history() if c.text == "暂停期间"), None))

        watcher.paused = False
        watcher.suppress = Capture("text", "自己贴的", b"", 0, 0).digest
        source.count = 4
        source.capture = Capture("text", "自己贴的", b"", 0, 0)
        self.assertIsNone(watcher.poll())
        store.close()
        tmp.cleanup()

    def test_paste_and_restore_do_not_reorder(self):
        import threading

        tmp = tempfile.TemporaryDirectory()
        store = Store(Path(tmp.name))
        source = FakeSource()
        watcher = Watcher(store, source)
        for text in ("一", "二", "三"):
            source.count += 1
            source.capture = Capture("text", text, b"", 0, 0)
            watcher.poll()
        order = [clip.id for clip in store.history()]
        tapped = store.history()[2]  # "一", the oldest

        # Paste from the board: write the tapped clip...
        with watcher.own_write():
            source.count += 1
            source.capture = Capture("text", tapped.text, b"", 0, 0)
        self.assertIsNone(watcher.poll())

        # ...and restore the previous clipboard ("三") from a timer thread,
        # with polls racing the write.
        def restore():
            with watcher.own_write():
                source.count += 1  # clearContents
                watcher.poll()
                source.count += 1  # writeObjects
                source.capture = Capture("text", "三", b"", 0, 0)

        thread = threading.Thread(target=restore)
        thread.start()
        thread.join()
        self.assertIsNone(watcher.poll())
        self.assertEqual([clip.id for clip in store.history()], order)

        # A real copy afterwards is still recorded.
        source.count += 1
        source.capture = Capture("text", "四", b"", 0, 0)
        self.assertEqual(watcher.poll().text, "四")
        store.close()
        tmp.cleanup()


if __name__ == "__main__":
    unittest.main()