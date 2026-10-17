"""Two-thread companion: native clipboard stays on the main thread."""
from __future__ import annotations

import argparse
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import queue
import signal
import sqlite3
import sys
import threading
import time
import uuid
from dataclasses import dataclass, field

from clipboard_agent.hub_protocol import CHUNK, HubError, make_upload, error_message
from clipboard_agent.hub_transport import upload
from clipboard_agent.ble_transport import BLETransport
from clipboard_agent.platform_clipboard import native_clipboard
from clipboard_agent import autopaste
from clipboard_agent.instance_lock import AlreadyRunning, InstanceLock


def support_dir() -> Path:
    if sys.platform == "win32":
        return Path(os.environ.get("LOCALAPPDATA", Path.home())) / "DeskClipboard"
    return Path.home() / "Library/Application Support/DeskClipboard"


def log_path() -> Path:
    """Where the autostart installers send the background client's log."""
    if sys.platform == "win32":
        return support_dir() / "client.log"
    return Path.home() / "Library/Logs/DeskClipboard/client.log"


def load_config(path: Path) -> dict:
    if not path.exists():
        raise HubError("not_paired_run_setup_or_pair")
    data = json.loads(path.read_text())
    for field in ("board_id", "device_id", "token"):
        if not isinstance(data.get(field), str) or not data[field]:
            raise HubError("invalid_configuration")
    return data


def save_config(path: Path, data: dict):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(".tmp")
    fd = os.open(temp, os.O_CREAT | os.O_TRUNC | os.O_WRONLY, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(temp, path)
    if sys.platform != "win32":
        path.chmod(0o600)


@dataclass
class Delivery:
    job: dict
    data: bytes
    expires: float
    done: threading.Event = field(default_factory=threading.Event)
    success: bool = False
    paste: str = autopaste.OFF


class HubClient:
    def __init__(self, config: dict, clipboard=None, transport=None, paster=None):
        self.config = config
        # None = clipboard only (tests); main() passes the native paster.
        self.paster = paster if paster is not None else autopaste.AutoPaster(None, enabled=False)
        self.clipboard = clipboard if clipboard is not None else native_clipboard()
        self.transport = transport
        self.pending: queue.Queue = queue.Queue(maxsize=8)
        self.deliveries: queue.Queue[Delivery] = queue.Queue(maxsize=1)
        self.stop = threading.Event()
        self.session = str(uuid.uuid4())
        self.last_heartbeat = 0.0
        self.paused = False
        self.received: set[str] = set()
        self.received_order = deque()
        self.last_count = self.clipboard.change_count()
        self.fatal = None

    def log(self, message):
        print(time.strftime("%H:%M:%S"), message, flush=True)

    def poll_clipboard(self):
        count = self.clipboard.change_count()
        if count == self.last_count:
            return
        capture = self.clipboard.read()
        # If another app copied while data was read, sample it next round.
        if self.clipboard.change_count() != count:
            return
        self.last_count = count
        if capture is None or capture.kind != "text" or self.paused:
            return
        try:
            item = make_upload(self.config["device_id"], self.config["name"], text=capture.text)
            self.pending.put_nowait(item)
        except HubError as e:
            self.log("未记录：" + error_message(e.code))
        except queue.Full:
            self.log("待上传队列已满，本次复制未记录")

    def write_deliveries(self):
        try:
            delivery = self.deliveries.get_nowait()
        except queue.Empty:
            return
        try:
            if (delivery.job["kind"] == "text" and time.monotonic() < delivery.expires
                    and delivery.job["id"] not in self.received):
                self.clipboard.write(delivery.job["kind"], delivery.data, delivery.job["id"])
                self.received.add(delivery.job["id"])
                self.received_order.append(delivery.job["id"])
                if len(self.received_order) > 128:
                    self.received.discard(self.received_order.popleft())
                delivery.success = True
                # Paste only inside the same lease, right after our own write;
                # nothing is written or pasted once the deadline has passed.
                try:
                    delivery.paste = self.paster.paste(delivery.expires)
                except Exception as exc:
                    self.log("自动粘贴失败：" + type(exc).__name__)
                    delivery.paste = autopaste.BLOCKED
        except Exception as exc:
            self.log("写入剪贴板失败：" + type(exc).__name__)
        finally:
            delivery.done.set()

    def heartbeat(self, force=False):
        if force or time.monotonic() - self.last_heartbeat >= 2:
            response = self.transport.request({"op": "heartbeat", "session": self.session})
            self.paused = bool(response.get("paused"))
            self.last_heartbeat = time.monotonic()

    def receive(self):
        job = self.transport.request({"op": "poll"}).get("job")
        if not job:
            return
        if job["kind"] != "text" or not 0 < job["bytes"] <= 64 * 1024:
            raise HubError("invalid_delivery")
        data = bytearray()
        while len(data) < job["bytes"]:
            self.heartbeat()
            part = self.transport.download(job["id"], len(data), min(CHUNK, job["bytes"] - len(data)))
            if not part or len(data) + len(part) > job["bytes"]:
                raise HubError("incomplete_delivery")
            data.extend(part)
        if hashlib.sha256(data).hexdigest() != job["sha256"]:
            raise HubError("checksum_mismatch")
        bytes(data).decode("utf-8")
        start = time.monotonic()
        lease = self.transport.request({"op": "ready", "job_id": job["id"]})
        delivery = Delivery(job, bytes(data), start + min(lease["lease_ms"] / 1000, 2))
        self.deliveries.put(delivery, timeout=0.1)
        delivery.done.wait(max(0, delivery.expires - time.monotonic()))
        # An uncertain ACK is never retried by rewriting the clipboard.
        self.transport.request({"op": "ack", "job_id": job["id"], "success": delivery.success,
                                "paste": delivery.paste})
        if not delivery.success:
            self.log("接收超时，未写入剪贴板")
        elif delivery.paste == autopaste.OFF:
            self.log("已更新本机剪贴板")
        else:
            self.log(autopaste.message(delivery.paste))

    def progress(self):
        self.heartbeat()

    def network(self):
        active = None
        last_error = None
        while not self.stop.is_set():
            try:
                if self.transport is None:
                    self.transport = BLETransport(self.config)
                    self.heartbeat(force=True)
                    self.log("蓝牙已连接开发板，身份验证通过")
                self.heartbeat()
                self.receive()
                if active is None:
                    try:
                        active = self.pending.get_nowait()
                    except queue.Empty:
                        pass
                if active is not None:
                    record_id = upload(self.transport, active, progress=self.progress)
                    self.log(f"内容已保存到开发板：#{record_id}")
                    active = None
                last_error = None
                self.stop.wait(0.25)
            except HubError as exc:
                if exc.code != last_error:
                    self.log("设备提示：" + error_message(exc.code))
                    last_error = exc.code
                if exc.code in ("paused", "too_large", "storage_full", "already_evicted", "invalid_card", "invalid_content"):
                    active = None
                if exc.code in ("device_authentication_failed", "replayed_or_missing_record", "unauthorized", "firmware_upgrade_required"):
                    self.fatal = exc
                    self.stop.set()
                    return
                self.stop.wait(2)
            except (OSError, TimeoutError, ValueError, KeyError) as exc:
                self.log("连接中断，等待重连：" + type(exc).__name__)
                if self.transport:
                    self.transport.close()
                self.transport = None
                self.session = str(uuid.uuid4())
                self.last_heartbeat = 0
                self.stop.wait(2)

    def run(self):
        self.log("蓝牙剪贴板客户端：" + self.config["name"] + " - " + self.config["device_id"][:6])
        if self.paster.enabled:
            self.log("收到内容后自动粘贴：开启（--no-auto-paste 可关闭）")
            try:
                if self.paster.prepare():
                    self.log(autopaste.startup_hint())
                else:
                    self.log("辅助功能已授权，可以自动粘贴")
            except Exception as exc:
                self.log("无法检查辅助功能权限：" + type(exc).__name__)
        else:
            self.log("收到内容后自动粘贴：关闭，只更新剪贴板")
        worker = threading.Thread(target=self.network, daemon=True)
        worker.start()
        try:
            while not self.stop.wait(0.1):
                self.write_deliveries()
                try:
                    self.poll_clipboard()
                except Exception as exc:
                    self.log("读取剪贴板失败：" + type(exc).__name__)
        except KeyboardInterrupt:
            pass
        finally:
            self.stop.set()
            worker.join(timeout=12)
            if self.transport:
                self.transport.close()
        if self.fatal:
            raise self.fatal


def migrate(transport, config: dict, old_root: Path):
    """Read-only import, using a consistent SQLite snapshot and stable events."""
    source = old_root / "clips.db"
    if not source.exists():
        print("没有旧版数据库")
        return
    backup = old_root / "migration-v4-backup"
    backup.mkdir(exist_ok=True)
    snapshot = backup / "clips.db"
    if not snapshot.exists():
        pending_snapshot = backup / "clips.tmp.db"
        with sqlite3.connect(f"file:{source}?mode=ro", uri=True) as a:
            with sqlite3.connect(pending_snapshot) as b:
                a.backup(b)
        os.replace(pending_snapshot, snapshot)
    # Query the snapshot read-only; Store() would CREATE/commit the legacy DB.
    with sqlite3.connect(f"file:{snapshot}?mode=ro", uri=True) as db:
        db.row_factory = sqlite3.Row
        rows = db.execute("SELECT * FROM clips WHERE kind='text' ORDER BY favorite DESC, CASE WHEN favorite=1 THEN favorited_at ELSE created_at END DESC").fetchall()
    status = transport.request({"op": "status"})
    existing = transport.request({"op": "list"})["records"]
    # Select favorites and recent content before uploading, so migration never
    # evicts newer imports or pre-existing board history to make room for old data.
    room = max(0, 8 * 1024 * 1024 - status["used_bytes"] - 1536 * 1024)
    slots = 40 - len(existing)
    hashes = {(r["kind"], r["sha256"]) for r in existing}
    selected = []
    for row in rows:
        event = str(uuid.uuid5(uuid.NAMESPACE_URL, config["board_id"] + ":legacy:" + str(row["id"])))
        try:
            item = make_upload(config["device_id"], config["name"], event_id=event,
                               favorite=bool(row["favorite"]), text=row["text"])
            if item.metadata["sha256"] != row["hash"]:
                raise HubError("legacy_content_changed")
            key = (item.metadata["kind"], item.metadata["sha256"])
            needed = len(item.content) + len(item.card)
            if key not in hashes:
                if slots <= 0 or needed > room:
                    continue
                slots -= 1
                room -= needed
                hashes.add(key)
            selected.append((row, item))
        except (HubError, OSError) as e:
            print(f'条目 #{row["id"]} 保留在旧数据库：{getattr(e, "code", type(e).__name__)}')
    imported = 0
    # Commit favorites first. Within each group oldest first preserves recency.
    for favorite in (True, False):
        for row, item in reversed(selected):
            if bool(row["favorite"]) != favorite:
                continue
            try:
                upload(transport, item)
                imported += 1
            except HubError as exc:
                print(f'条目 #{row["id"]} 保留在旧数据库：{exc.code}')
    print(f"已导入 {imported} 条；原数据库和图片保持不变")


def main(argv=None):
    parser = argparse.ArgumentParser(description="DeskClipboard v4：独立跨设备剪贴板")
    parser.add_argument("--config", type=Path, default=support_dir() / "client-v4.json")
    parser.add_argument("--port")
    parser.add_argument("--name", default="Windows" if sys.platform == "win32" else "Mac")
    parser.add_argument("--setup", action="store_true", help="通过 USB 配对蓝牙客户端")
    parser.add_argument("--setup-ui", action="store_true", help="在本地浏览器完成 USB 信任配对")
    parser.add_argument("--pair", action="store_true", help="通过 USB 配对另一台电脑")
    parser.add_argument("--status", action="store_true")
    parser.add_argument("--migrate", action="store_true")
    parser.add_argument("--no-auto-paste", action="store_true",
                        help="收到内容后只更新剪贴板，不自动按 Command-V / Ctrl-V")
    parser.add_argument("--wait-for-lock", action="store_true",
                        help="已有客户端运行时等待它退出再接管（开机自启使用）")
    parser.add_argument("--log-file", type=Path, help="把日志追加写入该文件（Windows 后台运行使用）")
    args = parser.parse_args(argv)
    if args.log_file:
        args.log_file.parent.mkdir(parents=True, exist_ok=True)
        stream = open(args.log_file, "a", encoding="utf-8", buffering=1)
        sys.stdout = sys.stderr = stream
    if args.setup or args.setup_ui or args.pair:
        from clipboard_agent.hub_setup import setup
        return setup(args)
    config = load_config(args.config)
    if args.status or args.migrate:
        transport = BLETransport(config)
        try:
            if args.status:
                print(json.dumps(transport.request({"op": "status"}), ensure_ascii=False, indent=2))
            if args.migrate:
                migrate(transport, config, support_dir())
        finally:
            transport.close()
        return 0
    enabled = not args.no_auto_paste and config.get("auto_paste", True) is not False
    lock = InstanceLock(args.config.parent / "client.lock")
    try:
        lock.acquire(wait=args.wait_for_lock, on_wait=lambda pid: print(
            time.strftime("%H:%M:%S"), f"已有客户端在运行（PID {pid}），等待它退出后接管", flush=True))
    except AlreadyRunning as exc:
        print(f"已有 DeskClipboard 客户端在运行（PID {exc.pid}），本次不再启动。"
              "如已安装开机自启，它就在后台运行；日志见 " + str(log_path()), flush=True)
        return 0
    if hasattr(signal, "SIGTERM"):
        # launchctl stop / kickstart -k send SIGTERM: leave through the normal
        # shutdown path (closes BLE, logs) instead of dying silently.
        def _stop(signum, frame):
            print(time.strftime("%H:%M:%S"), "收到停止信号，客户端退出", flush=True)
            raise KeyboardInterrupt
        signal.signal(signal.SIGTERM, _stop)
    try:
        HubClient(config, paster=autopaste.native_paster(enabled)).run()
    finally:
        lock.release()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except HubError as exc:
        print("错误：" + exc.code, file=sys.stderr)
        raise SystemExit(1)
