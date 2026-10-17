"""Write realistic host frames for the simulator (run from host/ with the venv).

build/frames.bin: chrome + initial sync. build/after.bin: toast + the resync
the host sends after the first card is favorited from its ⋮ menu.
"""
import sys, tempfile
from pathlib import Path
from clipboard_agent.catalog import chrome_frames, sync_frames, toast_frame
from clipboard_agent.pngutil import sample_png
from clipboard_agent.store import Store

out = Path(sys.argv[1])
tmp = tempfile.TemporaryDirectory(); s = Store(Path(tmp.name))
for i in range(3):
    s.add_text(f"测试 {i}")
s.add_text("https://example.com/item/clipboard")
s.add_text("需要点开始抢单，然后在这个页面上")
s.add_text("四川省成都市 示例地址 张三 13800138000")
s.add_image(sample_png(), 48, 48)
assets = Path(__file__).resolve().parents[1] / "clipboard/main/ui_assets.bin"
(out / "frames.bin").write_bytes(assets.read_bytes() + b"".join(sync_frames(s)))
s.set_favorite(s.history()[0].id, True)
(out / "after.bin").write_bytes(toast_frame("已粘贴") + b"".join(sync_frames(s)))
