"""Which networks this laptop is on, so Desk Link can share on some only: a Wi-Fi
network by its SSID, a wired one by its connection's name. Asked of
NetworkManager, else of iwd, else of `iw`, whichever this distribution has."""

import re
import shutil
import subprocess
from typing import Callable

NM = "org.freedesktop.NetworkManager"
NM_PATH = "/org/freedesktop/NetworkManager"
ACTIVE = "org.freedesktop.NetworkManager.Connection.Active"
ACCESS_POINT = "org.freedesktop.NetworkManager.AccessPoint"
IWD = "net.connman.iwd"
PROPERTIES = "org.freedesktop.DBus.Properties"
WIFI = "802-11-wireless"
WIRED = "802-3-ethernet"
IW_EVERY_S = 5
_IW_SSID = re.compile(r"^\s*ssid (.+)$", re.MULTILINE)


def name_of(kind: str, connection_id: str, ssid: bytes | None) -> str | None:
    if kind == WIFI:
        return ssid.decode("utf-8", "replace") if ssid else connection_id or None
    if kind == WIRED:
        return connection_id or None
    return None  # a bridge, a VPN or the loopback is not where the laptop is


def ssids_in_iw(output: str) -> frozenset[str]:
    """`iw dev` writes a byte outside printable ASCII as \\xNN."""
    names = set()
    for escaped in _IW_SSID.findall(output):
        raw = re.sub(rb"\\x([0-9a-fA-F]{2})", lambda m: bytes([int(m.group(1), 16)]), escaped.encode())
        names.add(raw.decode("utf-8", "replace"))
    return frozenset(names)


def connected_in_iwd(objects: dict) -> frozenset[str]:
    """iwd's GetManagedObjects: each connected Station's network, by its Name."""
    names = set()
    for interfaces in objects.values():
        station = interfaces.get(f"{IWD}.Station", {})
        if station.get("State") == "connected":
            network = objects.get(station.get("ConnectedNetwork", ""), {}).get(f"{IWD}.Network", {})
            if network.get("Name"):
                names.add(network["Name"])
    return frozenset(names)


class Networks:
    """`changed` hears the set of networks as it changes, or None when nothing here can tell."""

    def __init__(self, changed: Callable[[frozenset[str] | None], None], timers, bus=None):
        self._changed = changed
        self._timers = timers
        self._bus = bus
        self.here: frozenset[str] | None = None
        self._timer = None

    def start(self) -> None:
        from gi.repository import Gio

        if self._bus is None:
            try:
                self._bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
            except Exception:  # no system bus, as in some containers
                self._bus = None
        if self._bus is not None and self._owned(NM):
            self._watch(NM, NM_PATH, self._ask_network_manager)
        elif self._bus is not None and self._owned(IWD):
            self._watch(IWD, None, self._ask_iwd)
        elif shutil.which("iw"):
            self._timer = self._timers.every(IW_EVERY_S, lambda: self._tell(self._ask_iw()))
            self._tell(self._ask_iw())
        else:
            self._tell(None)

    def _owned(self, name: str) -> bool:
        reply = self._call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                           ("(s)", (name,)))
        return bool(reply and reply[0])

    def _watch(self, sender: str, path: str | None, ask: Callable[[], frozenset[str] | None]) -> None:
        from gi.repository import Gio

        self._bus.signal_subscribe(sender, PROPERTIES, "PropertiesChanged", path, None, Gio.DBusSignalFlags.NONE,
                                   lambda *_: self._tell(ask()))
        self._tell(ask())

    def _tell(self, here: frozenset[str] | None) -> None:
        if here != self.here:
            self.here = here
            self._changed(here)

    def _call(self, name: str, path: str, interface: str, method: str, args=None):
        from gi.repository import GLib

        try:
            reply = self._bus.call_sync(name, path, interface, method,
                                        GLib.Variant(*args) if args else None, None, 0, 1000, None)
        except GLib.Error:  # the daemon gone, or a connection gone while asked after
            return None
        return reply.unpack()

    def _get(self, path: str, interface: str, name: str):
        reply = self._call(NM, path, PROPERTIES, "Get", ("(ss)", (interface, name)))
        return reply[0] if reply else None

    def _ask_network_manager(self) -> frozenset[str] | None:
        active = self._get(NM_PATH, NM, "ActiveConnections")
        if active is None:
            return None
        names = set()
        for path in active:
            kind = self._get(path, ACTIVE, "Type") or ""
            ssid = None
            if kind == WIFI:
                point = self._get(path, ACTIVE, "SpecificObject")
                ssid = bytes(self._get(point, ACCESS_POINT, "Ssid") or b"") if point and point != "/" else None
            name = name_of(kind, self._get(path, ACTIVE, "Id") or "", ssid)
            if name:
                names.add(name)
        return frozenset(names)

    def _ask_iwd(self) -> frozenset[str] | None:
        reply = self._call(IWD, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects")
        return connected_in_iwd(reply[0]) if reply else None

    @staticmethod
    def _ask_iw() -> frozenset[str] | None:
        try:
            done = subprocess.run(["iw", "dev"], capture_output=True, text=True, timeout=3, check=False)
        except (OSError, subprocess.TimeoutExpired):
            return None
        return ssids_in_iw(done.stdout) if done.returncode == 0 else None
