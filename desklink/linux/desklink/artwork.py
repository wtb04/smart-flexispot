"""The cover as the panel takes it: a baseline JPEG no larger than it draws,
both sides a multiple of eight so its decoder does it in hardware."""

import hashlib
import io
from dataclasses import dataclass
from typing import Callable

import gi

gi.require_version("Soup", "3.0")
from gi.repository import Gio, GLib, Soup  # noqa: E402
from PIL import Image  # noqa: E402

LARGEST = 480


@dataclass(frozen=True)
class Artwork:
    jpeg: bytes
    version: str
    wide: bool


def make(original: bytes) -> Artwork | None:
    try:
        image = Image.open(io.BytesIO(original))
        image.load()
    except (OSError, ValueError, Image.DecompressionBombError):
        return None
    width, height = image.size
    scale = min(1.0, LARGEST / max(width, height))
    size = (max(8, int(width * scale) // 8 * 8), max(8, int(height * scale) // 8 * 8))
    out = io.BytesIO()
    image.convert("RGB").resize(size, Image.Resampling.LANCZOS).save(out, "JPEG", quality=85, progressive=False)
    return Artwork(out.getvalue(), hashlib.sha256(original).digest()[:6].hex(), width > height * 1.2)


class Covers:
    def __init__(self, max_bytes: int = 8 * 1024 * 1024):
        self._max = max_bytes
        self._session = Soup.Session(timeout=5)

    def load(self, url: str, done: Callable[[bytes | None], None]) -> None:
        if url.startswith("file://"):
            Gio.File.new_for_uri(url).load_contents_async(None, self._file_loaded, done)
        elif url.startswith(("http://", "https://")):
            message = Soup.Message.new("GET", url)
            if message is None:
                GLib.idle_add(lambda: done(None))
                return
            self._session.send_and_read_async(message, GLib.PRIORITY_LOW, None, self._fetched, (message, done))
        else:
            GLib.idle_add(lambda: done(None))

    def _file_loaded(self, file, result, done):
        try:
            _, contents, _ = file.load_contents_finish(result)
        except GLib.Error:
            done(None)
            return
        done(contents if len(contents) <= self._max else None)

    def _fetched(self, session, result, data):
        message, done = data
        try:
            body = session.send_and_read_finish(result).get_data() or b""
        except GLib.Error:
            done(None)
            return
        done(body if message.get_status() == 200 and 0 < len(body) <= self._max else None)
