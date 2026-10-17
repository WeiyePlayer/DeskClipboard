"""Desk clipboard host: record copies, draw cards, paste what the board taps."""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from clipboard_agent.catalog import card_frame, chrome_frames, sync_frames, toast_frame
from clipboard_agent.device_link import DeviceLink, candidate_ports
from clipboard_agent.extract import extract_chips
from clipboard_agent.frames import (
    CARD_DROP,
    HELLO,
    MENU,
    MENU_DELETE,
    MENU_FAVORITE,
    MENU_UNFAVORITE,
    PAGE_ACTION,
    PAGE_CLEAR,
    PAGE_PAUSE,
    PAGE_RESUME,
    REGION_BODY,
    REGION_CHIP,
    TAP,
    VERIFY_TAP,
    decode_menu,
    decode_page,
    decode_tap,
    encode_frame,
)
from clipboard_agent.pasteboard import (
    SAMPLE_TEXT,
    close_textedit_front,
    focus_textedit_document,
    paste,
    paste_content,
    read_png,
    read_text,
    restore,
    snapshot,
    textedit_front_text,
)
from clipboard_agent.pngutil import sample_png
from clipboard_agent.store import Clip, Store
from clipboard_agent.watcher import MacPasteboard, Watcher

APP_SUPPORT = Path.home() / "Library" / "Application Support" / "DeskClipboard"


def wait_for_port(explicit: str | None, timeout: float) -> str:
    if explicit:
        return explicit
    deadline = time.time() + timeout
    while True:
        found = candidate_ports()
        if found:
            return found[0]
        if time.time() >= deadline:
            raise SystemExit(
                "没有找到剪切板开发板（USB VID 303A PID 4001）。"
                "若要烧录，按住 BOOT，点一下 RESET，再松开 BOOT。"
            )
        time.sleep(0.2)


TOAST_SAFE_VERSION = 3

_RESET_REASONS = {
    1: "上电",
    3: "软件重启",
    4: "崩溃（panic）",
    5: "中断看门狗",
    6: "任务看门狗",
    7: "看门狗",
    9: "掉电",
}


def log(message: str) -> None:
    print(f"{time.strftime('%H:%M:%S')} {message}", flush=True)


def describe_hello(frame) -> str:
    payload = frame.payload
    text = f"开发板已连接，协议 {payload[0] if payload else '?'}"
    if len(payload) >= 2:
        reason = payload[1]
        text += f"，上次重启：{_RESET_REASONS.get(reason, reason)}"
    return text


class Session:
    """Keeps the board in step with the store.

    A frame the board does not acknowledge never kills the agent: the session
    marks the board as out of date and resends everything a moment later. If
    the port disappears (board reset or unplugged) it reopens it when the
    board comes back.
    """

    _RETRY_SECONDS = 2.0

    def __init__(self, store: Store, link: DeviceLink, port: str | None = None) -> None:
        self.store = store
        self.link = link
        self.port = port
        self.watcher = Watcher(store, MacPasteboard())
        self._prompted = False
        self._stale = True
        self._next_sync = 0.0
        self._synced_at = 0.0
        self.board_version: int | None = None
        self._old_firmware_logged = False

    def _note_hello(self, frame) -> str:
        if frame.payload:
            self.board_version = frame.payload[0]
        return describe_hello(frame)

    def _send(self, frame: bytes) -> bool:
        try:
            self.link.send(frame)
            return True
        except (TimeoutError, ConnectionError, OSError) as exc:
            log(f"开发板没有确认（{type(exc).__name__}：{exc}），稍后重新同步")
            self._stale = True
            # Let the board drop the half-received frame before we resend.
            self._next_sync = time.time() + self._RETRY_SECONDS
            return False

    def _send_all(self, frames) -> bool:
        for frame in frames:
            if not self._send(frame):
                return False
        return True

    def sync(self) -> bool:
        started = time.time()
        ok = self._send_all(chrome_frames()) and self._send_all(sync_frames(self.store))
        if ok:
            self._stale = False
            self._synced_at = time.time()
            log(f"已同步 {len(self.store.history())} 条，用时 {time.time() - started:.1f} 秒")
        return ok

    def publish(self, clip: Clip) -> None:
        if self._stale:
            return  # the pending full sync will carry it
        if self._send(card_frame(self.store, clip)) and self._send_all(sync_frames(self.store)[-2:]):
            kind = "图片" if clip.kind == "image" else "文字"
            log(f"新卡片已送到开发板：{kind} #{clip.id}")

    def _resync(self) -> None:
        for frame in sync_frames(self.store):
            if not self._send(frame):
                return

    def _reconnect(self) -> bool:
        if self.port is None:
            return False
        self.link.close()
        found = candidate_ports()
        if not found:
            return False
        try:
            self.link = DeviceLink(found[0])
        except (OSError, ValueError) as exc:
            log(f"打不开串口：{exc}")
            return False
        self.port = found[0]
        hello = self.link.wait_hello(1.5)
        log("重新连上开发板" if hello is None else "重新连上，" + self._note_hello(hello))
        self._stale = True
        self._next_sync = 0.0
        return True

    def run(self) -> int:
        hello = self.link.wait_hello(1.5)
        if hello is None:
            log("没有等到 HELLO，直接同步")
        else:
            log(self._note_hello(hello))
        self.sync()
        lost_logged = False
        while True:
            try:
                self._drain_pasteboard()
                if not self.link.alive:
                    if not lost_logged:
                        log("开发板串口断开，等待重新连接")
                        lost_logged = True
                    if not self._reconnect():
                        time.sleep(0.5)
                        continue
                    lost_logged = False
                if self._stale and time.time() >= self._next_sync:
                    self.sync()
                event = self.link.next_event(0.3)
                if event is None:
                    continue
                if event.type == HELLO:
                    # The board repeats HELLO every 0.5 s until our first frame
                    # lands; one that crossed a sync we just finished is stale.
                    if time.time() - self._synced_at > 2.0:
                        log(self._note_hello(event))
                        self.sync()
                elif event.type == TAP:
                    self._tap(*decode_tap(event.payload))
                elif event.type == MENU:
                    self._menu(*decode_menu(event.payload))
                elif event.type == PAGE_ACTION:
                    self._page(decode_page(event.payload))
                elif event.type == CARD_DROP:
                    pass
            except KeyboardInterrupt:
                print("\n已停止")
                return 0
            except Exception as exc:  # keep the agent alive; resync next round
                log(f"出错：{type(exc).__name__}：{exc}")
                self._stale = True
                self._next_sync = time.time() + self._RETRY_SECONDS
                time.sleep(0.5)

    def _drain_pasteboard(self) -> None:
        clip = self.watcher.poll()
        if clip is not None:
            self.publish(clip)

    def _tap(self, clip_id: int, kind: int, chip_index: int) -> None:
        clip = self.store.get(clip_id)
        if clip is None:
            return
        text: str | None = None
        png: bytes | None = None
        if clip.kind == "image" and kind == REGION_BODY:
            png = self.store.image_bytes(clip)
        elif kind == REGION_CHIP:
            chips = extract_chips(clip.text)
            if chip_index >= len(chips):
                return
            text = chips[chip_index]
        else:
            text = clip.text
        if text is not None:
            self.watcher.suppress = __import__("hashlib").sha256(text.encode()).hexdigest()
        elif png is not None:
            self.watcher.suppress = __import__("hashlib").sha256(png).hexdigest()
        result = paste_content(
            text=text,
            png=png,
            keystroke=True,
            prompt=not self._prompted,
            restore_after=0.5,
            own_write=self.watcher.own_write,
        )
        self._prompted = True
        log(result.message)
        toast = "已粘贴" if result.code == "ok" else _toast_for(result.code)
        # Firmware before protocol 3 double-frees its toast timer on the second
        # toast and panics, so only send hints to boards that are fixed.
        if (self.board_version or 0) >= TOAST_SAFE_VERSION:
            self._send(toast_frame(toast))
        elif not self._old_firmware_logged:
            log("开发板固件较旧（协议 < 3），不在屏上显示粘贴提示")
            self._old_firmware_logged = True

    def _menu(self, clip_id: int, action: int) -> None:
        if action == MENU_FAVORITE:
            self.store.set_favorite(clip_id, True)
        elif action == MENU_UNFAVORITE:
            self.store.set_favorite(clip_id, False)
        elif action == MENU_DELETE:
            self.store.delete(clip_id)
        else:
            return
        self._resync()

    def _page(self, action: int) -> None:
        if action == PAGE_CLEAR:
            self.store.clear_history()
            self._resync()
        elif action == PAGE_PAUSE:
            self.watcher.paused = True
        elif action == PAGE_RESUME:
            self.watcher.paused = False


def _toast_for(code: str) -> str:
    if code == "accessibility":
        return "辅助功能未授权"
    if code == "secure-input":
        return "请手动 Command-V"
    return "粘贴失败"


def verify(port: str) -> int:
    store = Store(APP_SUPPORT)
    link = DeviceLink(port)
    try:
        if link.wait_hello(1.5) is None:
            print("没有等到 HELLO，直接同步", flush=True)
        session = Session(store, link)
        address = "四川省成都市郫都区欣茂西郡兰庭 张世伦 13558801181"
        clip = store.add_text(address)
        session.sync()
        link.send(encode_frame(VERIFY_TAP, decode_ready(clip.id, REGION_BODY, 0)))
        echoed = _expect(link, TAP)
        if echoed is None or decode_tap(echoed.payload)[0] != clip.id:
            print(f"开发板没有回传点击：{echoed}", file=sys.stderr)
            return 1
        chips = extract_chips(address)
        link.send(encode_frame(VERIFY_TAP, decode_ready(clip.id, REGION_CHIP, 0)))
        chip_echo = _expect(link, TAP)
        if chip_echo is None or decode_tap(chip_echo.payload) != (clip.id, REGION_CHIP, 0):
            print("开发板没有回传手机号胶囊", file=sys.stderr)
            return 1
        if not chips or chips[0] != "13558801181":
            print(f"胶囊拆分不对：{chips}", file=sys.stderr)
            return 1
        original = snapshot()
        try:
            focus_textedit_document()
            result = paste_content(text=address, keystroke=True, prompt=True, restore_after=None)
            time.sleep(0.3)
            pasted = textedit_front_text()
            close_textedit_front()
            if result.code != "ok" or address not in pasted:
                print(result.message, file=sys.stderr)
                print(f"TextEdit 读到：{pasted!r}", file=sys.stderr)
                return 1
            print(f"文字已贴进 TextEdit：{address}")
            focus_textedit_document()
            png = sample_png()
            image = paste_content(png=png, keystroke=True, prompt=False, restore_after=None)
            time.sleep(0.4)
            image_text = textedit_front_text()
            close_textedit_front()
            if image.code != "ok" or "\ufffc" not in image_text:
                print(image.message, file=sys.stderr)
                print(f"TextEdit 读到：{image_text!r}", file=sys.stderr)
                return 1
            if read_png() != png:
                print("剪切板上的 PNG 与示例图不一致", file=sys.stderr)
                return 1
            print("图片已贴进 TextEdit")
            phone = paste_content(text=chips[0], keystroke=False, prompt=False)
            if phone.code != "held" or read_text() != chips[0]:
                print("手机号没有单独写入剪切板", file=sys.stderr)
                return 1
            print("手机号胶囊内容已单独写入剪切板")
        finally:
            restore(original)
        return 0
    finally:
        link.close()


def decode_ready(clip_id: int, kind: int, chip_index: int) -> bytes:
    import struct

    return struct.pack("<IBB", clip_id, kind, chip_index)


def _expect(link: DeviceLink, frame_type: int, timeout: float = 3):
    deadline = time.time() + timeout
    while time.time() < deadline:
        event = link.next_event(0.2)
        if event is not None and event.type == frame_type:
            return event
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="桌面剪切板：把开发板上的卡片贴进 Mac")
    parser.add_argument("--port")
    parser.add_argument("--once", choices=["text", "image"])
    parser.add_argument("--set-only", action="store_true")
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--enter-boot", action="store_true")
    parser.add_argument("--wait", type=float, default=20)
    args = parser.parse_args(argv)

    if args.once:
        result = paste(args.once, keystroke=not args.set_only, prompt=True)
        print(result.message)
        return 0 if result.code in ("ok", "held") else 1

    port = wait_for_port(args.port, args.wait)
    if args.verify:
        return verify(port)
    link = DeviceLink(port)
    try:
        if args.enter_boot:
            link.enter_boot()
            print("开发板正在进入下载模式")
            return 0
        store = Store(APP_SUPPORT)
        return Session(store, link, port).run()
    finally:
        link.close()


if __name__ == "__main__":
    raise SystemExit(main())
