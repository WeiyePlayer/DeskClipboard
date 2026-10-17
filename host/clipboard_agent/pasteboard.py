"""Write the system pasteboard and send Command-V."""

from __future__ import annotations

import ctypes
import subprocess
import time
from dataclasses import dataclass

SAMPLE_TEXT = "桌面剪切板已接通，这是一句中文。"

ACCESSIBILITY_MESSAGE = (
    "辅助功能未授权，模拟 Command-V 被系统拒绝。"
    "请到「系统设置 → 隐私与安全性 → 辅助功能」，允许当前运行本程序的应用"
    "（终端或 Cursor），然后重新执行。"
    "内容已经留在系统剪切板上，现在可以手动按 Command-V。"
)

SECURE_INPUT_MESSAGE = (
    "当前窗口处于安全输入，系统会拦住模拟按键。"
    "内容已经留在系统剪切板上，请手动按 Command-V。"
)


@dataclass(frozen=True)
class PasteResult:
    code: str
    message: str


def accessibility_trusted(prompt: bool) -> bool:
    from ApplicationServices import AXIsProcessTrustedWithOptions

    if not prompt:
        return bool(AXIsProcessTrustedWithOptions(None))
    return bool(AXIsProcessTrustedWithOptions({"AXTrustedCheckOptionPrompt": True}))


def secure_input_enabled() -> bool:
    carbon = ctypes.CDLL("/System/Library/Frameworks/Carbon.framework/Carbon")
    carbon.IsSecureEventInputEnabled.restype = ctypes.c_uint8
    return bool(carbon.IsSecureEventInputEnabled())


def _pasteboard():
    from AppKit import NSPasteboard

    return NSPasteboard.generalPasteboard()


def snapshot() -> list[list[tuple[str, bytes]]]:
    items: list[list[tuple[str, bytes]]] = []
    for item in _pasteboard().pasteboardItems() or []:
        entry: list[tuple[str, bytes]] = []
        for kind in item.types():
            data = item.dataForType_(kind)
            if data is not None:
                entry.append((str(kind), bytes(data)))
        items.append(entry)
    return items


def restore(items: list[list[tuple[str, bytes]]]) -> None:
    from AppKit import NSData, NSPasteboardItem

    board = _pasteboard()
    board.clearContents()
    objects = []
    for entry in items:
        pasted = NSPasteboardItem.alloc().init()
        for kind, raw in entry:
            pasted.setData_forType_(NSData.dataWithBytes_length_(raw, len(raw)), kind)
        objects.append(pasted)
    if objects:
        board.writeObjects_(objects)


def read_text() -> str | None:
    from AppKit import NSPasteboardTypeString

    value = _pasteboard().stringForType_(NSPasteboardTypeString)
    if value is None:
        return None
    return str(value)


def read_png() -> bytes | None:
    from AppKit import NSPasteboardTypePNG

    data = _pasteboard().dataForType_(NSPasteboardTypePNG)
    if data is None:
        return None
    return bytes(data)


def read_tiff() -> bytes | None:
    from AppKit import NSPasteboardTypeTIFF

    data = _pasteboard().dataForType_(NSPasteboardTypeTIFF)
    if data is None:
        return None
    return bytes(data)


def set_text(text: str) -> None:
    from AppKit import NSPasteboardTypeString

    board = _pasteboard()
    board.clearContents()
    if not board.setString_forType_(text, NSPasteboardTypeString):
        raise RuntimeError("写入文字剪切板失败")


def set_image_png(png: bytes) -> None:
    from AppKit import (
        NSData,
        NSImage,
        NSPasteboardTypePNG,
        NSPasteboardTypeTIFF,
    )

    png_data = NSData.dataWithBytes_length_(png, len(png))
    image = NSImage.alloc().initWithData_(png_data)
    if image is None:
        raise RuntimeError("PNG 无法解码")
    tiff = image.TIFFRepresentation()
    if tiff is None:
        raise RuntimeError("无法从 PNG 生成 TIFF")
    board = _pasteboard()
    board.clearContents()
    board.declareTypes_owner_([NSPasteboardTypePNG, NSPasteboardTypeTIFF], None)
    if not board.setData_forType_(png_data, NSPasteboardTypePNG):
        raise RuntimeError("写入 PNG 剪切板失败")
    if not board.setData_forType_(tiff, NSPasteboardTypeTIFF):
        raise RuntimeError("写入 TIFF 剪切板失败")


def send_command_v() -> None:
    from Quartz import (
        CGEventCreateKeyboardEvent,
        CGEventPost,
        CGEventSetFlags,
        kCGEventFlagMaskCommand,
        kCGHIDEventTap,
    )

    key_v = 9
    down = CGEventCreateKeyboardEvent(None, key_v, True)
    up = CGEventCreateKeyboardEvent(None, key_v, False)
    CGEventSetFlags(down, kCGEventFlagMaskCommand)
    CGEventSetFlags(up, kCGEventFlagMaskCommand)
    CGEventPost(kCGHIDEventTap, down)
    time.sleep(0.02)
    CGEventPost(kCGHIDEventTap, up)


def paste_content(
    *,
    text: str | None = None,
    png: bytes | None = None,
    keystroke: bool,
    prompt: bool,
    restore_after: float | None = None,
    own_write=None,
) -> PasteResult:
    """``own_write`` is a context-manager factory (Watcher.own_write) that
    marks the writes below as the agent's own so they are not recorded."""
    import contextlib

    guard = own_write or contextlib.nullcontext
    previous = snapshot() if restore_after is not None else None
    if png is not None:
        with guard():
            set_image_png(png)
        message = "已粘贴图片"
    elif text is not None:
        with guard():
            set_text(text)
        message = "已粘贴"
    else:
        return PasteResult("unknown-paste", "没有可粘贴的内容")
    if not keystroke:
        return PasteResult("held", "内容已放入系统剪切板，未发送 Command-V")
    if not accessibility_trusted(prompt):
        return PasteResult("accessibility", ACCESSIBILITY_MESSAGE)
    if secure_input_enabled():
        return PasteResult("secure-input", SECURE_INPUT_MESSAGE)
    time.sleep(0.05)
    send_command_v()
    if previous is not None and restore_after is not None:
        import threading

        def _restore() -> None:
            with guard():
                restore(previous)

        threading.Timer(restore_after, _restore).start()
    return PasteResult("ok", message)


def paste(kind: str, *, keystroke: bool, prompt: bool) -> PasteResult:
    if kind == "text":
        set_text(SAMPLE_TEXT)
    elif kind == "image":
        from clipboard_agent.pngutil import sample_png

        set_image_png(sample_png())
    else:
        return PasteResult("unknown-paste", "未知的粘贴类型")

    if not keystroke:
        return PasteResult("held", "内容已放入系统剪切板，未发送 Command-V")

    if not accessibility_trusted(prompt):
        return PasteResult("accessibility", ACCESSIBILITY_MESSAGE)
    if secure_input_enabled():
        return PasteResult("secure-input", SECURE_INPUT_MESSAGE)
    time.sleep(0.05)
    send_command_v()
    if kind == "text":
        return PasteResult("ok", "已把示例中文贴进当前输入框")
    return PasteResult("ok", "已把示例图片贴进当前输入框")


def frontmost_application() -> str:
    output = subprocess.check_output(
        [
            "osascript",
            "-e",
            'tell application "System Events" to get name of first application process whose frontmost is true',
        ],
        text=True,
    )
    return output.strip()


def focus_textedit_document() -> None:
    script = """
    tell application "TextEdit"
        activate
        make new document
    end tell
    """
    subprocess.run(["osascript", "-e", script], check=True)
    deadline = time.time() + 3
    while time.time() < deadline:
        if frontmost_application() == "TextEdit":
            return
        time.sleep(0.1)
    raise RuntimeError(f"TextEdit 没有成为前台应用，当前是 {frontmost_application()}")


def textedit_front_text() -> str:
    output = subprocess.check_output(
        ["osascript", "-e", 'tell application "TextEdit" to text of front document'],
        text=True,
    )
    return output


def close_textedit_front() -> None:
    subprocess.run(
        [
            "osascript",
            "-e",
            'tell application "TextEdit" to close front document saving no',
        ],
        check=False,
    )
