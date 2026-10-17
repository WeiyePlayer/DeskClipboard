import threading
import time
import unittest
import uuid

from clipboard_agent.retry_log import RetryLog, describe_error
from clipboard_agent.hub_client import HubClient
from test_hub_client import FakeClipboard


class Clock:
    def __init__(self): self.t = 0.0
    def __call__(self): return self.t


class DescribeTest(unittest.TestCase):
    def test_message_errno_and_cause(self):
        try:
            try:
                raise OSError(13, "Permission denied")
            except OSError as inner:
                raise OSError("无法建立蓝牙连接：x") from inner
        except OSError as exc:
            text = describe_error(exc)
        self.assertIn("无法建立蓝牙连接", text)
        self.assertIn("errno=13", text)
        self.assertIn("Permission denied", text)

    def test_winerror_is_reported(self):
        exc = OSError("连接失败")
        exc.winerror = -2147023673
        self.assertIn("winerror=-2147023673", describe_error(exc))

    def test_type_only_when_no_message_and_truncated(self):
        self.assertEqual(describe_error(TimeoutError()), "TimeoutError")
        self.assertLessEqual(len(describe_error(OSError("x" * 1000))), 240)


class RetryLogTest(unittest.TestCase):
    def test_collapses_repeats_with_periodic_summary_and_backoff(self):
        lines, clock = [], Clock()
        r = RetryLog(lines.append, clock=clock)
        delays = []
        for _ in range(8):
            delays.append(r.failure("连接失败", OSError("busy")))
            clock.t += delays[-1]
        self.assertEqual(delays, [2, 4, 8, 16, 30, 30, 30, 30])
        self.assertEqual(lines[0], "连接失败：OSError：busy")
        summaries = [l for l in lines if l.startswith("仍在重试")]
        self.assertTrue(summaries and len(lines) < 8)
        self.assertIn("已失败", summaries[0])
        r.failure("连接失败", OSError("other"))  # a different error is logged at once
        self.assertIn("other", lines[-1])
        r.success("蓝牙已连接")
        self.assertIn("此前连续失败 9 次", lines[-1])
        self.assertEqual(r.delay(), 0)


class ClientReconnectTest(unittest.TestCase):
    def test_connect_failures_back_off_and_are_logged_once(self):
        lines = []
        config = {"device_id": str(uuid.uuid4()), "name": "Mac", "board_id": "t"}
        client = HubClient(config, FakeClipboard())
        client.log = lines.append
        waits = []
        attempts = []
        def connect():
            attempts.append(1)
            if len(attempts) >= 4:
                client.stop.set()
            raise OSError("无法建立蓝牙连接：BleakError：Could not start notify")
        client.connect = connect
        client.stop.wait = lambda t=None: waits.append(t) or client.stop.is_set()
        client.network()
        self.assertEqual(waits[:3], [2, 4, 8])
        self.assertEqual(len([l for l in lines if "Could not start notify" in l]), 1)


if __name__ == "__main__":
    unittest.main()
