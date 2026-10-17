import asyncio
import unittest

from clipboard_agent.ble_transport import BLETransport


class BLEFlowControlTest(unittest.IsolatedAsyncioTestCase):
    async def exchange(self, native):
        transport = BLETransport.__new__(BLETransport)
        transport.loop = asyncio.get_running_loop()
        transport.responses = asyncio.Queue()
        transport.fragment = 20
        transport.characteristic = "write"
        writes = []
        attempts = 0
        def can_write():
            nonlocal attempts
            attempts += 1
            return attempts > 1
        transport.can_write = can_write if native else None
        class Client:
            is_connected = True
            async def write_gatt_char(self, characteristic, data, response):
                writes.append((bytes(data), response))
                if sum(len(w[0]) for w in writes) == 104:
                    transport.responses.put_nowait(b"reply")
        transport.client = Client()
        self.assertEqual(await transport._exchange(b"x" * 100), b"reply")
        self.assertEqual(b"".join(w[0] for w in writes), b"d\0\0\0" + b"x" * 100)
        return writes, attempts

    async def test_native_capacity_waits_then_confirms_last_fragment(self):
        writes, attempts = await self.exchange(True)
        self.assertGreater(attempts, len(writes))
        self.assertEqual([w[1] for w in writes], [False] * 5 + [True])

    async def test_other_backends_use_bounded_write_window(self):
        writes, _ = await self.exchange(False)
        self.assertEqual([i for i,w in enumerate(writes) if w[1]], [3,5])


if __name__=="__main__":unittest.main()
