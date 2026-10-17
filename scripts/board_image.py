"""Back up / restore DeskClipboard's board state (pairing + history) with esptool.

What lives where (see firmware/clipboard/partitions.csv and main/hub.c):
  nvs      0x9000  0x6000   NVS namespace "deskclip": "peers" (paired computers'
                            ids, names and 256-bit tokens), "initialized"; plus
                            PHY calibration. This is the pairing state.
  phy_init 0xf000  0x1000   PHY init data (small; kept for completeness).
  factory  0x10000 0x400000 DeskClipboard app (optional).
  hubdata  0x410000 0xA00000 FAT + wear levelling: history index, texts, cards.
The board id (deskclip-<MAC>) and the BLE public address come from eFuse MAC,
so they survive any reflash; there is no OS-level BLE bond to restore.

Only reads and writes the listed regions; never erases the chip. Board must be
in download mode (hold BOOT, replug; 303A:1001, /dev/cu.usbmodem1101).
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
PYTHON = ROOT / ".venv/bin/python"
PT_OFFSET, PT_SIZE = 0x8000, 0xC00
BOOTLOADER = (0x0, 0x8000)
STATE = ("nvs", "phy_init", "otadata", "hubdata")


def esptool(port, *args, after="no-reset"):
    cmd = [str(PYTHON), "-m", "esptool", "--chip", "esp32s3", "--port", port, "--baud", "921600",
           "--before", "no-reset", "--after", after, *args]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode:
        sys.stderr.write(result.stdout[-2000:] + result.stderr[-2000:])
        raise SystemExit(f"esptool failed: {' '.join(args[:1])}（开发板是否处于下载模式？按住 BOOT 拔插 USB）")
    return result.stdout


def parse_table(data: bytes) -> dict:
    parts = {}
    for i in range(0, len(data), 32):
        entry = data[i:i + 32]
        if entry[:2] != b"\xaa\x50":
            break
        _, kind, subtype, offset, size, label, _ = struct.unpack("<2sBBII16sI", entry)
        parts[label.rstrip(b"\0").decode()] = {"type": kind, "subtype": subtype, "offset": offset, "size": size}
    return parts


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def board_mac(port) -> str:
    out = esptool(port, "read-mac")
    m = re.search(r"MAC:\s*([0-9a-f:]{17})", out, re.I)
    if not m:
        raise SystemExit("无法读取开发板 MAC")
    return m.group(1).lower()


def backup(args):
    out = ROOT / "backups" / ("board-" + dt.datetime.now().strftime("%Y%m%d-%H%M%S"))
    out.mkdir(parents=True, exist_ok=False)  # never reuse or overwrite an existing backup
    start = time.monotonic()
    mac = board_mac(args.port)
    table_file = out / "partition-table.bin"
    esptool(args.port, "read-flash", hex(PT_OFFSET), hex(PT_SIZE), str(table_file))
    table = parse_table(table_file.read_bytes())
    if "hubdata" not in table or "nvs" not in table:
        raise SystemExit(f"板上分区表不是 DeskClipboard 的（{sorted(table)}）；请先刷回 DeskClipboard 再备份")
    regions = [("partition-table", PT_OFFSET, PT_SIZE, table_file.name)]
    wanted = [("bootloader", *BOOTLOADER)] if args.app else []
    wanted += [(n, table[n]["offset"], table[n]["size"]) for n in STATE if n in table]
    if args.app:
        wanted += [(n, p["offset"], p["size"]) for n, p in table.items() if p["type"] == 0]
    for name, offset, size in wanted:
        t = time.monotonic()
        path = out / f"{name}.bin"
        esptool(args.port, "read-flash", hex(offset), hex(size), str(path))
        print(f"读取 {name:16s} 0x{offset:06x} +0x{size:06x}  {time.monotonic() - t:5.1f}s", flush=True)
        regions.append((name, offset, size, path.name))
    manifest = {"created": dt.datetime.now().astimezone().isoformat(timespec="seconds"),
                "chip": "esp32s3", "mac": mac, "board_id": "deskclip-" + mac.replace(":", ""),
                "partitions": table, "seconds": round(time.monotonic() - start, 1),
                "regions": []}
    for name, offset, size, file in regions:
        path = out / file
        if path.stat().st_size != size:
            raise SystemExit(f"{file} 大小不符")
        manifest["regions"].append({"name": name, "offset": offset, "size": size, "file": file, "sha256": sha256(path)})
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False))
    (out / "SHA256SUMS").write_text("".join(f"{r['sha256']}  {r['file']}\n" for r in manifest["regions"]))
    for p in out.iterdir():
        p.chmod(0o600)  # contains pairing tokens
    print(f"备份完成：{out}（{manifest['seconds']}s，开发板 {manifest['board_id']}）")


def restore(args):
    src = Path(args.backup).resolve()
    manifest = json.loads((src / "manifest.json").read_text())
    for r in manifest["regions"]:
        if sha256(src / r["file"]) != r["sha256"] or (src / r["file"]).stat().st_size != r["size"]:
            raise SystemExit(f"{r['file']} 校验失败，未写入任何内容")
    print("备份文件 SHA-256 全部一致")
    mac = board_mac(args.port)
    if mac != manifest["mac"] and not args.other_board:
        raise SystemExit(f"这块开发板 MAC {mac} ≠ 备份 {manifest['mac']}；配对凭据只对原开发板有效（--other-board 可强制）")
    names = {"partition-table", "nvs", "phy_init", "otadata", "hubdata"}
    if args.app:
        names |= {"bootloader"} | {n for n, p in manifest["partitions"].items() if p["type"] == 0}
    chosen = [r for r in manifest["regions"] if r["name"] in names]
    missing = names - {r["name"] for r in chosen} - {"otadata"}
    if args.app and missing:
        raise SystemExit(f"备份里没有 {sorted(missing)}（备份时未加 --app）")
    pairs = []
    for r in chosen:
        pairs += [hex(r["offset"]), str(src / r["file"])]
        print(f"写入 {r['name']:16s} 0x{r['offset']:06x} +0x{r['size']:06x}")
    if args.dry_run:
        print("--dry-run：未写入")
        return
    start = time.monotonic()
    esptool(args.port, "write-flash", *pairs, after="watchdog-reset")  # verifies each region's hash
    print(f"恢复完成（{time.monotonic() - start:.1f}s），开发板已重启")
    if not args.app:
        print("如板上还不是 DeskClipboard 固件，请再刷入 firmware/clipboard/build 的 bootloader 与应用，或用 --app 从备份恢复")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", default="/dev/cu.usbmodem1101")
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("backup")
    b.add_argument("--no-app", dest="app", action="store_false", help="不备份 bootloader 和应用（只备份配对与历史）")
    r = sub.add_parser("restore")
    r.add_argument("backup", help="backups/board-<时间> 目录")
    r.add_argument("--app", action="store_true", help="同时从备份写回 bootloader 和 DeskClipboard 应用")
    r.add_argument("--other-board", action="store_true")
    r.add_argument("--dry-run", action="store_true")
    args = p.parse_args()
    if not Path(args.port).exists():
        raise SystemExit(f"找不到 {args.port}：请按住 BOOT 拔插 USB 进入下载模式（303A:1001）")
    (backup if args.cmd == "backup" else restore)(args)


if __name__ == "__main__":
    main()
