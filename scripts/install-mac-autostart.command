#!/bin/zsh
# DeskClipboard: start the client at login in the background (LaunchAgent),
# restart it if it crashes. Double-click, or run with --no-load to only write the plist.
set -e
cd "$(dirname "$0")/.."
ROOT="$PWD"
LABEL="com.weiye.deskclipboard"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/DeskClipboard/client.log"
CONFIG="$HOME/Library/Application Support/DeskClipboard/client-v4.json"

if [[ ! -x .venv/bin/python ]]; then
    python3 -m venv .venv
    .venv/bin/python -m pip install -r requirements.txt
fi
if [[ ! -f "$CONFIG" ]]; then
    echo "尚未配对。请先用 USB 连接开发板，运行：./scripts/run-mac.command --pair"
    exit 1
fi
export PYTHONPATH="$ROOT/host"
APP="$(.venv/bin/python -c 'from clipboard_agent.autopaste import mac_python_app; print(mac_python_app())')"

mkdir -p "$HOME/Library/LaunchAgents" "$(dirname "$LOG")"
ROOT="$ROOT" PLIST="$PLIST" LOG="$LOG" LABEL="$LABEL" .venv/bin/python - <<'PY'
import os, plistlib
root = os.environ["ROOT"]
plist = {
    "Label": os.environ["LABEL"],
    "ProgramArguments": [f"{root}/.venv/bin/python", "-u", "-m", "clipboard_agent", "--wait-for-lock"],
    "WorkingDirectory": root,
    "EnvironmentVariables": {"PYTHONPATH": f"{root}/host", "PYTHONUNBUFFERED": "1",
                             "DESKCLIP_BACKGROUND": "1", "LANG": "zh_CN.UTF-8"},
    "RunAtLoad": True,
    "KeepAlive": {"SuccessfulExit": False},   # restart after a crash, not after a clean stop
    "ThrottleInterval": 10,
    "ProcessType": "Interactive",
    "LimitLoadToSessionType": "Aqua",
    "StandardOutPath": os.environ["LOG"],
    "StandardErrorPath": os.environ["LOG"],
}
with open(os.environ["PLIST"], "wb") as f:
    plistlib.dump(plist, f)
PY
plutil -lint "$PLIST" >/dev/null
echo "已写入 $PLIST"

if [[ "$1" == "--no-load" ]]; then
    echo "未加载（--no-load）。稍后运行：launchctl bootstrap gui/$UID \"$PLIST\""
else
    # Clients from before the single-instance lock don't hold it: ask to quit them.
    if pgrep -f -- "-m clipboard_agent" >/dev/null && .venv/bin/python -c '
import sys
from pathlib import Path
from clipboard_agent.instance_lock import InstanceLock, AlreadyRunning
try: InstanceLock(Path.home()/"Library/Application Support/DeskClipboard/client.lock").acquire().release()
except AlreadyRunning: sys.exit(1)'; then
        echo "检测到正在运行的旧版客户端（多半在终端窗口里）。请在那个窗口按 Ctrl-C 退出，然后回到这里按回车。"
        read -r
    fi
    launchctl bootout "gui/$UID/$LABEL" 2>/dev/null || true
    launchctl bootstrap "gui/$UID" "$PLIST"
    echo "已加载：登录后自动在后台运行，崩溃后自动重启。日志：$LOG"
fi

cat <<MSG

—— 一次性授权（自动粘贴需要）——
后台运行时，macOS 把「辅助功能」和「蓝牙」权限记在这个程序上，而不是终端：
  Python  →  $APP
1. 首次启动时若弹出「Python 想要控制这台电脑 / 使用蓝牙」，选择允许或「打开系统设置」。
2. 在「系统设置 → 隐私与安全性 → 辅助功能」中打开「Python」开关。
   列表里没有时点「+」，把刚刚在访达中选中的 Python.app 拖进去。
3. 授权后重启后台客户端：launchctl kickstart -k gui/$UID/$LABEL
（brew 升级 Python 后需重新授权。）
MSG
if [[ "$1" != "--no-load" ]]; then
    open -R "$APP" || true
    open "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility" || true
fi
