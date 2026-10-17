"""Correlated USB RPC and shared resumable upload; credentials never go into logs."""
from __future__ import annotations

import base64
import json
import struct
import threading
import time

from clipboard_agent.frames import ACK, HELLO, Decoder, encode_frame
from clipboard_agent.hub_protocol import CHUNK, HubError, Upload

RPC = 0x20
RPC_REPLY = 0x21


def checked(response: dict) -> dict:
    if not response.get("ok"):
        raise HubError(response.get("error", "invalid_response"))
    return response


class USBTransport:
    def __init__(self, port: str, token: str = "") -> None:
        import serial
        self.serial = serial.Serial(port, 115200, timeout=0.1, write_timeout=5)
        self.serial.dtr = True
        self.decoder = Decoder()
        self.sequence = 0
        self.lock = threading.Lock()
        self.version = None
        self.device_id = None
        self.token = token

    def close(self) -> None:
        self.serial.close()

    def request(self, data: dict, timeout: float = 15) -> dict:
        with self.lock:
            self.sequence += 1
            request_id = self.sequence
            if self.token:
                data = {"token": self.token, **data}
            body = json.dumps(data, ensure_ascii=False, separators=(",", ":")).encode()
            if len(body) > 48 * 1024:
                raise HubError("request_too_large")
            self.serial.write(encode_frame(RPC, struct.pack("<I", request_id) + body))
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                frames, _ = self.decoder.feed(self.serial.read(self.serial.in_waiting or 1))
                for frame in frames:
                    if frame.type == HELLO and frame.payload:
                        self.version = frame.payload[0]
                        if self.version != 4:
                            raise HubError("firmware_upgrade_required")
                    elif frame.type == RPC_REPLY and len(frame.payload) >= 4:
                        if struct.unpack_from("<I", frame.payload)[0] == request_id:
                            return checked(json.loads(frame.payload[4:]))
            raise TimeoutError("开发板没有响应 USB 配置请求")

    def chunk(self, event: str, part: str, offset: int, data: bytes) -> dict:
        return self.request({"op": "chunk", "event_id": event, "part": part,
                             "offset": offset, "data": base64.b64encode(data).decode()})

    def download(self, job: str, offset: int, size: int) -> bytes:
        reply = self.request({"op": "fetch", "job_id": job, "offset": offset, "size": size})
        return base64.b64decode(reply["data"], validate=True)



def upload(transport, item: Upload, progress=None) -> int:
    reply = transport.request({"op": "begin", "metadata": item.metadata})
    if reply.get("record_id"):
        return int(reply["record_id"])
    event = item.metadata["event_id"]
    try:
        for part, data in (("content", item.content), ("card", item.card)):
            offset = int(reply.get(part + "_offset", 0))
            if not 0 <= offset <= len(data):
                raise HubError("invalid_offset")
            while offset < len(data):
                chunk = data[offset:offset + CHUNK]
                transport.chunk(event, part, offset, chunk)
                offset += len(chunk)
                if progress:
                    progress()
        return int(transport.request({"op": "commit", "event_id": event})["record_id"])
    except HubError:
        try:
            transport.request({"op": "abort", "event_id": event})
        except Exception:
            pass
        raise
