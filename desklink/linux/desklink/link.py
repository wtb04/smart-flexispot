"""What this laptop tells the panel and when, and what it does with the panel's
messages back, as the Mac's Desk Link does."""

import enum
import json
import logging
import time
from dataclasses import dataclass
from typing import Callable

from gi.repository import GLib

from . import artwork, seal
from .player import Track
from .settings import Settings

log = logging.getLogger(__name__)

HEARTBEAT_S = 10  # the panel forgets a laptop after 30 s of silence
SOON_S = 0.3
GONE_AFTER_S = 3  # players drop their metadata for a moment around a pause
IDLE_AWAY_S = 120


class Clock:
    def monotonic(self) -> float:
        return time.monotonic()

    def wall_ms(self) -> int:
        return seal.now_ms()


def _guarded(fn: Callable[[], None]) -> None:
    # an exception in a GLib callback would remove its timer for good, the heartbeat with it
    try:
        fn()
    except Exception:
        log.exception("timer callback")


class GLibTimers:
    class Timer:
        def __init__(self):
            self.source = 0

        def cancel(self):
            if self.source:
                GLib.source_remove(self.source)
                self.source = 0

    def after(self, seconds: float, fn: Callable[[], None]) -> "GLibTimers.Timer":
        timer = self.Timer()

        def fire():
            timer.source = 0
            _guarded(fn)
            return False

        timer.source = GLib.timeout_add(int(seconds * 1000), fire)
        return timer

    def every(self, seconds: float, fn: Callable[[], None]) -> "GLibTimers.Timer":
        timer = self.Timer()
        timer.source = GLib.timeout_add(int(seconds * 1000), lambda: _guarded(fn) or True)
        return timer


@dataclass
class Response:
    status: int
    type: str = ""
    body: bytes = b""


class Reach(enum.Enum):
    UNKNOWN = "unknown"
    ANSWERING = "answering"
    REFUSED = "refused"
    UNREACHABLE = "unreachable"


def build_state(machine: str, port: int, track: Track | None, art: artwork.Artwork | None, volume,
                now: float, square: artwork.Artwork | None = None) -> dict:
    state = {"type": "state", "machine": machine, "port": port, "title": ""}
    if track is None:
        return state
    takes = ["pause"]
    if track.duration > 0 and track.can_seek:
        takes.append("seek")
    if track.can_next:
        takes.append("next")
    if track.can_previous:
        takes.append("previous")
    if volume.percent is not None:
        takes.append("volume")
        state["volume"] = volume.percent
        state["muted"] = volume.muted
    state.update(app=track.identity, title=track.title, artist=track.artist, playing=track.playing,
                 position=round(track.position(now), 1), duration=track.duration,
                 art=art.version if art else "", square=square.version if square else "",
                 video=art.wide if art else track.browser, takes=takes)
    return state


class Link:
    def __init__(self, *, settings: Settings, save_settings, key: str, client, volume, covers, player_command,
                 clock, timers, machine: str, locked: Callable[[], bool], youtube=None):
        self.settings = settings
        self._save = save_settings
        self._client = client
        self._volume = volume
        self._covers = covers
        self._command = player_command
        self._clock = clock
        self._timers = timers
        self._machine = machine
        self._locked = locked
        self._youtube = youtube
        self._envelope: seal.Envelope | None = None
        self._listeners: list[Callable[[], None]] = []
        self.track: Track | None = None
        self.artwork: artwork.Artwork | None = None
        self._cover_url = ""
        self._small: bytes | None = None  # the player's own cover, before a video's thumbnail replaces it
        self.square: artwork.Artwork | None = None
        self._looked_up = ""  # the browser video a thumbnail was searched for
        self._search_waits = False
        self._found = False
        self.reach = Reach.UNKNOWN
        self.reaches_back: bool | None = None
        self.firmware = ""
        self._beat = None
        self._soon = None
        self._vanishing = None
        self._sleeping = False
        self._still_since = clock.monotonic()
        self._told_away = False
        self._generation = 0  # bumped by a new address or key, so an older reply is not taken for theirs
        self.set_key(key, store=False)

    # wiring

    def add_listener(self, fn: Callable[[], None]) -> None:
        self._listeners.append(fn)

    def _changed(self) -> None:
        for fn in self._listeners:
            fn()

    def start(self) -> None:
        if self.settings.sharing:
            self._begin()

    def _begin(self) -> None:
        if self._beat is None:
            self._beat = self._timers.every(HEARTBEAT_S, self.report)
        self.report()

    def _end(self) -> None:
        for timer in (self._beat, self._soon, self._vanishing):
            if timer:
                timer.cancel()
        if self._vanishing:
            self.track, self.artwork, self._cover_url = None, None, ""  # it went; there is no later to pass it on
        self._beat = self._soon = self._vanishing = None
        self.report(force_nothing=True)
        self.reach, self.reaches_back = Reach.UNKNOWN, None
        self._changed()

    # what plays

    def take(self, track: Track | None) -> None:
        if self._vanishing:
            self._vanishing.cancel()
            self._vanishing = None
        if track is None and self.track is not None:
            self._vanishing = self._timers.after(GONE_AFTER_S, lambda: self._apply(None))
            return
        self._apply(track)

    def _apply(self, track: Track | None) -> None:
        self._vanishing = None
        url = track.art_url if track else ""
        new_cover = url != self._cover_url
        if new_cover:
            self._cover_url = url
            self._small = None
            if not self._found:
                self.artwork = None
            if url:
                self._covers.load(url, lambda data, wanted=url: self._cover_loaded(wanted, data))
        self._look_up(track, url, new_cover)
        paused = (track is not None and self.track is not None and track.title == self.track.title
                  and track.playing != self.track.playing)
        self.track = track
        self._changed()
        if paused:
            self.report()
        else:
            self.report_soon()

    def _cover_loaded(self, url: str, data: bytes | None) -> None:
        if url != self._cover_url:
            return
        self._small = data
        if not self._found:
            self.artwork = artwork.make(data) if data else None
        if self._search_waits:
            self._search_waits = False
            self._search(data)
        self._changed()
        self.report_soon()

    def _look_up(self, track: Track | None, url: str, new_cover: bool) -> None:
        wanted = f"{track.title}\n{track.artist}" if track and track.browser and self._youtube else ""
        if wanted == self._looked_up:
            return
        self._looked_up = wanted
        self.square = None
        self._search_waits = False
        if self._found:
            self._found = False
            self.artwork = artwork.make(self._small) if self._small else None
        if not wanted:
            return
        if url and new_cover:
            self._search_waits = True  # the small cover goes with the search, for the likeness check
        else:
            self._search(self._small if url else None)

    def _search(self, small: bytes | None) -> None:
        wanted = self._looked_up
        title, artist = wanted.split("\n", 1)
        self._youtube.find(title, artist, small, lambda found: self._video_found(wanted, found))

    def _video_found(self, wanted: str, found) -> None:
        if wanted != self._looked_up or found is None:
            return
        thumbnail = artwork.make(found.thumbnail)
        if thumbnail is None:
            return
        self.artwork, self._found = thumbnail, True
        self.square = artwork.make(found.channel) if found.channel else None
        self._changed()
        self.report_soon()

    def volume_changed(self) -> None:
        if self.track is not None:
            self.report_soon()

    def set_sleeping(self, sleeping: bool) -> None:
        self._sleeping = sleeping
        self.report()

    # telling the panel

    def report_soon(self) -> None:
        if self._soon:
            self._soon.cancel()
        self._soon = self._timers.after(SOON_S, self.report)

    def _away(self) -> bool:
        now = self._clock.monotonic()
        playing = self.track is not None and self.track.playing
        if playing:
            self._still_since = now
        return self._sleeping or (not playing and self._locked() and now - self._still_since > IDLE_AWAY_S)

    def report(self, force_nothing: bool = False) -> None:
        if self._soon:
            self._soon.cancel()
            self._soon = None
        away = self._away()
        if away and self._told_away and not force_nothing:
            return
        self._told_away = away
        if not force_nothing and not self.settings.sharing:
            return
        track = None if (away or force_nothing) else self.track
        state = build_state(self._machine, seal.PORT, track, self.artwork if track else None, self._volume,
                            self._clock.monotonic(), self.square if track else None)
        generation = self._generation
        self._send(state, lambda status, body: self._answered(status, body, generation))

    def _send(self, message: dict, done=None) -> None:
        if not self.settings.panel or self._envelope is None:
            return
        body = self._envelope.sealed(message, "/link")
        self._client.post(f"http://{self.settings.panel}/link", body, done or (lambda *_: None))

    def _answered(self, status: int | None, body: bytes, generation: int | None = None) -> None:
        if not self.settings.sharing or (generation is not None and generation != self._generation):
            return
        if status is None:
            self.reach = Reach.UNREACHABLE
        else:
            answer = self._envelope.opened(body, "/link reply") if status == 200 and self._envelope else None
            if answer is None:
                self.reach = Reach.REFUSED
            else:
                self.reach = Reach.ANSWERING
                if answer.get("type") == "pong":
                    self.firmware = str(answer.get("firmware", ""))
                    reaches = answer.get("reaches")
                    self.reaches_back = reaches if isinstance(reaches, bool) else None
        self._changed()

    # settings

    def set_sharing(self, sharing: bool) -> None:
        if sharing == self.settings.sharing:
            return
        self.settings.sharing = sharing
        self._save(self.settings)
        self._begin() if sharing else self._end()
        self._changed()

    def set_claude(self, claude: bool) -> None:
        self.settings.claude = claude
        self._save(self.settings)

    def set_panel(self, panel: str) -> None:
        self.settings.panel = panel.strip()
        self._save(self.settings)
        self._generation += 1
        self.reach, self.reaches_back = Reach.UNKNOWN, None
        self.report()
        self._changed()

    def set_key(self, hex_key: str, store: bool = True) -> bool:
        if store and not hex_key:
            return False  # an emptied field is no key to forget the old one for
        key = seal.parse_key(hex_key) if hex_key else None
        if hex_key and key is None:
            return False
        self._generation += 1
        self._envelope = seal.Envelope(key, clock_ms=self._clock.wall_ms) if key else None
        self.reach, self.reaches_back = Reach.UNKNOWN, None
        if store:
            self.report()
            self._changed()
        return True

    def has_key(self) -> bool:
        return self._envelope is not None

    def status_text(self) -> str:
        panel = self.settings.panel
        if not self.settings.sharing:
            return "Not sharing"
        if self._envelope is None:
            return "Needs the panel's key"
        if self.reach is Reach.UNKNOWN:
            return f"Looking for {panel}"
        if self.reach is Reach.REFUSED:
            return "Wrong key, or not the panel"
        if self.reach is Reach.UNREACHABLE:
            return f"{panel} not reachable"
        if self.reaches_back is True:
            return f"Connected to {panel}"
        if self.reaches_back is False:
            return f"{panel} cannot reach this laptop"
        return "Connected, checking back"

    def health(self) -> str:
        if not self.settings.sharing:
            return "off"
        if self._envelope is None or self.reach in (Reach.REFUSED, Reach.UNREACHABLE) or self.reaches_back is False:
            return "trouble"
        return "good" if self.reach is Reach.ANSWERING and self.reaches_back else "checking"

    def play_pause(self) -> None:
        if self.track is not None:
            self._command("pause" if self.track.playing else "play")

    # the panel's messages

    def handle(self, method: str, path: str, body: bytes, local: bool) -> Response:
        try:
            return self._handle(method, path, body, local)
        except Exception:
            log.exception("request to %s", path)
            return Response(500)

    def _handle(self, method: str, path: str, body: bytes, local: bool) -> Response:
        if method == "POST" and path == "/claude":
            return self._relay(body) if local else Response(403)
        if method != "POST" or path not in ("/link", "/cover") or self._envelope is None:
            return Response(403)
        message = self._envelope.opened(body, path)
        if message is None:
            return Response(403)
        reply = path + " reply"
        kind = message["type"]
        if kind == "cover":
            picture = next((art for art in (self.artwork, self.square) if art and art.version == message.get("art")),
                           None)
            if picture is None:
                return Response(404)
            return Response(200, "application/octet-stream", self._envelope.sealed_bytes(picture.jpeg, reply))
        if kind == "ping":
            return Response(200, "application/octet-stream", self._envelope.sealed({"type": "pong"}, reply))
        if kind == "command":
            if not self._run(message):
                return Response(400)
            return Response(200, "application/octet-stream", self._envelope.sealed({"type": "ok"}, reply))
        return Response(400)

    @staticmethod
    def _number(value) -> bool:
        return isinstance(value, (int, float)) and not isinstance(value, bool)

    def _run(self, message: dict) -> bool:
        name = message.get("command")
        if name in ("play", "pause", "next", "previous"):
            self._command(name)
        elif name == "seek":
            if not self._number(message.get("position")):
                return False
            self._command("seek", float(message["position"]))
        elif name == "volume":
            if not self._number(message.get("level")):
                return False
            self._volume.set_percent(int(message["level"]))
        elif name == "mute":
            if not isinstance(message.get("muted"), bool):
                return False
            self._volume.set_muted(message["muted"])
        elif name in ("volume_up", "volume_down"):
            self._volume.step(5 if name == "volume_up" else -5)
        else:
            return False
        return True

    def _relay(self, body: bytes) -> Response:
        if self.settings.claude:
            try:
                event = json.loads(body)
            except ValueError:
                event = None
            if isinstance(event, dict):
                self._send({"type": "claude", "event": event})
        return Response(204)
