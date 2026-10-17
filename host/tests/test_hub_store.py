"""Fault/reboot and quota tests against the firmware C code, under ASan/UBSan."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

from clipboard_agent.hub_protocol import make_upload

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "firmware/sim/build/store_driver"


class FirmwareStoreTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["bash", str(ROOT / "firmware/sim/build_store.sh")], check=True)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.process = subprocess.Popen([str(DRIVER), self.tmp.name], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.device = str(uuid.uuid4())

    def tearDown(self):
        self.process.stdin.close()
        self.process.wait(timeout=10)
        errors = self.process.stderr.read()
        self.process.stdout.close()
        self.process.stderr.close()
        self.tmp.cleanup()
        self.assertEqual(self.process.returncode, 0, errors)
        self.assertNotIn("runtime error:", errors)

    def call(self, op, **kwargs):
        self.process.stdin.write(json.dumps(dict(op=op, **kwargs), ensure_ascii=False) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        self.assertTrue(line, "storage driver exited")
        return json.loads(line)

    def item(self, text="中文\n🙂", **kwargs):
        return make_upload(self.device, "Mac", text=text, **kwargs)

    def stage(self, item):
        start = self.call("begin", metadata=item.metadata)
        self.assertTrue(start["ok"], start)
        for part, data in (("content", item.content), ("card", item.card)):
            for offset in range(0, len(data), 8192):
                r = self.call("chunk", event_id=item.metadata["event_id"], part=part,
                              offset=offset, hex=data[offset:offset + 8192].hex())
                self.assertTrue(r["ok"], r)

    def put(self, item):
        self.stage(item)
        r = self.call("commit", event_id=item.metadata["event_id"])
        self.assertTrue(r["ok"], r)
        return r

    def test_text_and_favorite_survive_restart(self):
        a = self.put(self.item())
        b = self.put(self.item("常用语", favorite=True))
        r = self.call("reopen")
        self.assertTrue(r["ready"])
        self.assertEqual(len(r["records"]), 2)
        self.assertTrue(next(v for v in r["records"] if v["id"]==b["record_id"])["favorite"])
        self.assertEqual((Path(self.tmp.name) / f'{a["record_id"]:08X}.DAT').read_bytes(), "中文\n🙂".encode())

    def test_interrupted_upload_is_not_published_and_is_removed(self):
        self.put(self.item("已有内容"))
        self.stage(self.item("未提交"))
        r = self.call("reopen")
        self.assertEqual(len(r["records"]), 1)
        self.assertFalse((Path(self.tmp.name) / "UPLOAD.DAT").exists())

    def test_checksum_failure_never_publishes(self):
        item = self.item()
        item.metadata["sha256"] = "0" * 64
        self.stage(item)
        r = self.call("commit", event_id=item.metadata["event_id"])
        self.assertEqual(r["error"], "checksum_mismatch")
        self.assertEqual(r["records"], [])

    def test_retry_is_idempotent_and_duplicate_preserves_favorite(self):
        item = self.item()
        a = self.put(item)
        self.call("favorite", id=a["record_id"], favorite=True)
        r = self.call("begin", metadata=item.metadata)
        self.assertEqual(r["record_id"], a["record_id"])
        r = self.call("begin", metadata=self.item().metadata)
        self.assertEqual(r["record_id"], a["record_id"])
        self.assertEqual(len(r["records"]), 1)
        self.assertTrue(r["records"][0]["favorite"])

    def test_count_limit_evicts_oldest_nonfavorite(self):
        a = self.put(self.item("收藏"))
        self.call("favorite", id=a["record_id"], favorite=True)
        old = self.put(self.item("最旧的非收藏"))["record_id"]
        for i in range(39):
            r = self.put(self.item(f"记录 {i}"))
        ids = [x["id"] for x in r["records"]]
        self.assertEqual(len(ids), 40)
        self.assertIn(a["record_id"], ids)
        self.assertNotIn(old, ids)
        self.assertLess(r["used_bytes"], 8 * 1024 * 1024)

    def test_all_favorites_reject_without_removal(self):
        for i in range(40):
            r = self.put(self.item(f"常用 {i}", favorite=True))
        result = self.call("begin", metadata=self.item("超出上限").metadata)
        self.assertEqual(result["error"], "storage_full")
        self.assertEqual(len(result["records"]), 40)

    def test_byte_limit_evicts_nonfavorite_then_refuses_full_favorites(self):
        from dataclasses import replace
        import struct
        ids=[]
        for seed in range(40):
            item=self.item(str(seed)+"x"*65530, favorite=seed==0)
            card=bytearray(item.card)
            struct.pack_into("<H",card,7,256)
            card.extend(b"\xff"*(10+card[9]*10+224*256*2-len(card)))
            metadata=dict(item.metadata,card_bytes=len(card),card_sha256=hashlib.sha256(card).hexdigest(),
                          chips=["a"*2048,"b"*2048,"c"*2048])
            item=replace(item,card=bytes(card),metadata=metadata)
            result=self.put(item)
            ids.append(result["record_id"])
        retained={r["id"] for r in result["records"]}
        self.assertLess(len(retained),40)
        self.assertIn(ids[0],retained)
        self.assertNotIn(ids[1],retained)
        self.assertLessEqual(result["used_bytes"],8*1024*1024)
        for record in retained:self.call("favorite",id=record,favorite=True)
        metadata=dict(item.metadata,event_id=str(uuid.uuid4()),sha256="a"*64)
        reply=self.call("begin",metadata=metadata)
        self.assertEqual(reply["error"],"storage_full")

    def test_clear_keeps_favorites_and_pause_survives_restart(self):
        a = self.put(self.item(favorite=True))
        self.put(self.item("普通"))
        self.call("clear")
        self.call("pause", paused=True)
        r = self.call("reopen")
        self.assertEqual([x["id"] for x in r["records"]], [a["record_id"]])
        self.assertEqual(self.call("begin", metadata=self.item("暂停").metadata)["error"], "paused")

    def test_torn_latest_catalog_falls_back(self):
        self.put(self.item())
        slots = list(Path(self.tmp.name).glob("INDEX*.BIN"))
        newest = max(slots, key=lambda p: int.from_bytes(p.read_bytes()[4:8], "little"))
        newest.write_bytes(b"torn")
        self.assertTrue(self.call("reopen")["ready"])

    def test_both_catalogs_corrupt_fail_closed(self):
        self.put(self.item())
        for p in Path(self.tmp.name).glob("INDEX*.BIN"):
            p.write_bytes(b"bad")
        r = self.call("reopen")
        self.assertFalse(r["ready"])
        self.assertTrue(any(Path(self.tmp.name).glob("*.DAT")))

    def test_offset_and_limit_validation(self):
        item = self.item()
        self.call("begin", metadata=item.metadata)
        r = self.call("chunk", event_id=item.metadata["event_id"], part="content", offset=1, hex="61")
        self.assertEqual(r["error"], "invalid_offset")
        self.call("abort")
        item.metadata["bytes"] = 65537
        self.assertEqual(self.call("begin", metadata=item.metadata)["error"], "too_large")

    def test_non_utf8_payload_rejected_even_with_matching_hash(self):
        from dataclasses import replace
        item=self.item("text")
        bad=b"\xff"*len(item.content)
        item=replace(item,content=bad,metadata=dict(item.metadata,sha256=hashlib.sha256(bad).hexdigest()))
        self.stage(item)
        self.assertEqual(self.call("commit",event_id=item.metadata["event_id"])["error"],"invalid_content")

    def test_image_upload_is_rejected(self):
        item=self.item()
        item.metadata["kind"]="image"
        self.assertEqual(self.call("begin",metadata=item.metadata)["error"],"invalid_kind")

    def test_extreme_metadata_numbers_fail_without_undefined_conversion(self):
        item=self.item()
        item.metadata["bytes"]=1e100
        self.assertEqual(self.call("begin",metadata=item.metadata)["error"],"too_large")

    def test_other_delete_does_not_remove_staging_files(self):
        old = self.put(self.item("旧内容"))["record_id"]
        item = self.item("正在上传")
        self.stage(item)
        self.call("delete", id=old)
        self.assertTrue(self.call("commit", event_id=item.metadata["event_id"])["ok"])


if __name__ == "__main__":
    unittest.main()
