"""A YouTube video's own thumbnail, for one a browser plays: Chrome hands MPRIS a
150 by 83 cover. As the Mac's YouTubeCover.swift: the video is found by searching
its title and channel and taking the result with both exactly, and when the
browser did give a cover, the thumbnail must look the same as it. The channel's
own picture comes with it, square, for where the panel draws a square."""

import io
import json
import re
import urllib.parse
from dataclasses import dataclass
from typing import Callable

import gi

gi.require_version("Soup", "3.0")
from gi.repository import GLib, Soup  # noqa: E402
from PIL import Image  # noqa: E402

AGENT = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15"
SAME_ENOUGH = 12.0  # mean difference over 16 by 9 greys; another video is near 60
LARGEST = "=s480"  # the panel's largest picture
SHORT_PREFIX = "shorts-shelf-item-"


@dataclass(frozen=True)
class Video:
    id: str
    avatar: str | None
    short: bool


@dataclass(frozen=True)
class Found:
    thumbnail: bytes
    channel: bytes | None


def initial_data(page: str):
    start = page.find("var ytInitialData = ")
    if start < 0:
        return None
    start += len("var ytInitialData = ")
    end = page.find(";</script>", start)
    try:
        return json.loads(page[start:end]) if end > 0 else None
    except ValueError:
        return None


def _first(node, pick):
    if isinstance(node, dict):
        found = pick(node)
        if found is not None:
            return found
        values = node.values()
    elif isinstance(node, list):
        values = node
    else:
        return None
    for value in values:
        found = _first(value, pick)
        if found is not None:
            return found
    return None


def _text(renderer: dict, key: str) -> str:
    runs = (renderer.get(key) or {}).get("runs") or [{}]
    return str(runs[0].get("text") or "")


def _at_largest(url: str) -> str:
    return re.sub(r"=s\d+", LARGEST, url)


def avatar_of(renderer: dict) -> str | None:
    link = (renderer.get("channelThumbnailSupportedRenderers") or {}).get("channelThumbnailWithLinkRenderer") or {}
    thumbnails = (link.get("thumbnail") or {}).get("thumbnails") or []
    url = thumbnails[-1].get("url") if thumbnails else None
    return _at_largest(url) if url else None


def find_video(data, title: str, channel: str) -> Video | None:
    wanted_title, wanted_channel = title.lower(), channel.lower()

    def pick(node):
        renderer = node.get("videoRenderer")
        if not isinstance(renderer, dict) or not renderer.get("videoId"):
            return None
        if _text(renderer, "title").lower() != wanted_title:
            return None
        if wanted_channel and _text(renderer, "ownerText").lower() != wanted_channel:
            return None
        return Video(renderer["videoId"], avatar_of(renderer), False)

    return _first(data, pick)


def find_short(data, title: str) -> str | None:
    """A Short is listed by its title and views alone, so its channel is asked for after."""
    wanted = title.lower() + ","

    def pick(node):
        short = node.get("shortsLockupViewModel")
        if not isinstance(short, dict):
            return None
        text, entity = str(short.get("accessibilityText") or ""), str(short.get("entityId") or "")
        if text.lower().startswith(wanted) and entity.startswith(SHORT_PREFIX):
            return entity[len(SHORT_PREFIX):]
        return None

    return _first(data, pick)


def og_image(html: str) -> str | None:
    match = re.search(r'<meta property="og:image" content="([^"]+)"', html)
    return _at_largest(match.group(1)) if match else None


def _greys(data: bytes) -> list[float] | None:
    try:
        image = Image.open(io.BytesIO(data)).convert("RGB").resize((16, 9), Image.Resampling.LANCZOS)
    except (OSError, ValueError, Image.DecompressionBombError):
        return None
    return [(r + g + b) / 3 for r, g, b in image.getdata()]


def difference(a: bytes, b: bytes) -> float | None:
    first, second = _greys(a), _greys(b)
    if first is None or second is None:
        return None
    return sum(abs(x - y) for x, y in zip(first, second)) / len(first)


class SoupFetch:
    def __init__(self):
        self._session = Soup.Session(timeout=5, user_agent=AGENT)

    def get(self, url: str, done: Callable[[bytes | None], None]) -> None:
        try:
            message = Soup.Message.new("GET", url)
        except TypeError:
            message = None
        if message is None:
            GLib.idle_add(lambda: done(None))
            return
        message.get_request_headers().append("Accept-Language", "en")
        self._session.send_and_read_async(message, GLib.PRIORITY_LOW, None, self._read, (message, done))

    @staticmethod
    def _read(session, result, data):
        message, done = data
        try:
            body = session.send_and_read_finish(result).get_data()
        except GLib.Error:
            done(None)
            return
        done(body if message.get_status() == 200 else None)


class YouTube:
    def __init__(self, fetch=None):
        self._fetch = fetch or SoupFetch().get

    def find(self, title: str, channel: str, small: bytes | None, done: Callable[[Found | None], None]) -> None:
        query = urllib.parse.urlencode({"search_query": f"{title} {channel}"})
        self._fetch(f"https://www.youtube.com/results?{query}",
                    lambda page: self._searched(page, title, channel, small, done))

    def _searched(self, page, title, channel, small, done):
        data = initial_data(page.decode("utf-8", "replace")) if page else None
        if data is None:
            done(None)
            return
        video = find_video(data, title, channel)
        if video is not None:
            self._thumbnail(video, small, done)
            return
        short = find_short(data, title)
        if short is None:
            done(None)
            return
        query = urllib.parse.urlencode({"url": f"https://www.youtube.com/shorts/{short}", "format": "json"})
        self._fetch(f"https://www.youtube.com/oembed?{query}",
                    lambda about: self._short_channel(about, short, channel, small, done))

    def _short_channel(self, about, short, channel, small, done):
        try:
            facts = json.loads(about) if about else {}
        except ValueError:
            facts = {}
        author = str(facts.get("author_name") or "") if isinstance(facts, dict) else ""
        if not author or (channel and author.lower() != channel.lower()):
            done(None)  # another channel's Short of the same name
            return
        home = facts.get("author_url")
        if not home:
            self._thumbnail(Video(short, None, True), small, done)
            return
        self._fetch(home, lambda html: self._thumbnail(
            Video(short, og_image(html.decode("utf-8", "replace")) if html else None, True), small, done))

    def _thumbnail(self, video, small, done, sizes=("maxresdefault", "mqdefault")):
        if not sizes:
            done(None)
            return

        def got(data):
            if data is None:
                self._thumbnail(video, small, done, sizes[1:])
                return
            # a Short's cover is its upright frame, never like the wide one; its channel was made sure of instead
            if not video.short and small is not None:
                apart = difference(small, data)
                if apart is not None and apart > SAME_ENOUGH:
                    done(None)  # a video of the same name, but not this one
                    return
            if video.avatar is None:
                done(Found(data, None))
                return
            self._fetch(video.avatar, lambda picture: done(Found(data, picture)))

        self._fetch(f"https://i.ytimg.com/vi/{video.id}/{sizes[0]}.jpg", got)
