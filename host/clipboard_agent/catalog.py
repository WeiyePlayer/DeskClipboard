"""Turn stored clips and chrome labels into device frames."""

from __future__ import annotations

from clipboard_agent.frames import (
    ASSET_CANCEL,
    ASSET_CLEAR,
    ASSET_CONFIRM,
    ASSET_CONFIRM_TITLE,
    ASSET_DELETE,
    ASSET_FAVORITE,
    ASSET_PAUSE,
    ASSET_RESUME,
    ASSET_TAB_CLIP_OFF,
    ASSET_TAB_CLIP_ON,
    ASSET_TAB_FAV_OFF,
    ASSET_TAB_FAV_ON,
    ASSET_UNFAVORITE,
    LIST_CLIPBOARD,
    LIST_FAVORITES,
    encode_asset,
    encode_card_put,
    encode_order,
    encode_toast,
)
from clipboard_agent.render import render_image_card, render_label, render_text_card
from clipboard_agent.store import Clip, Store

_GRAY = (242, 242, 247)
_WHITE = (255, 255, 255)
_DEVICE_LIMIT = 40


def chrome_frames() -> list[bytes]:
    labels = [
        (ASSET_TAB_CLIP_ON, "剪贴板", _WHITE),
        (ASSET_TAB_CLIP_OFF, "剪贴板", _GRAY),
        (ASSET_TAB_FAV_ON, "常用语", _WHITE),
        (ASSET_TAB_FAV_OFF, "常用语", _GRAY),
        (ASSET_CLEAR, "清空剪贴板历史", _WHITE),
        (ASSET_PAUSE, "暂停记录", _WHITE),
        (ASSET_RESUME, "继续记录", _WHITE),
        (ASSET_FAVORITE, "收入常用语", _WHITE),
        (ASSET_UNFAVORITE, "移出常用语", _WHITE),
        (ASSET_DELETE, "删除", _WHITE),
        (ASSET_CONFIRM_TITLE, "清空剪贴板历史？", _WHITE),
        (ASSET_CONFIRM, "清空", _WHITE),
        (ASSET_CANCEL, "取消", _WHITE),
    ]
    frames = []
    for asset_id, text, background in labels:
        raster = render_label(text, background)
        frames.append(encode_asset(asset_id, raster.width, raster.height, raster.rgb565))
    return frames


def card_frame(store: Store, clip: Clip) -> bytes:
    if clip.kind == "image":
        png = store.image_bytes(clip) or b""
        raster = render_image_card(png, clip.width, clip.height)
    else:
        raster = render_text_card(clip.text)
    regions = list(raster.regions)
    return encode_card_put(
        clip.id,
        LIST_FAVORITES if clip.favorite else LIST_CLIPBOARD,
        raster.width,
        raster.height,
        regions,
        raster.rgb565,
    )


def sync_frames(store: Store) -> list[bytes]:
    history = store.history()[:_DEVICE_LIMIT]
    favorites = store.favorites()[:_DEVICE_LIMIT]
    by_id: dict[int, Clip] = {}
    for clip in history + favorites:
        by_id[clip.id] = clip
    frames = [card_frame(store, clip) for clip in by_id.values()]
    frames.append(encode_order(LIST_CLIPBOARD, [clip.id for clip in history]))
    frames.append(encode_order(LIST_FAVORITES, [clip.id for clip in favorites]))
    return frames


def toast_frame(text: str, duration_ms: int = 1200) -> bytes:
    # Dark pill with white text. The label default is near-black text, which
    # on this background rendered the toast as a solid black block.
    raster = render_label(text, (32, 32, 32), 14, foreground=(255, 255, 255))
    return encode_toast(duration_ms, raster.width, raster.height, raster.rgb565)
