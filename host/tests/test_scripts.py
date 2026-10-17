"""Windows PowerShell 5.1 needs a BOM to read UTF-8 scripts (else GBK mojibake)."""
from pathlib import Path
import re
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = sorted((ROOT / "scripts").glob("*.ps1"))
PS7_ONLY = [r"\?\?", r"\?\.", r"&&", r"\|\|", r"utf8NoBOM", r"\s\?\s.*\s:\s", r"-Parallel"]


class PowerShellScriptsTest(unittest.TestCase):
    def test_found(self):
        self.assertGreaterEqual(len(SCRIPTS), 3)

    def test_utf8_bom_and_crlf(self):
        for path in SCRIPTS:
            data = path.read_bytes()
            self.assertTrue(data.startswith(b"\xef\xbb\xbf"), f"{path.name} lacks UTF-8 BOM")
            data[3:].decode("utf-8")
            self.assertNotIn(b"\n", data.replace(b"\r\n", b""), f"{path.name} has bare LF")

    def test_no_powershell7_only_syntax(self):
        for path in SCRIPTS:
            text = path.read_bytes().decode("utf-8-sig")
            for line in text.splitlines():
                code = line.split("#", 1)[0]
                for pattern in PS7_ONLY:
                    self.assertIsNone(re.search(pattern, code), f"{path.name}: {line!r}")
                self.assertFalse(code.endswith("` "), f"{path.name}: space after line continuation")

    def test_package_keeps_bom(self):
        import runpy, tempfile, os
        runpy.run_path(str(ROOT / "scripts/package-client.py"))
        with zipfile.ZipFile(ROOT / "dist/DeskClipboard-v4-BLE-client.zip") as z:
            names = [n for n in z.namelist() if n.endswith(".ps1")]
            self.assertEqual(len(names), len(SCRIPTS))
            for name in names:
                self.assertTrue(z.read(name).startswith(b"\xef\xbb\xbf"), name)


if __name__ == "__main__":
    unittest.main()
