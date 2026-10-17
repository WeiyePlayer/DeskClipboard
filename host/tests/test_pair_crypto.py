"""Firmware main/pair_crypto.c (compiled for the host with ESP-IDF's mbedtls)
against RFC 9382 vectors and the Python reference pake.py."""
import subprocess
import unittest
from pathlib import Path

from clipboard_agent import pake
from test_pake import RFC

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "firmware/sim/build/pair_driver"


def run(*args):
    out = subprocess.run([str(DRIVER), *args], capture_output=True, text=True, timeout=60)
    return out.returncode, out.stdout.split()


@unittest.skipUnless((Path.home() / "esp/esp-idf/components/mbedtls").exists(), "ESP-IDF not installed")
class FirmwarePairCrypto(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["bash", str(ROOT / "firmware/sim/build_pair.sh")], check=True)

    def test_rfc_vectors(self):
        for v in RFC:
            rc, out = run(v["w"], v["A"].decode(), v["B"].decode(), v["pA"], v["y"])
            self.assertEqual(rc, 0, out)
            self.assertEqual(out[1:5], [v["pB"], v["cA"], v["cB"], v["Ke"]])

    def test_matches_python_reference_with_code(self):
        device, board = "0f6d1c2e-6a3b-4c5d-8e9f-a0b1c2d3e4f5", "deskclip-84c7bb78dc60"
        for code in ("123456", "000000", "987650"):
            client = pake.PairClient(code, device, "Pixel", "android")
            server = pake.PairServer(code, board)
            server.reply(client.start())
            rc, out = run("code:" + code, device, board, client.pa.hex(), format(server.y, "064x"))
            self.assertEqual(rc, 0, out)
            self.assertEqual(out[0], format(pake.code_scalar(code), "064x"))
            self.assertEqual(out[1:4], [server.pb.hex(), server.ca.hex(), server.cb.hex()])
            self.assertEqual(out[5], server.token)
            # And the client accepts the firmware's reply and confirmation.
            confirm = client.confirm(b"P\x02" + bytes([len(board)]) + board.encode() + bytes.fromhex(out[1]))
            self.assertEqual(confirm[2:].hex(), out[2])
            self.assertEqual(client.finish(b"P\x04" + bytes.fromhex(out[3])), out[5])

    def test_random_y_and_bad_points(self):
        client = pake.PairClient("424242", "0f6d1c2e-6a3b-4c5d-8e9f-a0b1c2d3e4f5", "x", "android")
        rc, out = run("code:424242", client.device_id, "deskclip-1", client.pa.hex(), "")
        self.assertEqual(rc, 0)
        reply = b"P\x02\x0adeskclip-1" + bytes.fromhex(out[1])
        client.confirm(reply)
        self.assertEqual(client.token, out[5])
        bad = "04" + "11" * 64
        self.assertEqual(run("code:424242", "a", "b", bad, "")[0], 1)
        self.assertEqual(run("code:12345a", "a", "b", client.pa.hex(), "")[0], 1)


if __name__ == "__main__":
    unittest.main()
