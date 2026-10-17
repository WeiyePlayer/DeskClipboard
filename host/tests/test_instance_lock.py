import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest

from clipboard_agent.instance_lock import AlreadyRunning, InstanceLock

HOLD = """
import sys, time
sys.path.insert(0, {host!r})
from clipboard_agent.instance_lock import InstanceLock
lock = InstanceLock({path!r}).acquire()
print("locked", flush=True)
sys.stdin.readline()
"""


class InstanceLockTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "sub" / "client.lock"

    def tearDown(self):
        self.tmp.cleanup()

    def hold_in_child(self):
        host = str(Path(__file__).resolve().parents[1])
        child = subprocess.Popen([sys.executable, "-c", HOLD.format(host=host, path=str(self.path))],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.assertEqual(child.stdout.readline().strip(), "locked")
        return child

    def test_second_instance_is_refused_with_holder_pid(self):
        child = self.hold_in_child()
        try:
            with self.assertRaises(AlreadyRunning) as ctx:
                InstanceLock(self.path).acquire()
            self.assertEqual(ctx.exception.pid, str(child.pid))
        finally:
            child.communicate("\n", timeout=5)

    def test_lock_freed_when_holder_dies(self):
        child = self.hold_in_child()
        child.kill(); child.wait(5)  # crash: no cleanup code runs
        lock = InstanceLock(self.path).acquire()
        self.assertEqual(self.path.read_text(), str(os.getpid()))
        lock.release()

    def test_wait_takes_over_after_holder_exits(self):
        child = self.hold_in_child()
        waited = []
        threading.Timer(0.3, lambda: child.communicate("\n", timeout=5)).start()
        start = time.monotonic()
        lock = InstanceLock(self.path).acquire(wait=True, poll=0.05, on_wait=waited.append)
        self.assertEqual(waited, [str(child.pid)])
        self.assertGreater(time.monotonic() - start, 0.2)
        lock.release()

    def test_release_and_reacquire_in_process(self):
        with InstanceLock(self.path):
            pass
        with InstanceLock(self.path):
            pass


if __name__ == "__main__":
    unittest.main()
