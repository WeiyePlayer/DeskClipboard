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
s.add_text("地址 四川省成都市 张三 13558801181")
s.add_image(sample_png(), 48, 48)
for i in range(6):
    s.add_text(f"测试 {i}")
(out / "frames.bin").write_bytes(b"".join(chrome_frames() + sync_frames(s)))
s.set_favorite(s.history()[0].id, True)
(out / "after.bin").write_bytes(toast_frame("已粘贴") + b"".join(sync_frames(s)))
