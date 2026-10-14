# Desk Link

Desk Link connects a laptop to the desk panel. What plays on the laptop shows on the panel with
its cover, the panel's buttons control it, and the laptop's Claude Code sessions reach the panel.

- [macOS](macos/), a menu bar app
- [Linux](linux/), for Wayland desktops, in a tray or a Wayle bar

Both speak the protocol in the panel's `components/laptop/include/link_envelope.h` and
`laptop_protocol.h`: every message sealed with AES-256-GCM under the panel's `DESK_LINK_KEY`,
state to the panel's `/link`, commands and cover requests back to port 47801 on the laptop.
[conformance/](conformance/) holds fixtures made with the panel's own code that each version is
tested against.
