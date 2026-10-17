"""Watch the system pasteboard without recording our own writes."""

from __future__ import annotations

import hashlib
import threading
from contextlib import contextmanager
from dataclasses import dataclass

from clipboard_agent.store import Clip, Store

CONCEALED = "org.nspasteboard.ConcealedType"
TRANSIENT = "org.nspasteboard.TransientType"


@dataclass(frozen=True)
class Capture:
    kind: str
    text: str
    png: bytes
    width: int
    height: int

    @property
    def digest(self) -> str:
        if self.kind == "image":
            return hashlib.sha256(self.png).hexdigest()
        return hashlib.sha256(self.text.encode()).hexdigest()


class Watcher:
    def __init__(self, store: Store, source) -> None:
        self.store = store
        self.source = source
        self.paused = False
        self._last = source.change_count()
        self.suppress: str | None = None
        self._own_counts: set[int] = set()
        self._writing = 0
        self._guard = threading.Lock()

    @contextmanager
    def own_write(self):
        """Wrap every pasteboard write the agent itself makes.

        A paste from the board writes the chosen clip and, 0.5 s later on a
        timer thread, writes the user's previous clipboard back. Both bump
        changeCount; without this the watcher recorded them as new copies and
        moved items to the top, so the list reordered after every paste.
        """
        with self._guard:
            self._writing += 1
        try:
            yield
        finally:
            with self._guard:
                self._own_counts.add(self.source.change_count())
                self._writing -= 1

    def poll(self) -> Clip | None:
        with self._guard:
            if self._writing:
                return None  # look again once our own write has finished
            count = self.source.change_count()
            if count == self._last:
                return None
            self._last = count
            if count in self._own_counts:
                self._own_counts.discard(count)
                return None
        if self.paused:
            return None
        capture = self.source.read()
        if capture is None:
            return None
        if capture.digest == self.suppress:
            self.suppress = None
            return None
        if capture.kind == "image":
            return self.store.add_image(capture.png, capture.width, capture.height)
        return self.store.add_text(capture.text)


class MacPasteboard:
    def change_count(self) -> int:
        from AppKit import NSPasteboard

        return int(NSPasteboard.generalPasteboard().changeCount())

    def read(self) -> Capture | None:
        from AppKit import (
            NSBitmapImageRep,
            NSData,
            NSPNGFileType,
            NSPasteboardTypePNG,
            NSPasteboardTypeString,
            NSPasteboardTypeTIFF,
            NSPasteboard,
        )

        board = NSPasteboard.generalPasteboard()
        for item in board.pasteboardItems() or []:
            kinds = {str(kind) for kind in item.types()}
            if CONCEALED in kinds or TRANSIENT in kinds:
                return None
        png_data = board.dataForType_(NSPasteboardTypePNG)
        if png_data is None:
            tiff = board.dataForType_(NSPasteboardTypeTIFF)
            if tiff is not None:
                rep = NSBitmapImageRep.imageRepWithData_(tiff)
                if rep is not None:
                    png_data = rep.representationUsingType_properties_(NSPNGFileType, None)
        if png_data is not None:
            raw = bytes(png_data)
            rep = NSBitmapImageRep.imageRepWithData_(NSData.dataWithBytes_length_(raw, len(raw)))
            if rep is None:
                return None
            return Capture("image", "", raw, int(rep.pixelsWide()), int(rep.pixelsHigh()))
        text = board.stringForType_(NSPasteboardTypeString)
        if text:
            return Capture("text", str(text), b"", 0, 0)
        return None
