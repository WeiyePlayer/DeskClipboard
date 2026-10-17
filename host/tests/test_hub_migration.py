import hashlib
import sqlite3
from pathlib import Path
import tempfile
import unittest
import uuid
from unittest.mock import patch

from clipboard_agent.hub_client import migrate
from clipboard_agent.store import Store
from clipboard_agent.pngutil import sample_png


class MigrationTest(unittest.TestCase):
    def test_favorites_first_recent_selection_and_original_unchanged(self):
        config = {"device_id": str(uuid.uuid4()), "name": "Mac", "board_id": "board"}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            store = Store(root)
            favorite = store.add_text("旧收藏")
            store.set_favorite(favorite.id, True)
            store.add_text("普通旧内容")
            store.add_text("普通新内容")
            old_image=store.add_image(sample_png(),48,48)
            store.set_favorite(old_image.id,True)
            store.close()
            original = (root / "clips.db").read_bytes()
            imported = []
            class Transport:
                def request(self, data):
                    if data["op"] == "status": return {"used_bytes": 0}
                    return {"records": [{"kind":"text", "sha256":str(i)} for i in range(38)]}
            with patch("clipboard_agent.hub_client.upload", side_effect=lambda t, item: imported.append(item)):
                migrate(Transport(), config, root)
            self.assertEqual([i.content.decode() for i in imported], ["旧收藏", "普通新内容"])
            self.assertTrue(imported[0].metadata["favorite"])
            self.assertEqual((root / "clips.db").read_bytes(), original)
            self.assertEqual((root / "images" / f'{old_image.id}.png').read_bytes(),sample_png())
            with sqlite3.connect(root / "clips.db") as source, sqlite3.connect(root / "migration-v4-backup/clips.db") as snapshot:
                self.assertEqual(source.execute("SELECT * FROM clips ORDER BY id").fetchall(),
                                 snapshot.execute("SELECT * FROM clips ORDER BY id").fetchall())
            first_events = [i.metadata["event_id"] for i in imported]
            imported.clear()
            with patch("clipboard_agent.hub_client.upload", side_effect=lambda t, item: imported.append(item)):
                migrate(Transport(), config, root)
            self.assertEqual([i.metadata["event_id"] for i in imported], first_events)

    def test_storage_full_preserves_source_without_upload_or_eviction(self):
        config = {"device_id":str(uuid.uuid4()),"name":"Mac","board_id":"board"}
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);store=Store(root);store.add_text("留在原数据库");store.close()
            digest=hashlib.sha256((root/"clips.db").read_bytes()).digest()
            class Full:
                def request(self,data):
                    return {"used_bytes":8*1024*1024} if data["op"]=="status" else {"records":[]}
            with patch("clipboard_agent.hub_client.upload") as upload:
                migrate(Full(),config,root)
                upload.assert_not_called()
            self.assertEqual(hashlib.sha256((root/"clips.db").read_bytes()).digest(),digest)


if __name__=="__main__":unittest.main()
