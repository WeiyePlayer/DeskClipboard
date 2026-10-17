# DeskClipboard: run the client hidden at logon (Task Scheduler), restart on failure.
# Run: powershell -ExecutionPolicy Bypass -File scripts\install-windows-autostart.ps1
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $projectRoot
$python = Join-Path $projectRoot '.venv-win\Scripts\python.exe'
$pythonw = Join-Path $projectRoot '.venv-win\Scripts\pythonw.exe'
if (-not (Test-Path $python)) {
    py -3 -m venv .venv-win
    if ($LASTEXITCODE -ne 0) { throw 'Python 3.11 or newer is required.' }
    & $python -m pip install -r requirements.txt
    if ($LASTEXITCODE -ne 0) { throw 'Dependency installation failed.' }
}
$dataDir = Join-Path $env:LOCALAPPDATA 'DeskClipboard'
if (-not (Test-Path (Join-Path $dataDir 'client-v4.json'))) {
    throw '尚未配对。请先用 USB 连接开发板并运行 scripts\run-windows.ps1 -Pair'
}
$log = Join-Path $dataDir 'client.log'
$action = New-ScheduledTaskAction -Execute $pythonw `
    -Argument "-m clipboard_agent --wait-for-lock --log-file `"$log`"" `
    -WorkingDirectory (Join-Path $projectRoot 'host')
$trigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew `
    -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1)
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName 'DeskClipboard' -Action $action -Trigger $trigger -Settings $settings `
    -Principal $principal -Force | Out-Null
Start-ScheduledTask -TaskName 'DeskClipboard'
Write-Host "已安装：登录后在后台运行（无窗口）。日志：$log"
Write-Host '如果终端里还开着客户端，请关掉它；后台客户端会在它退出后自动接管。'
