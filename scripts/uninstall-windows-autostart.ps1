# Stop the hidden DeskClipboard client and remove the logon task.
$ErrorActionPreference = 'SilentlyContinue'
Stop-ScheduledTask -TaskName 'DeskClipboard'
Unregister-ScheduledTask -TaskName 'DeskClipboard' -Confirm:$false
Write-Host '已移除开机自启。日志保留在 %LOCALAPPDATA%\DeskClipboard\client.log'
