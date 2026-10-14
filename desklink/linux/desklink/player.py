"""What plays, from MPRIS: every player on the session bus, the one that counts
chosen as the panel should see it, and the panel's commands sent to it."""

import itertools
import logging
import time
from dataclasses import dataclass
from typing import Callable

from gi.repository import Gio, GLib

log = logging.getLogger(__name__)

PREFIX = "org.mpris.MediaPlayer2."
PATH = "/org/mpris/MediaPlayer2"
PLAYER = "org.mpris.MediaPlayer2.Player"
BROWSERS = ("chrome", "chromium", "edge", "firefox", "brave", "vivaldi", "opera")


@dataclass(frozen=True)
class Track:
    identity: str
    title: str
    artist: str
    playing: bool
    duration: float
    elapsed: float
    at: float
    rate: float
    can_seek: bool
    can_next: bool
    can_previous: bool
    art_url: str

    @property
    def browser(self) -> bool:
        name = self.identity.lower()
        return any(browser in name for browser in BROWSERS)

    def position(self, now: float) -> float:
        if not self.playing:
            return self.elapsed
        position = self.elapsed + (now - self.at) * (self.rate if self.rate > 0 else 1.0)
        return min(position, self.duration) if self.duration > 0 else position


@dataclass
class Seen:
    track: Track
    started: float
    order: int


def choose(seen: dict[str, Seen]) -> str | None:
    titled = {name: s for name, s in seen.items() if s.track.title}
    if not titled:
        return None
    playing = {name: s for name, s in titled.items() if s.track.playing}
    pool = playing or titled
    return max(pool, key=lambda name: (pool[name].started, pool[name].order))


def track_from(identity: str, props: dict, elapsed_us: int, now: float) -> Track | None:
    status = props.get("PlaybackStatus", "Stopped")
    meta = props.get("Metadata") or {}
    title = str(meta.get("xesam:title") or "")
    if status == "Stopped" or not title:
        return None
    artist = meta.get("xesam:artist") or []
    return Track(identity=identity, title=title,
                 artist=", ".join(artist) if isinstance(artist, list) else str(artist),
                 playing=status == "Playing", duration=max(0, int(meta.get("mpris:length") or 0)) / 1e6,
                 elapsed=max(0, elapsed_us) / 1e6, at=now, rate=float(props.get("Rate") or 1.0),
                 can_seek=bool(props.get("CanSeek")), can_next=bool(props.get("CanGoNext")),
                 can_previous=bool(props.get("CanGoPrevious")), art_url=str(meta.get("mpris:artUrl") or ""))


class _Player:
    def __init__(self, bus, name, changed, clock):
        self.name = name
        self.elapsed_us = 0
        self._changed = changed
        self._clock = clock
        root = Gio.DBusProxy.new_sync(bus, Gio.DBusProxyFlags.NONE, None, name, PATH, "org.mpris.MediaPlayer2", None)
        identity = root.get_cached_property("Identity")
        self.identity = identity.unpack() if identity else name[len(PREFIX):]
        self.proxy = Gio.DBusProxy.new_sync(bus, Gio.DBusProxyFlags.NONE, None, name, PATH, PLAYER, None)
        self.proxy.connect("g-properties-changed", lambda *_: self.refresh_position())
        self.proxy.connect("g-signal", self._signal)
        self.refresh_position()

    def props(self) -> dict:
        return {key: self.proxy.get_cached_property(key).unpack()
                for key in self.proxy.get_cached_property_names()}

    def refresh_position(self):
        self.proxy.call("org.freedesktop.DBus.Properties.Get", GLib.Variant("(ss)", (PLAYER, "Position")),
                        Gio.DBusCallFlags.NONE, 1000, None, self._position, None)

    def _position(self, proxy, result, _data):
        try:
            self.elapsed_us = int(proxy.call_finish(result).unpack()[0])
        except (GLib.Error, TypeError, ValueError):
            pass
        self._changed(self)

    def _signal(self, _proxy, _sender, signal, params):
        if signal == "Seeked":
            self.elapsed_us = int(params.unpack()[0])
            self._changed(self)

    def track(self) -> Track | None:
        return track_from(self.identity, self.props(), self.elapsed_us, self._clock())


class Mpris:
    def __init__(self, on_change: Callable[[Track | None], None], clock=time.monotonic):
        self._on_change = on_change
        self._clock = clock
        self._players: dict[str, _Player] = {}
        self._seen: dict[str, Seen] = {}
        self._order = itertools.count()
        self._chosen: str | None = None
        self._bus = None

    def start(self) -> None:
        self._bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self._bus.signal_subscribe("org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                                   "/org/freedesktop/DBus", None, Gio.DBusSignalFlags.NONE, self._owner_changed)
        names = self._bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                    "ListNames", None, None, Gio.DBusCallFlags.NONE, -1, None).unpack()[0]
        for name in names:
            if name.startswith(PREFIX):
                self._add(name)

    def _owner_changed(self, _bus, _sender, _path, _iface, _signal, params):
        name, old, new = params.unpack()
        if not name.startswith(PREFIX):
            return
        if old:
            self._players.pop(name, None)
            self._seen.pop(name, None)
            self._update()
        if new:
            self._add(name)

    def _add(self, name: str) -> None:
        try:
            self._players[name] = _Player(self._bus, name, self._player_changed, self._clock)
        except GLib.Error as error:
            log.info("skipping player %s: %s", name, error.message)

    def _player_changed(self, changed: _Player) -> None:
        track = changed.track()
        if track is None:
            self._seen.pop(changed.name, None)
        else:
            before = self._seen.get(changed.name)
            started = before.started if before else 0.0
            if track.playing and not (before and before.track.playing):
                started = self._clock()
            order = before.order if before else next(self._order)
            self._seen[changed.name] = Seen(track, started, order)
        self._update()

    def _update(self) -> None:
        self._chosen = choose(self._seen)
        self._on_change(self._seen[self._chosen].track if self._chosen else None)

    def command(self, name: str, position: float | None = None) -> bool:
        target = self._players.get(self._chosen or "")
        if target is None:
            return False
        if name == "seek":
            if position is None:
                return False
            meta = target.props().get("Metadata") or {}
            trackid = meta.get("mpris:trackid")
            micros = int(max(position, 0.0) * 1e6)
            if trackid:
                args, method = GLib.Variant("(ox)", (trackid, micros)), "SetPosition"
            else:
                args, method = GLib.Variant("(x)", (micros - target.elapsed_us,)), "Seek"
        else:
            method = {"play": "Play", "pause": "Pause", "next": "Next", "previous": "Previous",
                      "play_pause": "PlayPause"}.get(name)
            if method is None:
                return False
            args = None
        target.proxy.call(method, args, Gio.DBusCallFlags.NONE, 2000, None, None, None)
        return True
