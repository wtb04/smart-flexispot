import logging
import socket
import subprocess
import sys
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gio, GLib  # noqa: E402

from . import __version__, artwork, link, player, server, settings, volume  # noqa: E402

log = logging.getLogger("desklink")
APP_ID = "nl.w-tb.desklink"
UNIT = "desklink.service"


def machine_name() -> str:
    try:
        for line in Path("/etc/machine-info").read_text().splitlines():
            if line.startswith("PRETTY_HOSTNAME="):
                return line.split("=", 1)[1].strip().strip('"') or socket.gethostname()
    except OSError:
        pass
    return socket.gethostname()


def hyprlock_running() -> bool:
    return subprocess.run(["pidof", "-q", "hyprlock"], check=False).returncode == 0


class App(Adw.Application):
    def __init__(self):
        super().__init__(application_id=APP_ID, flags=Gio.ApplicationFlags.HANDLES_COMMAND_LINE)
        self.link: link.Link | None = None
        self.server_error: str | None = None
        self._started = False
        self._keys = settings.SecretKeyStore()
        self._settings_path = settings.default_path()

    def do_startup(self):
        Adw.Application.do_startup(self)
        self.hold()
        stored = settings.load(self._settings_path)
        self._volume = volume.Volume(lambda: self.link and self.link.volume_changed())
        self._mpris = player.Mpris(lambda track: self.link and self.link.take(track))
        self.link = link.Link(settings=stored, save_settings=lambda s: settings.save(s, self._settings_path),
                              key=self._keys.read(), client=server.PanelClient(), volume=self._volume,
                              covers=artwork.Covers(), player_command=self._mpris.command, clock=link.Clock(),
                              timers=link.GLibTimers(), machine=machine_name(), locked=hyprlock_running)
        self._server = server.Server(self.link.handle)
        self.server_error = self._server.start()
        self._volume.start()
        self._mpris.start()
        self._watch_sleep()
        self.link.add_listener(lambda: log.debug("status: %s", self.link.status_text()))
        self.link.start()

    def _watch_sleep(self):
        system = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        system.signal_subscribe("org.freedesktop.login1", "org.freedesktop.login1.Manager", "PrepareForSleep",
                                "/org/freedesktop/login1", None, Gio.DBusSignalFlags.NONE,
                                lambda *args: self.link.set_sleeping(args[5].unpack()[0]))

    def do_command_line(self, command_line):
        args = command_line.get_arguments()[1:]
        if args[:1] == ["set-key"] and len(args) == 2:
            if not self.link.set_key(args[1]):
                command_line.printerr_literal("the key is 64 hex digits, DESK_LINK_KEY in desk_link_secrets.h\n")
                return 1
            self._keys.write(args[1])
            return 0
        if args == ["--version"]:
            command_line.print_literal(f"Desk Link {__version__}\n")
            return 0
        if args:
            command_line.printerr_literal("usage: desklink [set-key <hex> | --version]\n")
            return 1
        if self._started:
            self.toggle_dropdown()
        self._started = True
        return 0

    def set_key(self, hex_key: str) -> bool:
        if not self.link.set_key(hex_key):
            return False
        self._keys.write(hex_key)
        return True

    def toggle_dropdown(self):
        log.info("dropdown: %s", self.link.status_text())

    def show_settings(self):
        log.info("settings: %s", self.link.status_text())

    @staticmethod
    def autostart_enabled() -> bool:
        return subprocess.run(["systemctl", "--user", "-q", "is-enabled", UNIT], check=False).returncode == 0

    @staticmethod
    def set_autostart(enabled: bool) -> None:
        subprocess.run(["systemctl", "--user", "enable" if enabled else "disable", UNIT], check=False)


def main(argv=None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(name)s: %(message)s")
    return App().run(argv if argv is not None else sys.argv)
