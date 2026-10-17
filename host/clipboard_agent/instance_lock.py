"""One client per user: an OS file lock that disappears with the process.

The lock is advisory and kernel-held (flock / msvcrt), so a crashed client
never leaves a stale lock behind. The PID inside is informational only.
"""
from __future__ import annotations

import os
from pathlib import Path
import sys
import time


class AlreadyRunning(RuntimeError):
    def __init__(self, pid: str):
        super().__init__(pid)
        self.pid = pid


class InstanceLock:
    def __init__(self, path: Path):
        self.path = Path(path)
        self.fd: int | None = None

    def _try(self) -> bool:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(self.path, os.O_RDWR | os.O_CREAT, 0o600)
        try:
            if sys.platform == "win32":
                import msvcrt
                os.lseek(fd, 0, os.SEEK_SET)
                msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(fd)
            return False
        # Byte 0 is the Windows lock region; keep the PID after it.
        os.lseek(fd, 1 if sys.platform == "win32" else 0, os.SEEK_SET)
        os.ftruncate(fd, 1 if sys.platform == "win32" else 0)
        os.write(fd, str(os.getpid()).encode())
        self.fd = fd
        return True

    def holder(self) -> str:
        try:
            return self.path.read_bytes().strip(b"\0 ").decode(errors="replace") or "?"
        except OSError:
            return "?"

    def acquire(self, wait: bool = False, poll: float = 2.0, on_wait=None) -> "InstanceLock":
        announced = False
        while not self._try():
            if not wait:
                raise AlreadyRunning(self.holder())
            if not announced and on_wait:
                on_wait(self.holder())
                announced = True
            time.sleep(poll)
        return self

    def release(self):
        if self.fd is not None:
            try:
                if sys.platform == "win32":
                    import msvcrt
                    os.lseek(self.fd, 0, os.SEEK_SET)
                    msvcrt.locking(self.fd, msvcrt.LK_UNLCK, 1)
            finally:
                os.close(self.fd)
                self.fd = None

    def __enter__(self):
        return self.acquire()

    def __exit__(self, *exc):
        self.release()
