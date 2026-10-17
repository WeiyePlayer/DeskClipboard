"""Pairing-code PAKE: SPAKE2-P256-SHA256-HKDF-HMAC (RFC 9382) plus the
DeskClipboard pairing messages. Reference implementation for the firmware
(main/pair_crypto.c) and the Android client; tests check it against the RFC's
Appendix B vectors.

Why a PAKE and not "ECDH + HMAC(code)": with plain ECDH an active attacker who
sits in the middle learns HMAC(K, code) for a K it knows and can brute-force
the 10^6 codes offline. SPAKE2 gives an attacker exactly one guess per online
run; the board allows 3 wrong confirmations per code, so the odds of a guess
are 3e-6, and a passive sniffer learns nothing about the code or credential.

Roles: the phone/computer is A (uses M, sends first), the board is B (N).
  w   = SHA-512("deskclip-v4/pair-code\\0" || code) mod n     (code = 6 ASCII digits)
  A   = device_id (36-char UUID), B = board_id ("deskclip-<mac>")
  TT, Ke||Ka, KcA||KcB, cA, cB exactly as RFC 9382 sections 3.3/4 (AAD = nil)
  credential = HKDF-SHA256(ikm=Ke, salt=nil, info="deskclip-v4/credential", 32)
The 32-byte credential is the same per-peer secret USB pairing stores; normal
sessions keep using the existing H handshake and AES-GCM records.

Records (inside the usual <u32 length> BLE framing, before any handshake):
  P 01 | device_id[36] | pA[65] | u8 len | name utf-8 (<=64) | u8 len | platform
  P 02 | u8 len | board_id | pB[65]
  P 03 | cA[32]
  P 04 | cB[32]                       (the board has stored the peer)
  P FF | error ascii                  (pairing_closed, peer_limit, bad_code, ...)
"""
from __future__ import annotations

import hashlib
import hmac
import secrets
import struct

# --- P-256 ---------------------------------------------------------------
P = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
N_ORDER = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
A_CURVE = P - 3
B_CURVE = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b
G = (0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296,
     0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5)
INFINITY = None


def on_curve(pt) -> bool:
    if pt is None:
        return False
    x, y = pt
    return 0 <= x < P and 0 <= y < P and (y * y - (x * x * x + A_CURVE * x + B_CURVE)) % P == 0


def add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    (x1, y1), (x2, y2) = p1, p2
    if x1 == x2:
        if (y1 + y2) % P == 0:
            return None
        lam = (3 * x1 * x1 + A_CURVE) * pow(2 * y1, -1, P) % P
    else:
        lam = (y2 - y1) * pow(x2 - x1, -1, P) % P
    x3 = (lam * lam - x1 - x2) % P
    return x3, (lam * (x1 - x3) - y1) % P


def neg(pt):
    return None if pt is None else (pt[0], (-pt[1]) % P)


def mul(k: int, pt):
    """Reference only (not constant time); devices use their crypto libraries."""
    result, addend = None, pt
    k %= N_ORDER
    while k:
        if k & 1:
            result = add(result, addend)
        addend = add(addend, addend)
        k >>= 1
    return result


def encode(pt) -> bytes:
    if pt is None:
        raise ValueError("identity")
    return b"\x04" + pt[0].to_bytes(32, "big") + pt[1].to_bytes(32, "big")


def decode(data: bytes):
    if len(data) == 33 and data[0] in (2, 3):
        x = int.from_bytes(data[1:], "big")
        y = pow((x * x * x + A_CURVE * x + B_CURVE) % P, (P + 1) // 4, P)
        if (y & 1) != (data[0] & 1):
            y = P - y
        pt = (x, y)
    elif len(data) == 65 and data[0] == 4:
        pt = (int.from_bytes(data[1:33], "big"), int.from_bytes(data[33:], "big"))
    else:
        raise ValueError("bad point encoding")
    if not on_curve(pt):
        raise ValueError("point not on P-256")
    return pt


# RFC 9382 section 6 seeds for P-256 (compressed).
M = decode(bytes.fromhex("02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f"))
N = decode(bytes.fromhex("03d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49"))


# --- RFC 9382 key schedule ------------------------------------------------
def _lp(data: bytes) -> bytes:
    return struct.pack("<Q", len(data)) + data


def hkdf(ikm: bytes, info: bytes, length: int, salt: bytes = b"") -> bytes:
    prk = hmac.digest(salt or b"\0" * 32, ikm, "sha256")
    out, block, counter = b"", b"", 1
    while len(out) < length:
        block = hmac.digest(prk, block + info + bytes([counter]), "sha256")
        out += block
        counter += 1
    return out[:length]


def transcript(a: bytes, b: bytes, pa: bytes, pb: bytes, k: bytes, w: int) -> bytes:
    return _lp(a) + _lp(b) + _lp(pa) + _lp(pb) + _lp(k) + _lp(w.to_bytes(32, "big"))


def schedule(tt: bytes):
    """-> Ke, KcA, KcB, cA, cB"""
    digest = hashlib.sha256(tt).digest()
    ke, ka = digest[:16], digest[16:]
    kc = hkdf(ka, b"ConfirmationKeys", 32)
    kca, kcb = kc[:16], kc[16:]
    return ke, kca, kcb, hmac.digest(kca, tt, "sha256"), hmac.digest(kcb, tt, "sha256")


def scalar() -> int:
    while True:
        k = secrets.randbelow(N_ORDER)
        if k:
            return k


# --- DeskClipboard pairing ------------------------------------------------
CODE_DIGITS = 6
PLATFORMS = ("android", "macos", "windows")


def code_scalar(code: str) -> int:
    if len(code) != CODE_DIGITS or not code.isdigit():
        raise ValueError("pairing code must be 6 digits")
    digest = hashlib.sha512(b"deskclip-v4/pair-code\0" + code.encode("ascii")).digest()
    return int.from_bytes(digest, "big") % N_ORDER


def credential(ke: bytes) -> str:
    return hkdf(ke, b"deskclip-v4/credential", 32).hex()


class PairError(Exception):
    def __init__(self, code: str):
        self.code = code
        super().__init__(code)


class PairClient:
    """Side A (phone/computer)."""

    def __init__(self, code: str, device_id: str, name: str, platform: str, x: int | None = None):
        if len(device_id) != 36 or platform not in PLATFORMS:
            raise ValueError("invalid device")
        self.w = code_scalar(code)
        self.device_id, self.name, self.platform = device_id, name, platform
        self.x = x if x is not None else scalar()
        self.pa = encode(add(mul(self.x, G), mul(self.w, M)))
        self.board_id = None
        self.expected_cb = None
        self.token = None

    def start(self) -> bytes:
        name = self.name.encode("utf-8")[:64]
        plat = self.platform.encode("ascii")
        return (b"P\x01" + self.device_id.encode("ascii") + self.pa + bytes([len(name)]) + name
                + bytes([len(plat)]) + plat)

    def confirm(self, reply: bytes) -> bytes:
        check_error(reply)
        if len(reply) < 3 or reply[:2] != b"P\x02":
            raise PairError("invalid_pair_reply")
        size = reply[2]
        if len(reply) != 3 + size + 65:
            raise PairError("invalid_pair_reply")
        board = reply[3:3 + size]
        pb = reply[3 + size:]
        try:
            pb_point = decode(pb)
        except ValueError as exc:
            raise PairError("invalid_pair_reply") from exc
        k = mul(self.x, add(pb_point, neg(mul(self.w, N))))
        if k is None:
            raise PairError("invalid_pair_reply")
        tt = transcript(self.device_id.encode("ascii"), board, self.pa, pb, encode(k), self.w)
        ke, _, _, ca, cb = schedule(tt)
        self.board_id = board.decode("ascii")
        self.expected_cb = cb
        self.token = credential(ke)
        return b"P\x03" + ca

    def finish(self, reply: bytes) -> str:
        check_error(reply)
        if len(reply) != 34 or reply[:2] != b"P\x04" or not hmac.compare_digest(reply[2:], self.expected_cb):
            raise PairError("bad_code")
        return self.token


class PairServer:
    """Side B (board); used by tests to model the firmware."""

    def __init__(self, code: str, board_id: str, y: int | None = None):
        self.w = code_scalar(code)
        self.board_id = board_id
        self.y = y if y is not None else scalar()

    def reply(self, message: bytes) -> bytes:
        if len(message) < 2 + 36 + 65 + 2 or message[:2] != b"P\x01":
            raise PairError("invalid_pair_request")
        self.device_id = message[2:38]
        self.pa = message[38:103]
        rest = message[103:]
        name_len = rest[0]
        self.name = rest[1:1 + name_len].decode("utf-8")
        plat_len = rest[1 + name_len]
        self.platform = rest[2 + name_len:2 + name_len + plat_len].decode("ascii")
        if len(rest) != 2 + name_len + plat_len:
            raise PairError("invalid_pair_request")
        pa_point = decode(self.pa)
        self.pb = encode(add(mul(self.y, G), mul(self.w, N)))
        k = mul(self.y, add(pa_point, neg(mul(self.w, M))))
        tt = transcript(self.device_id, self.board_id.encode("ascii"), self.pa, self.pb, encode(k), self.w)
        ke, _, _, self.ca, self.cb = schedule(tt)
        self.token = credential(ke)
        board = self.board_id.encode("ascii")
        return b"P\x02" + bytes([len(board)]) + board + self.pb

    def finish(self, message: bytes) -> bytes:
        if len(message) != 34 or message[:2] != b"P\x03" or not hmac.compare_digest(message[2:], self.ca):
            return b"P\xffbad_code"
        return b"P\x04" + self.cb


def check_error(reply: bytes):
    if reply[:2] == b"P\xff":
        raise PairError(reply[2:].decode("ascii", "replace") or "pairing_failed")
