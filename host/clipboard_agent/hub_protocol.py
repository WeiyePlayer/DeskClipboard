"""V4 content contract. Network and USB share metadata and error codes."""
from __future__ import annotations

import hashlib
import struct
import uuid
from dataclasses import dataclass

from clipboard_agent.extract import extract_chips
from clipboard_agent.frames import encode_region
from clipboard_agent.render import render_text_card

VERSION = 4
MAX_TEXT = 64 * 1024
CHUNK = 16 * 1024


class HubError(Exception):
    def __init__(self, code: str) -> None:
        self.code = code
        super().__init__(code)


def error_message(code: str) -> str:
    return {
        "text_too_large_64KiB": "文字超过 64KiB，未记录",
        "invalid_kind": "开发板仅接受文字内容",
        "storage_full": "开发板空间或条数已满，收藏无法自动淘汰，请清理记录",
        "paused": "开发板已暂停记录",
        "busy": "开发板正在处理另一条上传，稍后重试",
        "pairing_closed": "请先在开发板点「⋯ → 配对管理」，两分钟内通过 USB 配对",
        "peer_limit": "已达到四台配对电脑上限，请先在开发板移除不用的电脑",
        "device_authentication_failed": "蓝牙设备身份验证失败，请检查 USB 配对；已停止连接",
        "unauthorized": "电脑凭据已失效，请重新通过 USB 配对",
        "not_paired_run_setup_or_pair": "尚未配对，请连接开发板 USB 后运行 --pair",
        "firmware_upgrade_required": "电脑端与固件协议不一致，请一并升级",
        "bluetooth_firmware_required": "开发板尚未运行纯蓝牙固件",
        "expired_delivery": "发送任务已过期，没有写入剪贴板",
        "already_evicted": "该条上传已被容量策略淘汰，本次重试不会重复新增",
    }.get(code, code)


@dataclass(frozen=True)
class Upload:
    metadata: dict
    content: bytes
    card: bytes


def make_upload(device_id: str, name: str, *, text: str,
                event_id: str | None = None, favorite: bool = False) -> Upload:
    uuid.UUID(device_id)
    content, kind = text.encode("utf-8"), "text"
    if len(content) > MAX_TEXT:
        raise HubError("text_too_large_64KiB")
    if not content:
        raise HubError("empty_content")
    chips = [c for c in extract_chips(text) if len(c.encode("utf-8")) <= 2048]
    raster = render_text_card(text, chips)
    card = struct.pack("<IBHHB", 0, 0, raster.width, raster.height, len(raster.regions))
    card += b"".join(encode_region(r) for r in raster.regions) + raster.rgb565
    metadata = {
        "event_id": event_id or str(uuid.uuid4()), "source_device": device_id,
        "source_name": name[:16], "kind": kind, "bytes": len(content),
        "sha256": hashlib.sha256(content).hexdigest(), "card_bytes": len(card),
        "card_sha256": hashlib.sha256(card).hexdigest(), "width": 0,
        "height": 0, "chips": chips, "favorite": bool(favorite),
    }
    return Upload(metadata, content, card)
