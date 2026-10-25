# Desk Link for Linux

A small app that connects a Linux laptop to the desk panel, as [the Mac's](../macos/) does. What
plays in any MPRIS player shows on the panel with its cover, the panel's buttons pause, seek,
skip and change the volume here, and Claude Code's sessions reach the panel through it.

Its icon in the bar opens what plays, whether the panel answers, and the switch that turns
sharing off and on; the menu behind the dots, or a right click, opens the settings.

## Install

Needs Python 3 with PyGObject, GTK 4, libadwaita, gtk4-layer-shell, libsoup 3 and libsecret,
python3-cryptography, Pillow, `wpctl` and `inotifywait`, all in Ubuntu 26.04's archive.

    cd desklink/linux && make install    # into ~/.local, enabled as a systemd user unit
    desklink set-key <DESK_LINK_KEY>

The key is the panel's `DESK_LINK_KEY` from `components/laptop/include/desk_link_secrets.h`,
kept in the keyring. `desklink` toggles the dropdown and `desklink settings` opens the settings,
so either can go on a key binding.

## In the bar

With a tray host such as Waybar's, the icon is a StatusNotifierItem. Wayle without a tray takes
it as a custom module instead, fed by `desklink-status` in watch mode:

```toml
[[modules.custom]]
id = "desklink"
command = "~/.local/bin/desklink-status"
mode = "watch"
icon-map = { display = "ld-monitor-symbolic", playing = "ld-monitor-play-symbolic", off = "ld-monitor-off-symbolic", trouble = "ld-monitor-x-symbolic" }
left-click = "~/.local/bin/desklink"
right-click = "~/.local/bin/desklink settings"
```

with `custom-desklink` in the layout, and the icons fetched once with
`wayle icons install lucide monitor monitor-play monitor-off monitor-x`. On Hyprland, float the
settings window with a rule on class `nl.w-tb.desklink`.

## Testing

`make test` runs the tests. With `desklink/conformance` built they also check the bytes against
the panel's own code. `tools/fake-panel` stands in for the panel: point the address at
`127.0.0.1:8080` and type its commands.
