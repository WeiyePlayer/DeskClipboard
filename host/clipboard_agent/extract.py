"""Pull phone numbers, links, emails, and codes out of copied text."""

from __future__ import annotations

import re

_URL = re.compile(r"https?://[^\s<>\"']+")
_EMAIL = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")
_PHONE = re.compile(r"(?<!\d)(1[3-9]\d{9})(?!\d)")
_CODE = re.compile(r"验证码[:：\s]*(\d{4,8})")
_MAX_CHIPS = 3


def extract_chips(text: str) -> list[str]:
    found: list[tuple[int, str]] = []
    spans: list[tuple[int, int]] = []

    def take(matches: list[re.Match[str]], group: int) -> None:
        for match in matches:
            span = match.span(group)
            if any(not (span[1] <= start or span[0] >= end) for start, end in spans):
                continue
            spans.append(span)
            found.append((span[0], match.group(group)))

    take(list(_URL.finditer(text)), 0)
    take(list(_EMAIL.finditer(text)), 0)
    take(list(_PHONE.finditer(text)), 1)
    take(list(_CODE.finditer(text)), 1)
    found.sort(key=lambda item: item[0])
    return [value for _, value in found[:_MAX_CHIPS]]
