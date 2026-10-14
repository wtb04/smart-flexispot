"""The dropdown under the bar and the settings window, after the Mac's
desklink/macos/shots/menu-dark.png and settings-dark.png."""

import json
import subprocess
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
gi.require_version("Gtk4LayerShell", "1.0")
from gi.repository import Adw, Gdk, GLib, Gtk  # noqa: E402
from gi.repository import Gtk4LayerShell as Layer  # noqa: E402

CSS = """
.desklink-card { background: alpha(#131419, 0.97); border: 2px solid #e0821f; border-radius: 18px; padding: 14px; }
.desklink-now { background: alpha(#ffffff, 0.04); border-radius: 12px; padding: 10px; }
.desklink-cover { border-radius: 8px; }
progressbar.desklink-progress trough { min-height: 6px; border-radius: 3px; background: alpha(#ffffff, 0.12); }
progressbar.desklink-progress progress { min-height: 6px; border-radius: 3px; }
.desklink-title { font-weight: 700; }
.desklink-dot-good { color: #9ece6a; }
.desklink-dot-checking { color: #e0af68; }
.desklink-dot-trouble { color: #e0821f; }
.desklink-dot-off { color: #565f89; }
progressbar.desklink-progress progress { background: #e0821f; }
"""


def status_line(link, server_error: str | None) -> str:
    return server_error or link.status_text()


def subtitle(track) -> str:
    return ", ".join(part for part in (track.artist, track.identity) if part)


def _focused_monitor():
    try:
        out = subprocess.run(["hyprctl", "-j", "monitors"], capture_output=True, text=True, check=True).stdout
        name = next(m["name"] for m in json.loads(out) if m.get("focused"))
    except (OSError, subprocess.CalledProcessError, ValueError, StopIteration):
        return None
    monitors = Gdk.Display.get_default().get_monitors()
    for i in range(monitors.get_n_items()):
        if monitors.get_item(i).get_connector() == name:
            return monitors.get_item(i)
    return None


_styled = False


def _style():
    global _styled
    if _styled:
        return
    _styled = True
    # the desktop is dark but sets no colour-scheme preference GTK can read
    Adw.StyleManager.get_default().set_color_scheme(Adw.ColorScheme.FORCE_DARK)
    provider = Gtk.CssProvider()
    provider.load_from_string(CSS)
    Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), provider,
                                              Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)


class Dropdown:
    def __init__(self, app):
        self._app = app
        self._link = app.link
        _style()
        self.window = Gtk.Window(application=app)
        Layer.init_for_window(self.window)
        Layer.set_namespace(self.window, "desklink")
        Layer.set_layer(self.window, Layer.Layer.TOP)
        Layer.set_anchor(self.window, Layer.Edge.TOP, True)
        Layer.set_anchor(self.window, Layer.Edge.RIGHT, True)
        Layer.set_margin(self.window, Layer.Edge.TOP, 6)
        Layer.set_margin(self.window, Layer.Edge.RIGHT, 12)
        Layer.set_keyboard_mode(self.window, Layer.KeyboardMode.ON_DEMAND)
        self.window.set_default_size(400, -1)

        card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, css_classes=["desklink-card"])
        self._now = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10, css_classes=["desklink-now"])
        row = Gtk.Box(spacing=12)
        self._cover = Gtk.Image(css_classes=["desklink-cover"], pixel_size=56, valign=Gtk.Align.CENTER)
        text = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, valign=Gtk.Align.CENTER, hexpand=True)
        self._title = Gtk.Label(xalign=0, ellipsize=3, css_classes=["desklink-title"])
        self._subtitle = Gtk.Label(xalign=0, ellipsize=3, css_classes=["dim-label"])
        text.append(self._title)
        text.append(self._subtitle)
        self._play = Gtk.Button(icon_name="media-playback-start-symbolic", valign=Gtk.Align.CENTER,
                                css_classes=["circular", "suggested-action"])
        self._play.connect("clicked", lambda *_: self._link.play_pause())
        for child in (self._cover, text, self._play):
            row.append(child)
        self._progress = Gtk.ProgressBar(css_classes=["desklink-progress"])
        self._now.append(row)
        self._now.append(self._progress)
        self._nothing = Gtk.Label(label="Nothing plays", css_classes=["dim-label"], margin_top=6, margin_bottom=6)

        status = Gtk.Box(spacing=8)
        self._dot = Gtk.Label(label="●")
        self._status = Gtk.Label(xalign=0, hexpand=True, ellipsize=3)
        self._sharing = Gtk.Switch(valign=Gtk.Align.CENTER)
        self._sharing.connect("notify::active", lambda s, _p: self._link.set_sharing(s.get_active()))
        more = Gtk.Button(icon_name="view-more-symbolic", css_classes=["flat", "circular"])
        more.connect("clicked", lambda *_: (self.hide(), self._app.show_settings()))
        for child in (self._dot, self._status, self._sharing, more):
            status.append(child)

        for child in (self._now, self._nothing, status):
            card.append(child)
        self.window.set_child(card)

        keys = Gtk.EventControllerKey()
        keys.connect("key-pressed", lambda _c, key, *_: key == Gdk.KEY_Escape and (self.hide() or True))
        self.window.add_controller(keys)
        self.window.connect("notify::is-active", lambda w, _p: not w.is_active() and self.hide())
        self._link.add_listener(self.refresh)
        self._ticker = 0
        self._cover_version = None

    def toggle(self) -> None:
        if self.window.get_visible():
            self.hide()
            return
        monitor = _focused_monitor()
        if monitor is not None:
            Layer.set_monitor(self.window, monitor)
        self.refresh()
        self.window.present()
        self._ticker = GLib.timeout_add(1000, lambda: self._tick() or True)

    def hide(self) -> None:
        self.window.set_visible(False)
        if self._ticker:
            GLib.source_remove(self._ticker)
            self._ticker = 0

    def _tick(self) -> None:
        track = self._link.track
        if track and track.duration > 0:
            self._progress.set_fraction(min(1.0, track.position(time.monotonic()) / track.duration))

    def refresh(self) -> None:
        link = self._link
        track = link.track
        self._now.set_visible(track is not None)
        self._nothing.set_visible(track is None)
        if track is not None:
            self._title.set_label(track.title)
            self._subtitle.set_label(subtitle(track))
            self._play.set_icon_name("media-playback-pause-symbolic" if track.playing
                                     else "media-playback-start-symbolic")
            self._progress.set_visible(track.duration > 0)
            self._tick()
        version = link.artwork.version if link.artwork else None
        if version != self._cover_version:
            self._cover_version = version
            texture = Gdk.Texture.new_from_bytes(GLib.Bytes.new(link.artwork.jpeg)) if link.artwork else None
            self._cover.set_from_paintable(texture)
            self._cover.set_visible(texture is not None)
        health = "trouble" if self._app.server_error else link.health()
        self._dot.set_css_classes([f"desklink-dot-{health}"])
        self._status.set_label(status_line(link, self._app.server_error))
        if self._sharing.get_active() != link.settings.sharing:
            self._sharing.set_active(link.settings.sharing)


class SettingsWindow:
    def __init__(self, app):
        self._app = app
        link = app.link
        _style()
        self.window = Adw.Window(application=app, title="Desk Link", default_width=460, default_height=640,
                                 hide_on_close=True)
        page = Adw.PreferencesPage()

        head = Adw.PreferencesGroup()
        card = Gtk.Box(spacing=16, margin_top=6, margin_bottom=6)
        card.append(Gtk.Image(icon_name="nl.w-tb.desklink", pixel_size=64))
        words = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, valign=Gtk.Align.CENTER)
        words.append(Gtk.Label(label="Desk Link", xalign=0, css_classes=["title-3"]))
        self._status = Gtk.Label(xalign=0)
        self._firmware = Gtk.Label(xalign=0, css_classes=["dim-label", "caption"])
        words.append(self._status)
        words.append(self._firmware)
        card.append(words)
        head.add(card)
        page.add(head)

        panel = Adw.PreferencesGroup(title="Panel")
        self._address = Adw.EntryRow(title="Address", show_apply_button=True, text=link.settings.panel)
        self._address.connect("apply", lambda row: link.set_panel(row.get_text()))
        self._key = Adw.PasswordEntryRow(title="Key", show_apply_button=True)
        self._key.connect("apply", self._apply_key)
        panel.add(self._address)
        panel.add(self._key)
        page.add(panel)

        share = Adw.PreferencesGroup(title="Share")
        plays = Adw.SwitchRow(title="What plays on this laptop", active=link.settings.sharing)
        plays.connect("notify::active", lambda row, _p: link.set_sharing(row.get_active()))
        claude = Adw.SwitchRow(title="Claude Code sessions", active=link.settings.claude)
        claude.connect("notify::active", lambda row, _p: link.set_claude(row.get_active()))
        share.add(plays)
        share.add(claude)
        page.add(share)
        self._plays = plays

        login = Adw.PreferencesGroup()
        start = Adw.SwitchRow(title="Open at login", active=app.autostart_enabled())
        start.connect("notify::active", lambda row, _p: app.set_autostart(row.get_active()))
        login.add(start)
        page.add(login)

        end = Adw.PreferencesGroup()
        quit_button = Gtk.Button(label="Quit Desk Link", halign=Gtk.Align.END)
        quit_button.connect("clicked", lambda *_: app.quit())
        end.add(quit_button)
        page.add(end)

        view = Adw.ToolbarView(content=page)
        view.add_top_bar(Adw.HeaderBar())
        self.window.set_content(view)
        link.add_listener(self.refresh)
        self.refresh()

    def _apply_key(self, row) -> None:
        if self._app.set_key(row.get_text().strip()):
            row.remove_css_class("error")
            row.set_text("")
        else:
            row.add_css_class("error")

    def refresh(self) -> None:
        link = self._app.link
        self._status.set_label(status_line(link, self._app.server_error))
        self._firmware.set_label(f"Panel firmware {link.firmware}" if link.firmware else "")
        self._key.set_title("Key" if link.has_key() else "Key, 64 hex digits")
        if self._plays.get_active() != link.settings.sharing:
            self._plays.set_active(link.settings.sharing)

    def present(self) -> None:
        self.refresh()
        self.window.present()
