"""Readable, non-repeating connection-error logging with exponential backoff.

Only exception metadata is logged (type, message, errno/winerror, cause);
BLE and OS errors never carry clipboard text, and messages are truncated.
"""
from __future__ import annotations

import time

_LIMIT = 240


def describe_error(exc: BaseException) -> str:
    parts = []
    seen = set()
    while exc is not None and id(exc) not in seen and len(parts) < 3:
        seen.add(id(exc))
        text = type(exc).__name__
        codes = []
        winerror = getattr(exc, "winerror", None)
        errno = getattr(exc, "errno", None)
        if winerror is not None:
            codes.append(f"winerror={winerror}")
        elif errno is not None:
            codes.append(f"errno={errno}")
        message = getattr(exc, "strerror", None) or str(exc)
        message = " ".join(str(message).split())
        if codes:
            text += "[" + ",".join(codes) + "]"
        if message:
            text += "：" + message
        parts.append(text)
        exc = exc.__cause__ or (None if exc.__suppress_context__ else exc.__context__)
    out = " ← ".join(parts)
    return out if len(out) <= _LIMIT else out[:_LIMIT - 1] + "…"


class RetryLog:
    """Log the first occurrence of an error, then a periodic summary; back off 2→30 s."""

    def __init__(self, log, base=2.0, cap=30.0, summary_every=60.0, clock=time.monotonic):
        self.log, self.base, self.cap, self.summary_every, self.clock = log, base, cap, summary_every, clock
        self.failures = 0
        self.last_text = None
        self.last_summary = 0.0

    def failure(self, prefix: str, exc: BaseException) -> float:
        """Record a failure; returns how long to wait before retrying."""
        self.failures += 1
        text = describe_error(exc)
        now = self.clock()
        if text != self.last_text:
            self.log(f"{prefix}：{text}" + (f"（已失败 {self.failures} 次）" if self.failures > 1 else ""))
            self.last_text = text
            self.last_summary = now
        elif now - self.last_summary >= self.summary_every:
            self.log(f"仍在重试（已失败 {self.failures} 次）：{text}")
            self.last_summary = now
        return self.delay()

    def delay(self) -> float:
        if not self.failures:
            return 0.0
        return min(self.cap, self.base * 2 ** (self.failures - 1))

    def success(self, message: str | None = None):
        if self.failures:
            self.log((message or "已恢复") + f"（此前连续失败 {self.failures} 次）")
        elif message:
            self.log(message)
        self.failures = 0
        self.last_text = None
