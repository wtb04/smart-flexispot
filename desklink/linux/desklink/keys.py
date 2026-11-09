"""The panel's clicker, as a presenter's clicker sends it: Page Down and Page Up,
which slides in LibreOffice Impress, Google Slides and a PDF viewer all take for
the next slide and the one before. Pressed through wtype, Wayland's virtual
keyboard, or ydotool where the compositor has none."""

import shutil
from typing import Callable

YDOTOOL_PAGE_DOWN, YDOTOOL_PAGE_UP = 109, 104  # the kernel's KEY_PAGEDOWN and KEY_PAGEUP


def argv_for(next_slide: bool, which: Callable[[str], str | None] = shutil.which) -> list[str] | None:
    if which("wtype"):
        return ["wtype", "-k", "Next" if next_slide else "Prior"]
    if which("ydotool"):
        code = YDOTOOL_PAGE_DOWN if next_slide else YDOTOOL_PAGE_UP
        return ["ydotool", "key", f"{code}:1", f"{code}:0"]
    return None


def run_async(argv: list[str], done) -> None:
    from .volume import run_async as run  # GLib's, only once a key is pressed

    run(argv, done)


class SlideKeys:
    def __init__(self, run=run_async, which: Callable[[str], str | None] = shutil.which):
        self._run = run
        self._which = which

    @property
    def available(self) -> bool:
        return argv_for(True, self._which) is not None

    def press(self, next_slide: bool) -> bool:
        argv = argv_for(next_slide, self._which)
        if argv is None:
            return False
        self._run(argv, lambda ok, _out: None)
        return True
