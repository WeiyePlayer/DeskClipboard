"""Real-board check of phone pairing by code, run from the Mac.

Opens the pairing window over USB (needs this Mac's existing credential),
pairs a temporary "android" test peer over BLE (stop the Mac
background client first: macOS shares one BLE link per board between apps) with the code, runs an
authenticated status request with the derived credential, checks that a
wrong code is rejected, then revokes the test peer. Nothing is printed
that is secret (codes are single-use and the test peer is removed).

  PYTHONPATH=host .venv/bin/python host/pair_code_check.py [--port /dev/cu.usbmodem1234561]
"""
from __future__ import annotations

import argparse
import json
import sys
import uuid
from pathlib import Path

from clipboard_agent.ble_transport import BLETransport
from clipboard_agent.hub_client import support_dir
from clipboard_agent.hub_protocol import HubError
from clipboard_agent.hub_transport import USBTransport
from clipboard_agent.pake import PairClient


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="/dev/cu.usbmodem1234561")
    parser.add_argument("--config", type=Path, default=support_dir() / "client-v4.json")
    args = parser.parse_args(argv)
    config = json.loads(args.config.read_text())
    usb = USBTransport(args.port, config["token"])
    device = str(uuid.uuid4())
    results = {}
    try:
        code = usb.request({"op": "pair_code"})["code"]
        client = PairClient(code, device, "pair-check", "android")
        transport = BLETransport({"pair": client, "board_id": "", "device_id": device, "token": ""})
        try:
            status = transport.request({"op": "status"})
            results["pair_and_session"] = status.get("board_id") == config["board_id"]
        finally:
            transport.close()
        # Single use: the same code must not pair again.
        try:
            BLETransport({"pair": PairClient(code, str(uuid.uuid4()), "x", "android"),
                          "board_id": "", "device_id": "", "token": ""}).close()
            results["code_single_use"] = False
        except HubError as exc:
            results["code_single_use"] = exc.code == "pairing_closed"
        # Wrong code is rejected and costs one of three attempts.
        good = usb.request({"op": "pair_code"})["code"]
        wrong = format((int(good) + 1) % 1000000, "06d")
        try:
            BLETransport({"pair": PairClient(wrong, str(uuid.uuid4()), "x", "android"),
                          "board_id": "", "device_id": "", "token": ""}).close()
            results["wrong_code_rejected"] = False
        except HubError as exc:
            results["wrong_code_rejected"] = exc.code == "bad_code"
        peers = usb.request({"op": "status"})
        results["status_ok"] = bool(peers.get("ok"))
    except Exception as exc:  # report which step failed instead of a bare traceback
        results["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        try:
            usb.request({"op": "revoke", "device_id": device})
            results["test_peer_revoked"] = True
        except HubError as exc:
            results["test_peer_revoked"] = exc.code
        # A second revoke must find nothing: the test peer is gone.
        try:
            usb.request({"op": "revoke", "device_id": device})
            results["test_peer_gone"] = False
        except HubError as exc:
            results["test_peer_gone"] = exc.code == "not_found"
        usb.close()
    print(json.dumps(results, indent=2))
    return 0 if all(v is True for v in results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
