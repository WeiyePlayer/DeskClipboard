"""Native Mac/Windows clipboard adapters. Receiving never simulates a key."""
from __future__ import annotations

import ctypes
import sys
import time
from contextlib import contextmanager

from clipboard_agent.watcher import Capture, MacPasteboard

OWN_TYPE = "com.deskclipboard.received-v4"


class MacClipboard(MacPasteboard):
    def read(self):
        from AppKit import NSPasteboard, NSPasteboardTypeString
        from clipboard_agent.watcher import CONCEALED, TRANSIENT
        board = NSPasteboard.generalPasteboard()
        kinds = {str(t) for t in board.types() or []}
        for item in board.pasteboardItems() or []:
            kinds.update(str(t) for t in item.types())
        if {OWN_TYPE, "public.file-url", CONCEALED, TRANSIENT} & kinds:
            return None
        value = board.stringForType_(NSPasteboardTypeString)
        return Capture("text", str(value), b"", 0, 0) if value else None

    def write(self, kind: str, content: bytes, job_id: str):
        if kind != "text":
            raise ValueError("仅支持文字剪贴板")
        from AppKit import NSPasteboard
        from clipboard_agent.pasteboard import set_text
        set_text(content.decode("utf-8"))
        board = NSPasteboard.generalPasteboard()
        board.addTypes_owner_([OWN_TYPE], None)
        if not board.setString_forType_(job_id, OWN_TYPE):
            raise RuntimeError("无法标记接收的剪贴板内容")


class WindowsClipboard:
    def __init__(self):
        from ctypes import wintypes as w
        self.user = ctypes.WinDLL("user32", use_last_error=True)
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.user.RegisterClipboardFormatW.argtypes = [w.LPCWSTR]
        self.user.RegisterClipboardFormatW.restype = w.UINT
        self.user.OpenClipboard.argtypes = [w.HWND]
        self.user.OpenClipboard.restype = w.BOOL
        self.user.CloseClipboard.restype = w.BOOL
        self.user.EmptyClipboard.restype = w.BOOL
        self.user.IsClipboardFormatAvailable.argtypes = [w.UINT]
        self.user.IsClipboardFormatAvailable.restype = w.BOOL
        self.user.GetClipboardData.argtypes = [w.UINT]
        self.user.GetClipboardData.restype = w.HANDLE
        self.user.SetClipboardData.argtypes = [w.UINT, w.HANDLE]
        self.user.SetClipboardData.restype = w.HANDLE
        self.user.GetClipboardSequenceNumber.restype = w.DWORD
        self.kernel.GetModuleHandleW.argtypes = [w.LPCWSTR]
        self.kernel.GetModuleHandleW.restype = w.HMODULE
        self.user.CreateWindowExW.argtypes = [w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD,
                                             ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                             w.HWND, w.HMENU, w.HINSTANCE, ctypes.c_void_p]
        self.user.CreateWindowExW.restype = w.HWND
        self.hwnd = self.user.CreateWindowExW(0, "STATIC", "DeskClipboard", 0, 0, 0, 0, 0,
                                             None, None, self.kernel.GetModuleHandleW(None), None)
        if not self.hwnd:
            raise OSError("无法创建系统剪贴板窗口")
        self.kernel.GlobalAlloc.argtypes = [w.UINT, ctypes.c_size_t]
        self.kernel.GlobalAlloc.restype = w.HGLOBAL
        self.kernel.GlobalLock.argtypes = [w.HGLOBAL]
        self.kernel.GlobalLock.restype = ctypes.c_void_p
        self.kernel.GlobalUnlock.argtypes = [w.HGLOBAL]
        self.kernel.GlobalSize.argtypes = [w.HGLOBAL]
        self.kernel.GlobalSize.restype = ctypes.c_size_t
        self.kernel.GlobalFree.argtypes = [w.HGLOBAL]
        self.kernel.GlobalFree.restype = w.HGLOBAL
        self.own = self.user.RegisterClipboardFormatW(OWN_TYPE)
        self.concealed = [self.user.RegisterClipboardFormatW(x) for x in (
            "ExcludeClipboardContentFromMonitorProcessing", "org.nspasteboard.ConcealedType",
            "org.nspasteboard.TransientType")]
        self.history = self.user.RegisterClipboardFormatW("CanIncludeInClipboardHistory")
        self.cloud = self.user.RegisterClipboardFormatW("CanUploadToCloudClipboard")

    def change_count(self):
        return int(self.user.GetClipboardSequenceNumber())

    @contextmanager
    def opened(self):
        for _ in range(20):
            if self.user.OpenClipboard(self.hwnd):
                break
            time.sleep(0.01)
        else:
            raise OSError("系统剪贴板正被其他程序占用")
        try:
            yield
        finally:
            self.user.CloseClipboard()

    def _read(self, fmt: int) -> bytes | None:
        if not self.user.IsClipboardFormatAvailable(fmt):
            return None
        handle = self.user.GetClipboardData(fmt)
        if not handle:
            return None
        length = self.kernel.GlobalSize(handle)
        # Avoid unbounded reads of a hostile clipboard block.
        if length > 256 * 1024 * 1024:
            return None
        ptr = self.kernel.GlobalLock(handle)
        if not ptr:
            return None
        try:
            return ctypes.string_at(ptr, length)
        finally:
            self.kernel.GlobalUnlock(handle)

    def read(self):
        with self.opened():
            if self.user.IsClipboardFormatAvailable(15):  # CF_HDROP: files are outside v1.
                return None
            if any(self.user.IsClipboardFormatAvailable(x) for x in [self.own] + self.concealed):
                return None
            for marker in (self.history, self.cloud):
                flag = self._read(marker)
                if flag is not None and len(flag) >= 4 and int.from_bytes(flag[:4], "little") == 0:
                    return None
            text = self._read(13)
        if text:
            value = text[:len(text) // 2 * 2].decode("utf-16-le").split("\0", 1)[0]
            if value:
                return Capture("text", value, b"", 0, 0)
        return None

    def _set(self, fmt: int, data: bytes):
        handle = self.kernel.GlobalAlloc(0x0002, len(data))
        if not handle:
            raise MemoryError("无法分配剪贴板内存")
        transferred = False
        try:
            ptr = self.kernel.GlobalLock(handle)
            if not ptr:
                raise OSError("无法锁定剪贴板内存")
            try:
                ctypes.memmove(ptr, data, len(data))
            finally:
                self.kernel.GlobalUnlock(handle)
            if not self.user.SetClipboardData(fmt, handle):
                raise OSError("写入系统剪贴板失败")
            transferred = True
        finally:
            if not transferred:
                self.kernel.GlobalFree(handle)

    def write(self, kind: str, content: bytes, job_id: str):
        if kind != "text":
            raise ValueError("仅支持文字剪贴板")
        text = content.decode("utf-8").encode("utf-16-le") + b"\0\0"
        with self.opened():
            if not self.user.EmptyClipboard():
                raise OSError("无法打开剪贴板进行写入")
            self._set(self.own, job_id.encode() + b"\0")
            self._set(13, text)


def native_clipboard():
    if sys.platform == "darwin":
        return MacClipboard()
    if sys.platform == "win32":
        return WindowsClipboard()
    raise RuntimeError("首版仅支持 macOS 和 Windows")
