"""SQLite history and favorites. Images live beside the database."""

from __future__ import annotations

import hashlib
import sqlite3
import time
from dataclasses import dataclass
from pathlib import Path

HISTORY_LIMIT = 200
MAX_IMAGE_BYTES = 20 * 1024 * 1024


@dataclass(frozen=True)
class Clip:
    id: int
    kind: str
    text: str
    image_path: str | None
    width: int
    height: int
    created_at: float
    favorite: bool


class Store:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.root.mkdir(parents=True, exist_ok=True)
        self.images = self.root / "images"
        self.images.mkdir(exist_ok=True)
        self._db = sqlite3.connect(self.root / "clips.db")
        self._db.row_factory = sqlite3.Row
        self._db.execute(
            """
            CREATE TABLE IF NOT EXISTS clips (
                id INTEGER PRIMARY KEY,
                kind TEXT NOT NULL,
                text TEXT NOT NULL DEFAULT '',
                image_path TEXT,
                width INTEGER NOT NULL DEFAULT 0,
                height INTEGER NOT NULL DEFAULT 0,
                hash TEXT NOT NULL UNIQUE,
                created_at REAL NOT NULL,
                favorite INTEGER NOT NULL DEFAULT 0,
                favorited_at REAL
            )
            """
        )
        self._db.commit()

    def close(self) -> None:
        self._db.close()

    def add_text(self, text: str) -> Clip:
        return self._add("text", text, None, 0, 0, hashlib.sha256(text.encode()).hexdigest())

    def add_image(self, png: bytes, width: int, height: int) -> Clip | None:
        if len(png) > MAX_IMAGE_BYTES or len(png) == 0:
            return None
        digest = hashlib.sha256(png).hexdigest()
        existing = self._by_hash(digest)
        if existing is not None:
            self._bump(existing.id)
            return self.get(existing.id)
        clip = self._add("image", "", None, width, height, digest)
        path = self.images / f"{clip.id}.png"
        path.write_bytes(png)
        self._db.execute("UPDATE clips SET image_path = ? WHERE id = ?", (str(path), clip.id))
        self._db.commit()
        return self.get(clip.id)

    def get(self, clip_id: int) -> Clip | None:
        row = self._db.execute("SELECT * FROM clips WHERE id = ?", (clip_id,)).fetchone()
        return None if row is None else self._row(row)

    def image_bytes(self, clip: Clip) -> bytes | None:
        if not clip.image_path:
            return None
        path = Path(clip.image_path)
        if not path.is_file():
            return None
        return path.read_bytes()

    def history(self) -> list[Clip]:
        rows = self._db.execute(
            "SELECT * FROM clips ORDER BY created_at DESC LIMIT ?",
            (HISTORY_LIMIT,),
        ).fetchall()
        return [self._row(row) for row in rows]

    def favorites(self) -> list[Clip]:
        rows = self._db.execute(
            "SELECT * FROM clips WHERE favorite = 1 ORDER BY favorited_at DESC"
        ).fetchall()
        return [self._row(row) for row in rows]

    def set_favorite(self, clip_id: int, favorite: bool) -> None:
        if favorite:
            self._db.execute(
                "UPDATE clips SET favorite = 1, favorited_at = ? WHERE id = ?",
                (time.time(), clip_id),
            )
        else:
            self._db.execute(
                "UPDATE clips SET favorite = 0, favorited_at = NULL WHERE id = ?",
                (clip_id,),
            )
        self._db.commit()

    def delete(self, clip_id: int) -> None:
        clip = self.get(clip_id)
        if clip and clip.image_path:
            Path(clip.image_path).unlink(missing_ok=True)
        self._db.execute("DELETE FROM clips WHERE id = ?", (clip_id,))
        self._db.commit()

    def clear_history(self) -> list[int]:
        rows = self._db.execute("SELECT id, image_path FROM clips WHERE favorite = 0").fetchall()
        ids = [int(row["id"]) for row in rows]
        for row in rows:
            if row["image_path"]:
                Path(row["image_path"]).unlink(missing_ok=True)
        self._db.execute("DELETE FROM clips WHERE favorite = 0")
        self._db.commit()
        return ids

    def _add(
        self,
        kind: str,
        text: str,
        image_path: str | None,
        width: int,
        height: int,
        digest: str,
    ) -> Clip:
        existing = self._by_hash(digest)
        if existing is not None:
            self._bump(existing.id)
            return self.get(existing.id)  # type: ignore[return-value]
        now = time.time()
        cursor = self._db.execute(
            """
            INSERT INTO clips (kind, text, image_path, width, height, hash, created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            """,
            (kind, text, image_path, width, height, digest, now),
        )
        self._db.commit()
        self._trim()
        return self.get(int(cursor.lastrowid))  # type: ignore[return-value]

    def _bump(self, clip_id: int) -> None:
        self._db.execute(
            "UPDATE clips SET created_at = ? WHERE id = ?",
            (time.time(), clip_id),
        )
        self._db.commit()

    def _by_hash(self, digest: str) -> Clip | None:
        row = self._db.execute("SELECT * FROM clips WHERE hash = ?", (digest,)).fetchone()
        return None if row is None else self._row(row)

    def _trim(self) -> None:
        rows = self._db.execute(
            """
            SELECT id, image_path FROM clips
            WHERE favorite = 0
            ORDER BY created_at DESC
            LIMIT -1 OFFSET ?
            """,
            (HISTORY_LIMIT,),
        ).fetchall()
        for row in rows:
            if row["image_path"]:
                Path(row["image_path"]).unlink(missing_ok=True)
            self._db.execute("DELETE FROM clips WHERE id = ?", (row["id"],))
        if rows:
            self._db.commit()

    @staticmethod
    def _row(row: sqlite3.Row) -> Clip:
        return Clip(
            id=int(row["id"]),
            kind=str(row["kind"]),
            text=str(row["text"]),
            image_path=None if row["image_path"] is None else str(row["image_path"]),
            width=int(row["width"]),
            height=int(row["height"]),
            created_at=float(row["created_at"]),
            favorite=bool(row["favorite"]),
        )
