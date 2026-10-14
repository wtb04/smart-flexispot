"""The default output's volume and mute, through wpctl. Polled, since wpctl has
no way to wait for a change and pactl is not installed."""

import re
from typing import Callable

from gi.repository import Gio, GLib

SINK = "@DEFAULT_AUDIO_SINK@"


def parse(text: str) -> tuple[int | None, bool]:
    match = re.search(r"Volume:\s*([0-9.]+)", text)
    if match is None:
        return None, False
    return min(100, round(float(match.group(1)) * 100)), "[MUTED]" in text


def run_async(argv: list[str], done: Callable[[bool, str], None]) -> None:
    try:
        process = Gio.Subprocess.new(argv, Gio.SubprocessFlags.STDOUT_PIPE | Gio.SubprocessFlags.STDERR_SILENCE)
    except GLib.Error:
        done(False, "")
        return

    def finished(proc, result):
        try:
            _, out, _ = proc.communicate_utf8_finish(result)
        except GLib.Error:
            done(False, "")
            return
        done(proc.get_successful(), out or "")

    process.communicate_utf8_async(None, None, finished)


class Volume:
    def __init__(self, on_change: Callable[[], None], run=run_async, poll_s: int = 2):
        self.percent: int | None = None
        self.muted = False
        self._on_change = on_change
        self._run = run
        self._poll_s = poll_s

    def start(self) -> None:
        self.refresh()
        GLib.timeout_add_seconds(self._poll_s, lambda: self.refresh() or True)

    def refresh(self) -> None:
        self._run(["wpctl", "get-volume", SINK], self._got)

    def _got(self, ok: bool, out: str) -> None:
        percent, muted = parse(out) if ok else (None, False)
        if (percent, muted) != (self.percent, self.muted):
            self.percent, self.muted = percent, muted
            self._on_change()

    def set_percent(self, level: int) -> None:
        level = max(0, min(100, int(level)))
        self._run(["wpctl", "set-volume", SINK, f"{level / 100:.2f}"], lambda *_: self.refresh())

    def set_muted(self, muted: bool) -> None:
        self._run(["wpctl", "set-mute", SINK, "1" if muted else "0"], lambda *_: self.refresh())

    def step(self, delta: int) -> bool:
        if self.percent is None:
            return False
        self.set_percent(self.percent + delta)
        return True
