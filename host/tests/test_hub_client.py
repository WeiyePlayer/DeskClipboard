import hashlib
import json
import queue
from pathlib import Path
import tempfile
import threading
import time
import unittest
import uuid
from unittest import mock

from clipboard_agent.hub_client import Delivery, HubClient, load_config, save_config
from clipboard_agent.hub_protocol import HubError, make_upload
from clipboard_agent.hub_transport import upload
from clipboard_agent.pngutil import sample_png
from clipboard_agent.watcher import Capture


class FakeClipboard:
    def __init__(self):
        self.count = 1
        self.capture = None
        self.writes = []
    def change_count(self): return self.count
    def read(self): return self.capture
    def write(self, kind, data, job):
        self.writes.append((kind, data, job))
        self.capture = None  # Native adapter marks received copies.
        self.count += 1


class HubClientTest(unittest.TestCase):
    def setUp(self):
        self.config = {"device_id": str(uuid.uuid4()), "name": "Mac", "board_id": "test"}
        self.clip = FakeClipboard()
        self.client = HubClient(self.config, self.clip)

    def test_capture_own_write_and_real_copy(self):
        self.clip.capture = Capture("text", "中文🙂", b"", 0, 0)
        self.clip.count += 1
        self.client.poll_clipboard()
        self.assertEqual(self.client.pending.get_nowait().content, "中文🙂".encode())
        delivery = Delivery({"id": "job", "kind": "text"}, b"received", time.monotonic() + 2)
        self.client.deliveries.put(delivery)
        self.client.write_deliveries()
        self.client.poll_clipboard()
        self.assertTrue(delivery.success)
        self.assertTrue(self.client.pending.empty())
        self.clip.capture = Capture("text", "真正复制", b"", 0, 0)
        self.clip.count += 1
        self.client.poll_clipboard()
        self.assertEqual(self.client.pending.get_nowait().content, "真正复制".encode())

    def test_expired_delivery_never_writes_later(self):
        delivery = Delivery({"id": "old", "kind": "text"}, b"old", time.monotonic() - 1)
        self.client.deliveries.put(delivery)
        self.client.write_deliveries()
        self.assertFalse(delivery.success)
        self.assertEqual(self.clip.writes, [])

    def test_text_limit_and_image_capture_is_ignored(self):
        item = make_upload(self.config["device_id"], "Mac", text="中文🙂")
        self.assertEqual(item.content, "中文🙂".encode())
        with self.assertRaises(HubError):
            make_upload(self.config["device_id"], "Mac", text="中" * 21846)
        self.clip.capture = Capture("image", "", sample_png(), 48, 48)
        self.clip.count += 1
        self.client.poll_clipboard()
        self.assertTrue(self.client.pending.empty())
        delivery=Delivery({"id":"image","kind":"image"},sample_png(),time.monotonic()+2)
        self.client.deliveries.put(delivery);self.client.write_deliveries()
        self.assertFalse(delivery.success)
        self.assertEqual(self.clip.writes,[])

    def test_chunk_resume_and_idempotent_commit(self):
        item = make_upload(self.config["device_id"], "Mac", text="x" * 40000)
        class Transport:
            def __init__(self): self.chunks=[]
            def request(self, q):
                if q["op"] == "begin": return {"content_offset": 16384, "card_offset": 0}
                return {"record_id": 9}
            def chunk(self, event, part, offset, data): self.chunks.append((part, offset, data))
        t=Transport()
        self.assertEqual(upload(t, item), 9)
        self.assertEqual(t.chunks[0][1], 16384)
        self.assertEqual(b"".join(x[2] for x in t.chunks if x[0]=="content"), item.content[16384:])

    def test_config_has_no_wifi_secret_and_is_private(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/"client.json"
            config=dict(self.config, token="a"*64, transport="ble")
            save_config(p, config)
            self.assertEqual(load_config(p), config)
            self.assertNotIn("password", p.read_text())
            self.assertEqual(p.stat().st_mode & 0o777, 0o600)




if __name__ == "__main__": unittest.main()
