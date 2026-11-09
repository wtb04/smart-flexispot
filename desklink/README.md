# Desk Link

Desk Link connects a laptop to the desk panel. What plays on the laptop shows on the panel with
its cover, the panel's buttons control it, and the laptop's Claude Code sessions reach the panel.
The panel is also a presentation clicker for it: tap the laptop's mark in the panel's bar, and its
Next and Back press Page Down and Page Up on the laptop, which slides in Keynote, PowerPoint,
Google Slides and a PDF viewer all take. In Okular, and in Preview on the Mac, the panel also shows
which page of how many and the file's name, for the document last touched: on the Mac read through
Accessibility, from Preview's title or Okular's toolbar, on Linux asked of Okular over D-Bus. There
the clicker also puts the file in and out of its full screen, and opens only while one is open.
The desk goes to Stand or Sit from the laptop too.

- [macOS](macos/), a menu bar app
- [Linux](linux/), for Wayland desktops, in a tray or a Wayle bar

Both speak the protocol in the panel's `components/laptop/include/link_envelope.h` and
`laptop_protocol.h`: every message sealed with AES-256-GCM under the panel's `DESK_LINK_KEY`,
state to the panel's `/link`, commands and cover requests back to port 47801 on the laptop.
[conformance/](conformance/) holds fixtures made with the panel's own code that each version is
tested against.

Several laptops can share at once. The panel follows whichever plays, and when several do, the
one that began playing last; when none plays it stays with the one it had. The others' Claude
Code sessions show all the same.
