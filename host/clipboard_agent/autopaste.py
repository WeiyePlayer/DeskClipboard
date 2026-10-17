"""Paste a received item into the focused app right after it lands on the clipboard.

The decision logic is platform-free and unit-tested with fake backends; only
MacKeys / WindowsKeys touch the OS. Outcomes go back to the board in the ACK
("paste" field) so it can show the matching toast.
"""
from __future__ import annotations

import os
import sys
import time

PASTED = "pasted"              # keystroke sent
OFF = "off"                    # auto paste disabled: clipboard only
ACCESSIBILITY = "accessibility"  # macOS Accessibility not granted
SECURE_INPUT = "secure_input"  # macOS secure input / Windows secure desktop
BLOCKED = "blocked"            # the OS refused the synthetic keystroke
LATE = "late"                  # the send deadline passed before the keystroke

MESSAGES = {
    "darwin": {
        PASTED: "已在当前窗口粘贴（Command-V）",
        ACCESSIBILITY: ("已写入剪贴板，但未自动粘贴：辅助功能未授权。请到「系统设置 → 隐私与安全性 → 辅助功能」"
                        "打开「{app}」，然后重启客户端。现在可手动按 Command-V。"),
        SECURE_INPUT: "已写入剪贴板，但当前窗口处于安全输入（如密码框），系统拦截了模拟按键。请手动按 Command-V。",
        BLOCKED: "已写入剪贴板，但模拟 Command-V 失败。请手动按 Command-V。",
    },
    "win32": {
        PASTED: "已在当前窗口粘贴（Ctrl-V）",
        ACCESSIBILITY: "已写入剪贴板，但未自动粘贴。请手动按 Ctrl-V。",
        SECURE_INPUT: "已写入剪贴板，但当前处于锁屏或 UAC 安全桌面，无法模拟按键。请手动按 Ctrl-V。",
        BLOCKED: ("已写入剪贴板，但系统拦截了模拟 Ctrl-V（目标窗口可能以管理员身份运行）。"
                  "请手动按 Ctrl-V，或以管理员身份运行客户端。"),
    },
}


def background_launch() -> bool:
    """True under the LaunchAgent / logon task (set by the installers)."""
    return os.environ.get("DESKCLIP_BACKGROUND") == "1"


def mac_python_app() -> str:
    """The bundle macOS charges Accessibility to when launchd starts the client.

    Framework Pythons (Homebrew, python.org) re-exec into Resources/Python.app,
    so that app, not .venv/bin/python, is what has to be allowed.
    """
    app = os.path.join(os.path.realpath(sys.base_prefix), "Resources", "Python.app")
    return app if os.path.isdir(app) else os.path.realpath(sys.executable)


def responsible_app() -> str:
    if background_launch():
        return "Python（" + mac_python_app() + "）"
    return "终端（Terminal，或你运行客户端所用的应用）"


def message(outcome: str, platform: str | None = None) -> str:
    table = MESSAGES.get(platform or sys.platform, MESSAGES["darwin"])
    text = table.get(outcome, "已更新本机剪贴板")
    return text.replace("{app}", responsible_app()) if "{app}" in text else text


class AutoPaster:
    """``keys`` must provide prepare(), blocker() -> outcome|None, send() -> bool."""

    def __init__(self, keys, enabled: bool = True, settle: float = 0.05, clock=time.monotonic, sleep=time.sleep):
        self.keys, self.enabled, self.settle = keys, enabled, settle
        self.clock, self.sleep = clock, sleep

    def prepare(self) -> str | None:
        """Run once at start-up. On macOS this raises the Accessibility prompt."""
        if not self.enabled:
            return None
        return self.keys.prepare()

    def paste(self, deadline: float) -> str:
        """Call right after the received text was written to the clipboard."""
        if not self.enabled:
            return OFF
        blocker = self.keys.blocker()
        if blocker:
            return blocker
        self.sleep(self.settle)  # let the pasteboard write settle before the app reads it
        if self.clock() >= deadline:
            return LATE
        return PASTED if self.keys.send() else BLOCKED


class MacKeys:
    def prepare(self):
        from clipboard_agent.pasteboard import accessibility_trusted
        return None if accessibility_trusted(prompt=True) else ACCESSIBILITY

    def blocker(self):
        from clipboard_agent.pasteboard import accessibility_trusted, secure_input_enabled
        if not accessibility_trusted(prompt=False):
            return ACCESSIBILITY
        if secure_input_enabled():
            return SECURE_INPUT
        return None

    def send(self):
        from clipboard_agent.pasteboard import send_command_v
        send_command_v()
        return True


class WindowsKeys:
    def __init__(self):
        import ctypes
        from ctypes import wintypes as w
        ulong_ptr = ctypes.c_size_t

        class KEYBDINPUT(ctypes.Structure):
            _fields_ = [("wVk", w.WORD), ("wScan", w.WORD), ("dwFlags", w.DWORD),
                        ("time", w.DWORD), ("dwExtraInfo", ulong_ptr)]

        class MOUSEINPUT(ctypes.Structure):  # largest member: fixes sizeof(INPUT)
            _fields_ = [("dx", w.LONG), ("dy", w.LONG), ("mouseData", w.DWORD), ("dwFlags", w.DWORD),
                        ("time", w.DWORD), ("dwExtraInfo", ulong_ptr)]

        class _U(ctypes.Union):
            _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT)]

        class INPUT(ctypes.Structure):
            _fields_ = [("type", w.DWORD), ("u", _U)]

        self.ctypes, self.INPUT, self.KEYBDINPUT = ctypes, INPUT, KEYBDINPUT
        self.user = ctypes.WinDLL("user32", use_last_error=True)
        self.user.SendInput.argtypes = [w.UINT, ctypes.POINTER(INPUT), ctypes.c_int]
        self.user.SendInput.restype = w.UINT
        self.user.OpenInputDesktop.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        self.user.OpenInputDesktop.restype = w.HANDLE
        self.user.CloseDesktop.argtypes = [w.HANDLE]

    def prepare(self):
        return None

    def blocker(self):
        desktop = self.user.OpenInputDesktop(0, False, 0x0100)  # DESKTOP_SWITCHDESKTOP
        if not desktop:  # lock screen or UAC secure desktop
            return SECURE_INPUT
        self.user.CloseDesktop(desktop)
        return None

    def send(self):
        KEYUP, CTRL, V = 0x0002, 0x11, 0x56
        events = [(CTRL, 0), (V, 0), (V, KEYUP), (CTRL, KEYUP)]
        array = (self.INPUT * 4)()
        for slot, (vk, flags) in zip(array, events):
            slot.type = 1  # INPUT_KEYBOARD
            slot.u.ki = self.KEYBDINPUT(vk, 0, flags, 0, 0)
        return self.user.SendInput(4, array, self.ctypes.sizeof(self.INPUT)) == 4


def native_paster(enabled: bool = True) -> AutoPaster:
    if not enabled:
        return AutoPaster(None, enabled=False)
    if sys.platform == "darwin":
        return AutoPaster(MacKeys())
    if sys.platform == "win32":
        return AutoPaster(WindowsKeys())
    return AutoPaster(None, enabled=False)
