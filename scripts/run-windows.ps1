param([switch]$Pair, [switch]$Status)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $projectRoot
$pythonPath = Join-Path $projectRoot '.venv-win\Scripts\python.exe'
if (-not (Test-Path $pythonPath)) {
    py -3 -m venv .venv-win
    if ($LASTEXITCODE -ne 0) { throw 'Python 3.11 or newer is required.' }
    & $pythonPath -m pip install -r requirements.txt
    if ($LASTEXITCODE -ne 0) { throw 'Dependency installation failed.' }
}
$env:PYTHONPATH = Join-Path $projectRoot 'host'
$clientArguments = @('-m', 'clipboard_agent')
if ($Pair) { $clientArguments += '--pair' }
if ($Status) { $clientArguments += '--status' }
& $pythonPath @clientArguments
exit $LASTEXITCODE
