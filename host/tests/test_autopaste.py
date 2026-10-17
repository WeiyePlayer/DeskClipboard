"""Auto-paste decisions with fake key backends: no real keystroke is ever sent."""
import tempfile
import time
from pathlib import Path
import unittest
import uuid

from clipboard_agent import autopaste
from clipboard_agent.autopaste import AutoPaster
from clipboard_agent.hub_client import Delivery, HubClient, main
from clipboard_agent.watcher import Capture
from test_hub_client import FakeClipboard


class FakeKeys:
    def __init__(self, blocker=None, sent_ok=True, trusted=True):
        self.blocked, self.sent_ok, self.trusted = blocker, sent_ok, trusted
        self.sent = 0
        self.prompts = 0
    def prepare(self):
        self.prompts += 1
        return None if self.trusted else autopaste.ACCESSIBILITY
    def blocker(self): return self.blocked
    def send(self):
        self.sent += 1
        return self.sent_ok


def paster(keys, **kw):
    return AutoPaster(keys, sleep=lambda s: None, **kw)


class DecisionTest(unittest.TestCase):
    def test_pastes_when_allowed(self):
        keys = FakeKeys()
        self.assertEqual(paster(keys).paste(time.monotonic() + 2), autopaste.PASTED)
        self.assertEqual(keys.sent, 1)

    def test_disabled_never_sends(self):
        keys = FakeKeys()
        p = paster(keys, enabled=False)
        self.assertEqual(p.paste(time.monotonic() + 2), autopaste.OFF)
        self.assertIsNone(p.prepare())
        self.assertEqual((keys.sent, keys.prompts), (0, 0))

    def test_blockers_keep_clipboard_and_skip_keystroke(self):
        for blocker in (autopaste.ACCESSIBILITY, autopaste.SECURE_INPUT):
            keys = FakeKeys(blocker=blocker)
            self.assertEqual(paster(keys).paste(time.monotonic() + 2), blocker)
            self.assertEqual(keys.sent, 0)
            for platform in ("darwin", "win32"):
                self.assertIn("剪贴板", autopaste.message(blocker, platform))
        self.assertIn("辅助功能", autopaste.message(autopaste.ACCESSIBILITY, "darwin"))

    def test_refused_keystroke_is_reported(self):
        self.assertEqual(paster(FakeKeys(sent_ok=False)).paste(time.monotonic() + 2), autopaste.BLOCKED)

    def test_no_keystroke_after_deadline(self):
        keys = FakeKeys()
        self.assertEqual(paster(keys).paste(time.monotonic() - 0.01), autopaste.LATE)
        self.assertEqual(keys.sent, 0)

    def test_prepare_reports_missing_permission(self):
        keys = FakeKeys(trusted=False)
        self.assertEqual(paster(keys).prepare(), autopaste.ACCESSIBILITY)
        self.assertEqual(keys.prompts, 1)


class ClientTest(unittest.TestCase):
    def make(self, keys, enabled=True):
        self.clip = FakeClipboard()
        config = {"device_id": str(uuid.uuid4()), "name": "Mac", "board_id": "t"}
        return HubClient(config, self.clip, paster=paster(keys, enabled=enabled))

    def deliver(self, client, expires_in=2):
        d = Delivery({"id": str(uuid.uuid4()), "kind": "text"}, "收到🙂".encode(), time.monotonic() + expires_in)
        client.deliveries.put(d)
        client.write_deliveries()
        return d

    def test_write_then_paste_and_not_reuploaded(self):
        keys = FakeKeys()
        client = self.make(keys)
        d = self.deliver(client)
        self.assertTrue(d.success)
        self.assertEqual(d.paste, autopaste.PASTED)
        self.assertEqual(len(self.clip.writes), 1)
        self.assertEqual(keys.sent, 1)
        client.poll_clipboard()  # own marked write is not uploaded again
        self.assertTrue(client.pending.empty())
        # No restore of the previous clipboard: exactly one clipboard change.
        self.assertEqual(self.clip.count, 2)

    def test_expired_delivery_neither_written_nor_pasted(self):
        keys = FakeKeys()
        client = self.make(keys)
        d = self.deliver(client, expires_in=-1)
        self.assertFalse(d.success)
        self.assertEqual((self.clip.writes, keys.sent), ([], 0))

    def test_duplicate_job_is_not_pasted_twice(self):
        keys = FakeKeys()
        client = self.make(keys)
        d = self.deliver(client)
        again = Delivery(d.job, d.data, time.monotonic() + 2)
        client.deliveries.put(again)
        client.write_deliveries()
        self.assertFalse(again.success)
        self.assertEqual(keys.sent, 1)

    def test_blocked_keeps_clipboard_and_reports(self):
        keys = FakeKeys(blocker=autopaste.ACCESSIBILITY)
        d = self.deliver(self.make(keys))
        self.assertTrue(d.success)
        self.assertEqual(d.paste, autopaste.ACCESSIBILITY)
        self.assertEqual(len(self.clip.writes), 1)

    def test_paste_exception_does_not_fail_delivery(self):
        class Boom(FakeKeys):
            def send(self): raise OSError("x")
        d = self.deliver(self.make(Boom()))
        self.assertTrue(d.success)
        self.assertEqual(d.paste, autopaste.BLOCKED)

    def test_ack_carries_paste_outcome(self):
        keys = FakeKeys()
        client = self.make(keys)
        job = {"id": "j" * 32, "kind": "text", "bytes": 3,
               "sha256": __import__("hashlib").sha256(b"abc").hexdigest()}
        sent = []
        class Transport:
            def request(self, q):
                sent.append(q)
                if q["op"] == "poll": return {"job": job}
                if q["op"] == "ready": return {"lease_ms": 2000}
                return {"ok": True}
            def download(self, j, offset, size): return b"abc"[offset:offset + size]
        client.transport = Transport()
        import threading
        worker = threading.Thread(target=client.receive)
        worker.start()
        deadline = time.monotonic() + 2
        while worker.is_alive() and time.monotonic() < deadline:
            client.write_deliveries(); time.sleep(0.01)
        worker.join(1)
        ack = [q for q in sent if q["op"] == "ack"][0]
        self.assertEqual((ack["success"], ack["paste"]), (True, autopaste.PASTED))

    def test_flag_turns_auto_paste_off(self):
        import clipboard_agent.hub_client as hc
        seen = {}
        class Stub:
            def __init__(self, config, paster=None): seen["enabled"] = paster.enabled
            def run(self): pass
        orig = (hc.HubClient, hc.load_config)
        hc.HubClient = Stub
        try:
            for argv, cfg, want in (([], {}, True), (["--no-auto-paste"], {}, False), ([], {"auto_paste": False}, False)):
                hc.load_config = lambda p, cfg=cfg: dict(cfg, device_id="d", board_id="b", token="t", name="Mac")
                with tempfile.TemporaryDirectory() as tmp:  # never the user's real lock
                    main(argv + ["--config", str(Path(tmp) / "client-v4.json")])
                self.assertEqual(seen["enabled"], want)
            self.assertTrue(autopaste.native_paster(True).enabled or __import__("sys").platform not in ("darwin", "win32"))
        finally:
            hc.HubClient, hc.load_config = orig


if __name__ == "__main__":
    unittest.main()
