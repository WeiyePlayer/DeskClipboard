#!/bin/zsh
# Stop the background DeskClipboard client and remove it from login.
LABEL="com.weiye.deskclipboard"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
launchctl bootout "gui/$UID/$LABEL" 2>/dev/null && echo "已停止后台客户端" || echo "后台客户端未在运行"
rm -f "$PLIST" && echo "已移除开机自启：$PLIST"
echo "日志保留在 ~/Library/Logs/DeskClipboard/。如不再需要，可在「系统设置 → 隐私与安全性 → 辅助功能」关闭 Python。"
