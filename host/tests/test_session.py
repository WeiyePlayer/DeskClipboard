import tempfile
import unittest
from pathlib import Path
from unittest import mock

from clipboard_agent.agent import Session
from clipboard_agent.store import Store


class FlakyLink:
    """Fails the first `failures` sends with a missed ACK, then accepts."""

    def __init__(self, failures: int) -> None:
        self.failures = failures
        self.sent = 0
        self.alive = True

    def send(self, frame: bytes, timeout: float = 3) -> None:
        if self.failures > 0:
            self.failures -= 1
            raise TimeoutError("开发板没有确认这一帧")
        self.sent += 1


class SessionResilienceTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.tmp.name))
        self.store.add_text("一")

    def tearDown(self):
        self.store.close()
        self.tmp.cleanup()

    def test_missed_ack_marks_stale_instead_of_raising(self):
        session = Session(self.store, FlakyLink(failures=1))
        self.assertFalse(session.sync())
        self.assertTrue(session._stale)
        self.assertTrue(session.sync())
        self.assertFalse(session._stale)

    def test_publish_failure_does_not_raise(self):
        link = FlakyLink(failures=0)
        session = Session(self.store, link)
        self.assertTrue(session.sync())
        link.failures = 1
        session.publish(self.store.add_text("二"))
        self.assertTrue(session._stale)


    def test_toast_only_for_fixed_firmware(self):
        from clipboard_agent.frames import REGION_BODY
        from clipboard_agent.pasteboard import PasteResult

        clip = self.store.add_text("三")
        refused = PasteResult("accessibility", "refused")
        for version, expected in ((2, 0), (3, 1)):
            link = FlakyLink(failures=0)
            session = Session(self.store, link)
            session.board_version = version
            with mock.patch("clipboard_agent.agent.paste_content", return_value=refused):
                session._tap(clip.id, REGION_BODY, 0)
                session._tap(clip.id, REGION_BODY, 0)
            self.assertEqual(link.sent, 2 * expected, f"protocol {version}")


if __name__ == "__main__":
    unittest.main()
