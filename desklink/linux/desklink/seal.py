"""The envelope every message to and from the panel goes in, as the panel's
components/laptop/include/link_envelope.h has it."""

import json
import math
import os
import time
from collections import deque

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

PORT = 47801
MAGIC = b"DL1"
NONCE_LEN = 12
TAG_LEN = 16
WINDOW_MS = 60_000
SEEN = 64


class Malformed(Exception):
    pass


class Forged(Exception):
    pass


def now_ms() -> int:
    return int(time.time() * 1000)


def parse_key(text: str) -> bytes | None:
    if len(text) != 64 or any(c not in "0123456789abcdefABCDEF" for c in text):
        return None
    return bytes.fromhex(text)


def _aad(path: str) -> bytes:
    return b"DL1 " + path.encode()


def seal(key: bytes, path: str, plain: bytes, nonce: bytes | None = None) -> bytes:
    nonce = nonce or os.urandom(NONCE_LEN)
    return MAGIC + nonce + AESGCM(key).encrypt(nonce, plain, _aad(path))


def open_(key: bytes, path: str, body: bytes) -> tuple[bytes, bytes]:
    if len(body) < len(MAGIC) + NONCE_LEN + TAG_LEN or body[: len(MAGIC)] != MAGIC:
        raise Malformed
    nonce = body[len(MAGIC) : len(MAGIC) + NONCE_LEN]
    try:
        return AESGCM(key).decrypt(nonce, body[len(MAGIC) + NONCE_LEN :], _aad(path)), nonce
    except InvalidTag:
        raise Forged from None


class Fresh:
    def __init__(self):
        self._seen = deque(maxlen=SEEN)

    def take(self, nonce: bytes, at_ms: int, now_ms: int) -> bool:
        if abs(at_ms - now_ms) > WINDOW_MS or nonce in self._seen:
            return False
        self._seen.append(nonce)
        return True


class Envelope:
    def __init__(self, key: bytes, clock_ms=now_ms):
        self._key = key
        self._clock_ms = clock_ms
        self._fresh = Fresh()

    def sealed(self, message: dict, path: str) -> bytes:
        plain = json.dumps({**message, "at": self._clock_ms()}, separators=(",", ":"), ensure_ascii=False,
                           allow_nan=False)
        return seal(self._key, path, plain.encode())

    def sealed_bytes(self, raw: bytes, path: str) -> bytes:
        return seal(self._key, path, raw)

    def opened(self, body: bytes, path: str) -> dict | None:
        try:
            plain, nonce = open_(self._key, path, body)
            message = json.loads(plain)
        except (Malformed, Forged, ValueError):
            return None
        if not isinstance(message, dict) or not isinstance(message.get("type"), str):
            return None
        at = message.get("at")
        if isinstance(at, bool) or not isinstance(at, (int, float)) or not math.isfinite(at) or at <= 0:
            return None
        return message if self._fresh.take(nonce, int(at), self._clock_ms()) else None
