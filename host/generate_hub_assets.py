"""Build offline UI labels. Only asset regeneration needs a Mac/font."""
from pathlib import Path
from clipboard_agent.catalog import chrome_frames
from clipboard_agent.frames import encode_asset
from clipboard_agent.render import render_label

labels = {
    14: "选择接收电脑", 15: "设备连接状态", 16: "存储用量", 17: "已配对设备",
    18: "配对管理", 19: "设备连接状态", 20: "存储用量", 21: "已写入对方剪贴板",
    22: "发送失败，请重试", 23: "正在发送，请稍候", 24: "复制内容后显示在这里",
    25: "请先通过 USB 配置", 26: "已暂停记录", 27: "空间已满，请清理记录",
    28: "操作失败，请重试", 29: "请升级电脑端程序", 30: "已开放 USB 配对",
    31: "已写入剪贴板，请开启辅助功能", 32: "已写入剪贴板，请手动粘贴", 33: "已在对方电脑粘贴",
    34: "息屏时间：30秒", 35: "息屏时间：1分钟", 36: "息屏时间：2分钟", 37: "息屏时间：5分钟", 38: "息屏时间：从不",
    39: "手机配对码（2 分钟内有效）", 40: "手机已配对", 41: "配对码错误",
}
frames = chrome_frames()
for key, value in labels.items():
    toast = 21 <= key <= 23 or 26 <= key <= 33 or key in (40, 41)
    raster = render_label(value, (32, 32, 32) if toast else (255, 255, 255),
                          foreground=(255, 255, 255) if toast else (20, 20, 20))
    frames.append(encode_asset(key, raster.width, raster.height, raster.rgb565))
path = Path(__file__).resolve().parents[1] / "firmware/clipboard/main/ui_assets.bin"
path.write_bytes(b"".join(frames))
print(f"{path}: {path.stat().st_size} bytes")
