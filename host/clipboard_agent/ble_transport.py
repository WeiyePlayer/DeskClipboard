"""Synchronous companion API over a dedicated Bleak/CoreBluetooth/WinRT loop."""
from __future__ import annotations

import asyncio
import concurrent.futures
import json
import struct
import threading

from clipboard_agent.ble_security import Handshake, Stream, SERVICE, WRITE, NOTIFY
from clipboard_agent.hub_protocol import HubError
from clipboard_agent.hub_transport import checked
from clipboard_agent.retry_log import describe_error


class BLETransport:
    def __init__(self, config: dict):
        self.config = config
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.client = None
        self.cipher = None
        self.thread.start()
        try:
            self._submit(self._connect(), 30)
        except Exception as exc:
            self.close()
            if isinstance(exc, (HubError, OSError)):
                raise
            raise OSError("无法建立蓝牙连接：" + describe_error(exc)) from exc

    def _run(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()
        pending = asyncio.all_tasks(self.loop)
        for task in pending:
            task.cancel()
        if pending:
            self.loop.run_until_complete(asyncio.gather(*pending, return_exceptions=True))
        self.loop.close()

    def _submit(self, coroutine, timeout=45):
        if self.loop.is_closed() or not self.thread.is_alive():
            coroutine.close()
            raise OSError("蓝牙连接已关闭")
        future = asyncio.run_coroutine_threadsafe(coroutine, self.loop)
        try:
            return future.result(timeout)
        except concurrent.futures.TimeoutError as exc:
            future.cancel()
            raise TimeoutError("蓝牙请求超时") from exc

    async def _connect(self):
        from bleak import BleakClient, BleakScanner
        name = "DeskClip-" + self.config["board_id"][-6:]
        device = await BleakScanner.find_device_by_filter(
            lambda d, a: a.local_name == name and SERVICE in [s.lower() for s in a.service_uuids],
            timeout=12, service_uuids=[SERVICE])
        if device is None:
            raise OSError("未找到已配对的开发板，请检查蓝牙和供电")
        self.responses = asyncio.Queue(maxsize=2)
        self.stream = Stream()
        self.client = BleakClient(device, disconnected_callback=self._disconnected)
        await self.client.connect()
        await self.client.start_notify(NOTIFY, self._notification)
        self.characteristic = self.client.services.get_characteristic(WRITE)
        self.fragment = min(244, self.characteristic.max_write_without_response_size)
        # Bleak 2.1.1's CoreBluetooth no-response path returns immediately.
        # Use Apple's native transmit-capacity signal when this backend exposes
        # its CBPeripheral; other backends retain a bounded ATT request window.
        peripheral = getattr(getattr(self.client, "_backend", None), "_peripheral", None)
        self.can_write = getattr(peripheral, "canSendWriteWithoutResponse", None)
        handshake = Handshake(self.config["board_id"], self.config["device_id"], self.config["token"])
        self.cipher = handshake.finish(await self._exchange(handshake.request()))

    def _disconnected(self, unused):
        if hasattr(self, "responses"):
            try:
                self.responses.put_nowait(OSError("蓝牙已断开"))
            except asyncio.QueueFull:
                pass

    def _notification(self, unused, data):
        try:
            for record in self.stream.feed(bytes(data)):
                self.responses.put_nowait(record)
        except (HubError, asyncio.QueueFull) as exc:
            while not self.responses.empty():
                self.responses.get_nowait()
            self.responses.put_nowait(exc)

    async def _exchange(self, record):
        if not self.client or not self.client.is_connected:
            raise OSError("蓝牙未连接")
        framed = struct.pack("<I", len(record)) + record
        for offset in range(0, len(framed), self.fragment):
            # Native capacity signals bound macOS's queue. Other backends use
            # a request every four fragments; the last request confirms that
            # all preceding ordered writes have reached the peripheral.
            last = offset + self.fragment >= len(framed)
            if callable(self.can_write):
                deadline = self.loop.time() + 10
                while not self.can_write():
                    if not self.client.is_connected or self.loop.time() >= deadline:
                        raise OSError("蓝牙发送队列超时")
                    await asyncio.sleep(0.002)
                acknowledged = last
            else:
                acknowledged = (offset // self.fragment + 1) % 4 == 0 or last
            await self.client.write_gatt_char(self.characteristic,
                framed[offset:offset + self.fragment], response=acknowledged)
            await asyncio.sleep(0)
        response = await asyncio.wait_for(self.responses.get(), timeout=30)
        if isinstance(response, Exception):
            raise response
        return response

    async def _secure(self, payload):
        return self.cipher.decrypt(await self._exchange(self.cipher.encrypt(payload)))

    def _call(self, payload):
        try:
            response = self._submit(self._secure(payload))
        except Exception as exc:
            self.close()
            if isinstance(exc, (HubError, OSError)):
                raise
            raise OSError("蓝牙通信中断：" + describe_error(exc)) from exc
        if response[:1] == b"J":
            return checked(json.loads(response[1:]))
        if response[:1] == b"B":
            return response[1:]
        raise HubError("invalid_ble_response")

    def request(self, data):
        return self._call(b"J" + json.dumps(data, ensure_ascii=False, separators=(",", ":")).encode())

    def chunk(self, event, part, offset, data):
        return self._call(b"U" + event.encode("ascii") + bytes([part == "card"])
                          + struct.pack("<I", offset) + data)

    def download(self, job, offset, size):
        result = self._call(b"D" + job.encode("ascii") + struct.pack("<II", offset, size))
        if not isinstance(result, bytes):
            raise HubError("invalid_ble_response")
        return result

    def close(self):
        if self.loop.is_closed() or not self.thread.is_alive():
            return
        try:
            if self.client and self.client.is_connected:
                self._submit(self.client.disconnect(), 5)
        except Exception:
            pass
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(timeout=6)
