"""USB-provisioned identity and authenticated BLE records (no bearer on air)."""
from __future__ import annotations

import hashlib
import hmac
import secrets
import struct

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from clipboard_agent.hub_protocol import HubError

SERVICE = "89c11000-4e46-4a49-9418-dfc18a624070"
WRITE = "89c11001-4e46-4a49-9418-dfc18a624070"
NOTIFY = "89c11002-4e46-4a49-9418-dfc18a624070"
MAX_FRAME = 1024 * 1024


def derive(token: str, board: str, device: str, client: bytes, server: bytes):
    key = bytes.fromhex(token)
    if len(key) != 32 or len(client) != 16 or len(server) != 16:
        raise HubError("invalid_credentials")
    context = board.encode("ascii") + device.encode("ascii") + client + server
    proof = hmac.digest(key, b"deskclip-v4/server\0" + context, "sha256")
    session = hmac.digest(key, b"deskclip-v4/session\0" + context, "sha256")
    return proof, session


class Cipher:
    def __init__(self, key: bytes, client=True):
        self.aes = AESGCM(key)
        self.send_direction = b"C2B\0" if client else b"B2C\0"
        self.recv_direction = b"B2C\0" if client else b"C2B\0"
        self.sent = self.received = 0

    def encrypt(self, data: bytes) -> bytes:
        self.sent += 1
        header = b"E" + struct.pack("<Q", self.sent)
        return header + self.aes.encrypt(self.send_direction + header[1:], data, header)

    def decrypt(self, data: bytes) -> bytes:
        if len(data) < 26 or data[:1] != b"E":
            raise HubError("invalid_encrypted_record")
        sequence = struct.unpack_from("<Q", data, 1)[0]
        if sequence != self.received + 1:
            raise HubError("replayed_or_missing_record")
        try:
            plain = self.aes.decrypt(self.recv_direction + data[1:9], data[9:], data[:9])
        except Exception as exc:
            raise HubError("device_authentication_failed") from exc
        self.received = sequence
        return plain


class Handshake:
    def __init__(self, board: str, device: str, token: str):
        self.board, self.device, self.token = board, device, token
        self.nonce = secrets.token_bytes(16)

    def request(self):
        return b"H" + self.device.encode("ascii") + self.nonce

    def finish(self, reply: bytes):
        if len(reply) != 49 or reply[:1] != b"H":
            raise HubError("device_authentication_failed")
        proof, key = derive(self.token, self.board, self.device, self.nonce, reply[1:17])
        if not hmac.compare_digest(proof, reply[17:]):
            raise HubError("device_authentication_failed")
        return Cipher(key)


class Stream:
    """ATT writes/notifications are fragments, not application records."""
    def __init__(self):
        self.pending = bytearray()

    def feed(self, data: bytes):
        self.pending.extend(data)
        records = []
        while len(self.pending) >= 4:
            size = struct.unpack_from("<I", self.pending)[0]
            if not 0 < size <= MAX_FRAME:
                self.pending.clear()
                raise HubError("invalid_ble_frame")
            if len(self.pending) < size + 4:
                break
            records.append(bytes(self.pending[4:4 + size]))
            del self.pending[:4 + size]
        return records
