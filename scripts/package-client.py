"""Build a source-only companion bundle without credentials or user history."""
from pathlib import Path
import zipfile

root = Path(__file__).resolve().parents[1]
output = root / "dist/DeskClipboard-v4-BLE-client.zip"
output.parent.mkdir(exist_ok=True)
paths = [root / name for name in ("README.md", "HUB_V4.md", "requirements.txt",
    "scripts/run-mac.command", "scripts/run-windows.ps1",
    "scripts/install-mac-autostart.command", "scripts/uninstall-mac-autostart.command",
    "scripts/install-windows-autostart.ps1", "scripts/uninstall-windows-autostart.ps1")]
paths += sorted((root / "host/clipboard_agent").glob("*.py"))
with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
    for path in paths:
        archive.write(path, "DeskClipboard/" + str(path.relative_to(root)))
print(f"{output}: {output.stat().st_size} bytes")
