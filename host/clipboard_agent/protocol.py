"""Line protocol shared by the phase-1 firmware and this host.

Device to host: ``CLIP HELLO 1``, ``CLIP PASTE text``, ``CLIP PASTE image``.
Host to device: ``PING``, ``TEST text``, ``TEST image``, ``ENTER-BOOT``,
``CLIP OK <kind>``, ``CLIP HELD <kind>``, ``CLIP ERR <code>``.
"""

from __future__ import annotations

PASTE_KINDS = ("text", "image")


def parse_device_line(line: str) -> tuple[str, str] | None:
    """Return ``(kind, argument)`` for a device line, or None if it is not ours."""
    body = line.strip("\r\n ")
    if not body.startswith("CLIP "):
        return None
    payload = body[5:]
    if payload.startswith("HELLO "):
        return ("hello", payload[6:])
    if payload.startswith("PASTE "):
        kind = payload[6:]
        if kind in PASTE_KINDS:
            return ("paste", kind)
        return ("error", "unknown-paste")
    return ("error", "unknown")


def format_reply(code: str, kind: str) -> str:
    if code == "ok":
        return f"CLIP OK {kind}\n"
    if code == "held":
        return f"CLIP HELD {kind}\n"
    return f"CLIP ERR {code}\n"
