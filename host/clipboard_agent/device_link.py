"""USB serial session that exchanges binary frames."""

from __future__ import annotations

import queue
import threading
import time

import serial

from clipboard_agent.frames import ACK, ENTER_BOOT, HELLO, NACK, Decoder, Frame, encode_frame

TINYUSB_VID = 0x303A
TINYUSB_PID = 0x4001


def candidate_ports() -> list[str]:
    from serial.tools import list_ports

    return [
        info.device
        for info in list_ports.comports()
        if info.vid == TINYUSB_VID and info.pid == TINYUSB_PID
    ]


class DeviceLink:
    def __init__(self, port: str) -> None:
        self._serial = serial.Serial(port, 115200, timeout=0.2)
        self._serial.dtr = True
        self._decoder = Decoder()
        self._events: queue.Queue[Frame] = queue.Queue()
        self._acks: queue.Queue[Frame] = queue.Queue()
        self._lock = threading.Lock()
        self._closed = False
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()

    def close(self) -> None:
        self._closed = True
        self._serial.close()

    def send(self, frame: bytes, timeout: float = 3) -> None:
        for _ in range(2):
            self._drop_stale_acks()
            with self._lock:
                self._serial.write(frame)
            try:
                reply = self._acks.get(timeout=timeout)
            except queue.Empty as exc:
                raise TimeoutError("开发板没有确认这一帧") from exc
            if reply.type == ACK:
                return
        raise TimeoutError("开发板拒绝了这一帧")

    def _drop_stale_acks(self) -> None:
        # A late ACK/NACK from an earlier timed-out frame must not be taken as
        # the answer to the frame we are about to send.
        while True:
            try:
                self._acks.get_nowait()
            except queue.Empty:
                return

    def enter_boot(self) -> None:
        self.send(encode_frame(ENTER_BOOT))

    def wait_hello(self, timeout: float) -> Frame | None:
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                frame = self._events.get(timeout=0.2)
            except queue.Empty:
                continue
            if frame.type == HELLO:
                return frame
            self._events.put(frame)
            time.sleep(0.05)
        return None

    def next_event(self, timeout: float) -> Frame | None:
        try:
            return self._events.get(timeout=timeout)
        except queue.Empty:
            return None

    def _read_loop(self) -> None:
        while not self._closed:
            try:
                # read(4096) would sit out the whole 0.2 s timeout unless 4 KB
                # arrived, delaying every 10-byte ACK by 200 ms. Take what is
                # there, or block for a single byte.
                chunk = self._serial.read(self._serial.in_waiting or 1)
            except serial.SerialException:
                return
            if not chunk:
                continue
            frames, errors = self._decoder.feed(chunk)
            for _ in range(errors):
                with self._lock:
                    try:
                        self._serial.write(encode_frame(NACK))
                    except serial.SerialException:
                        return
            for frame in frames:
                if frame.type in (ACK, NACK):
                    self._acks.put(frame)
                else:
                    self._events.put(frame)
