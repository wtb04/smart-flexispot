"""The tray icon, as a StatusNotifierItem Wayle shows. Left click is the
dropdown; right click opens settings, which hosts call ContextMenu for when an
item has no D-Bus menu."""

import json
import os
from pathlib import Path
from typing import Callable

from gi.repository import Gio, GLib

WATCHER = "org.kde.StatusNotifierWatcher"
PATH = "/StatusNotifierItem"
XML = """
<node><interface name="org.kde.StatusNotifierItem">
  <property name="Category" type="s" access="read"/>
  <property name="Id" type="s" access="read"/>
  <property name="Title" type="s" access="read"/>
  <property name="Status" type="s" access="read"/>
  <property name="IconName" type="s" access="read"/>
  <property name="IconThemePath" type="s" access="read"/>
  <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
  <property name="ItemIsMenu" type="b" access="read"/>
  <property name="Menu" type="o" access="read"/>
  <method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  <method name="SecondaryActivate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  <method name="ContextMenu"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
  <method name="Scroll"><arg type="i" direction="in"/><arg type="s" direction="in"/></method>
  <signal name="NewIcon"/><signal name="NewToolTip"/><signal name="NewStatus"><arg type="s"/></signal>
</interface></node>"""


def icon_for(health: str, playing: bool) -> str:
    if health == "off":
        return "desklink-off-symbolic"
    if health == "trouble":
        return "desklink-trouble-symbolic"
    return "desklink-playing-symbolic" if playing else "desklink-display-symbolic"


def bar_status(health: str, playing: bool, text: str) -> dict:
    """What a Wayle custom module in watch mode shows, its icon picked by `alt`."""
    if health in ("off", "trouble"):
        alt = health
    else:
        alt = "playing" if playing else "display"
    return {"alt": alt, "text": "", "tooltip": f"Desk Link: {text}"}


def bar_status_path() -> Path:
    return Path(os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}") / "desklink" / "status.json"


def write_bar_status(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(payload) + "\n")
    temporary.replace(path)


class Tray:
    def __init__(self, link, on_activate: Callable[[], None], on_menu: Callable[[], None]):
        self._link = link
        self._on_activate = on_activate
        self._on_menu = on_menu
        self._bus = None
        self._name = f"org.kde.StatusNotifierItem-{os.getpid()}-1"
        self._icon = ""

    def start(self) -> None:
        self._bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        info = Gio.DBusNodeInfo.new_for_xml(XML).interfaces[0]
        self._bus.register_object(PATH, info, self._call, self._get, None)
        Gio.bus_own_name_on_connection(self._bus, self._name, Gio.BusNameOwnerFlags.NONE, None, None)
        # Wayle's restart brings a new watcher, which knows no items until told again.
        Gio.bus_watch_name_on_connection(self._bus, WATCHER, Gio.BusNameWatcherFlags.NONE,
                                         lambda *_: self._register(), None)
        self._link.add_listener(self._changed)
        self._icon = self._current_icon()

    def _register(self) -> None:
        self._bus.call(WATCHER, "/StatusNotifierWatcher", WATCHER, "RegisterStatusNotifierItem",
                       GLib.Variant("(s)", (self._name,)), None, Gio.DBusCallFlags.NONE, -1, None, None, None)

    def _current_icon(self) -> str:
        track = self._link.track
        return icon_for(self._link.health(), bool(track and track.playing))

    def _changed(self) -> None:
        icon = self._current_icon()
        if icon != self._icon:
            self._icon = icon
            self._bus.emit_signal(None, PATH, "org.kde.StatusNotifierItem", "NewIcon", None)
        self._bus.emit_signal(None, PATH, "org.kde.StatusNotifierItem", "NewToolTip", None)

    def _get(self, _bus, _sender, _path, _iface, name):
        values = {
            "Category": GLib.Variant("s", "ApplicationStatus"),
            "Id": GLib.Variant("s", "desklink"),
            "Title": GLib.Variant("s", "Desk Link"),
            "Status": GLib.Variant("s", "Active"),
            "IconName": GLib.Variant("s", self._icon or self._current_icon()),
            "IconThemePath": GLib.Variant("s", os.path.expanduser("~/.local/share/icons")),
            "ToolTip": GLib.Variant("(sa(iiay)ss)", ("", [], "Desk Link", self._link.status_text())),
            "ItemIsMenu": GLib.Variant("b", False),
            "Menu": GLib.Variant("o", "/NO_DBUSMENU"),
        }
        return values.get(name)

    def _call(self, _bus, _sender, _path, _iface, method, _params, invocation):
        if method == "Activate":
            GLib.idle_add(lambda: self._on_activate())
        elif method == "ContextMenu":
            GLib.idle_add(lambda: self._on_menu())
        elif method == "SecondaryActivate":
            GLib.idle_add(lambda: self._link.play_pause())
        invocation.return_value(None)
