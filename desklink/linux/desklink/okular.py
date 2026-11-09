"""Which slide the clicker moves, for the panel to show: Okular's page, asked
over D-Bus. Each Okular process is org.kde.okular-<pid>, and each document open
in it, a tab, is /okular, /okular2 and so on there. Several may be open; the
one followed is the one last touched, whose page last changed or that was
last opened."""

import itertools
import os
import re
from dataclasses import dataclass
from typing import Callable

PREFIX = "org.kde.okular-"
INTERFACE = "org.kde.okular"
LOOK_EVERY_S = 0.5
_NODE = re.compile(r'<node name="(okular\d*)"')


@dataclass(frozen=True)
class Slide:
    page: int
    pages: int
    deck: str


def deck_of(document: str) -> str:
    """"/home/w/talk.pdf" and "file:///home/w/talk.pdf" are "talk.pdf"."""
    return os.path.basename(document.rstrip("/"))


def documents_in(introspection: str) -> list[str]:
    return ["/" + node for node in _NODE.findall(introspection)]


class Decks:
    """What each open document said last, and which was touched last."""

    def __init__(self):
        self._seen: dict[tuple[str, str], tuple[Slide, int]] = {}
        self._order = itertools.count()

    def saw(self, where: tuple[str, str], page: int, pages: int, document: str) -> None:
        if pages <= 0 or not 1 <= page <= pages:
            self._seen.pop(where, None)
            return
        slide = Slide(page, pages, deck_of(document))
        before = self._seen.get(where)
        touched = before[1] if before and before[0] == slide else next(self._order)
        self._seen[where] = (slide, touched)

    def keep_only(self, open_now: set[tuple[str, str]]) -> None:
        for where in set(self._seen) - open_now:
            del self._seen[where]

    def chosen(self) -> tuple[tuple[str, str], Slide] | None:
        if not self._seen:
            return None
        where, (slide, _) = max(self._seen.items(), key=lambda item: item[1][1])
        return where, slide


class Okular:
    """Looks at every Okular every half second; `changed` hears the followed slide as it changes."""

    def __init__(self, changed: Callable[[Slide | None], None], timers, bus=None):
        self._changed = changed
        self._timers = timers
        self._decks = Decks()
        self._told: Slide | None = None
        self._bus = bus
        self._timer = None

    @property
    def slide(self) -> Slide | None:
        chosen = self._decks.chosen()
        return chosen[1] if chosen else None

    def start(self) -> None:
        if self._bus is None:
            from gi.repository import Gio

            self._bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self._timer = self._timers.every(LOOK_EVERY_S, self._look)

    def turn(self, next_slide: bool) -> bool:
        """The followed document's next or previous page, through Okular itself."""
        chosen = self._decks.chosen()
        if self._bus is None or chosen is None:
            return False
        (name, path), _ = chosen
        self._call(name, path, INTERFACE, "slotNextPage" if next_slide else "slotPreviousPage", None, lambda _v: None)
        return True

    def show(self) -> bool:
        """The followed document's presentation in or out; Okular does not say which it is in."""
        chosen = self._decks.chosen()
        if self._bus is None or chosen is None:
            return False
        (name, path), _ = chosen
        self._call(name, path, INTERFACE, "slotTogglePresentation", None, lambda _v: None)
        return True

    def _call(self, name, path, interface, method, args, done) -> None:
        from gi.repository import Gio, GLib

        def finished(bus, result):
            try:
                value = bus.call_finish(result)
            except GLib.Error:
                value = None
            done(value)

        self._bus.call(name, path, interface, method, args, None, Gio.DBusCallFlags.NO_AUTO_START, 1000, None,
                       finished)

    def _look(self) -> None:
        self._call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames", None,
                   self._names)

    def _names(self, value) -> None:
        if value is None:
            return
        names = [n for n in value.unpack()[0] if n.startswith(PREFIX)]
        pending = {"left": len(names)}
        found: set[tuple[str, str]] = set()
        if not names:
            self._settle(found)
            return
        for name in names:
            self._call(name, "/", "org.freedesktop.DBus.Introspectable", "Introspect", None,
                       lambda v, name=name: self._documents(name, v, found, pending))

    def _documents(self, name, value, found, pending) -> None:
        paths = documents_in(value.unpack()[0]) if value is not None else []
        pending["left"] += len(paths) - 1
        for path in paths:
            found.add((name, path))
            self._read(name, path, found, pending)
        if pending["left"] == 0:
            self._settle(found)

    def _read(self, name, path, found, pending) -> None:
        answers = {}

        def answer(method, value):
            answers[method] = value.unpack()[0] if value is not None else None
            if len(answers) < 3:
                return
            page, pages, document = answers["currentPage"], answers["pages"], answers["currentDocument"]
            if isinstance(page, int) and isinstance(pages, int) and isinstance(document, str):
                self._decks.saw((name, path), page, pages, document)
            pending["left"] -= 1
            if pending["left"] == 0:
                self._settle(found)

        for method in ("currentPage", "pages", "currentDocument"):
            self._call(name, path, INTERFACE, method, None, lambda v, m=method: answer(m, v))

    def _settle(self, found: set[tuple[str, str]]) -> None:
        self._decks.keep_only(found)
        slide = self.slide
        if slide != self._told:
            self._told = slide
            self._changed(slide)
