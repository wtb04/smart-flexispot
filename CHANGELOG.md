# Changelog

The versions are the points where the panel was a step further on my desk. None of them is a stable release, it is still a project in progress.

## Unreleased

- Screenshots at 9:41, retaken with `sim/shots.sh`
- Board pictures on a background, from five angles, with `pcb/render.sh`
- Photo credits show accents (César, not C\u00e9sar)
- The simulator checks routes as the panel does
- Cinema: ten seconds back and on as buttons, a swipe across the picture for the episode before or after
- Between episodes, the cover and the still stay until the next one comes

## v0.9.0, 4 October 2026

First public release.

- Calendar: the next event first, with its start time and room in large type
- Radar: plane details as a boarding pass, and better routes for low-cost and multi-leg flights
- Personal settings moved out of the code into your own config files
- Time zone setting, longer update keys, and the companion builds again
- Apache 2.0 licence

## v0.8.0, 3 October 2026

One frame for every page, a quicker panel, and a radar that follows what is worth seeing.

- A dock at the side, right by default and left in Setup: the desk's height, which folds the rest of the desk out beside it, one button for Stand and Sit, Home in its middle, and from its foot the focus timer, Radar and Calendar.
- A control bar along the top: what plays, with play and pause and a small music view under it; the heating, which drops the thermostat; the lights, all at once or each when held; and the status, which opens Setup and shows an update arriving.
- Cards that unfold from a corner and draw only what is newly shown, at the panel's full frame rate, and fold away by themselves.
- Home is the air, the radar as its page had it, and what comes next; the Radar page is the map to its every edge.
- Stand and Sit as one button, the desk it goes to drawn on it: Sit from Stand, Stand from Sit, and Sit from anywhere else, showing which way while the desk moves.
- The desk's presets act as the control box's own keys: one tapped while the desk moves stops it, and a start just after a stop waits until the box will take it.
- The panel shows it has been unplugged or plugged in within two seconds.
- What the player playing would not act on is left out rather than faded: a Jellyfin player that is only followed has no play or skip, and the speaker shows only what the app on it takes, from its supported features in Home Assistant.
- Claude Code on the laptops: a pill beside the lights while a session works, a ring of one piece per session with their count inside it, amber with a chime and a notice while one waits on you, and gone ten minutes after the last event. Its card has each session's project and laptop, what it does, its steps and what it did last. The laptops post to `/claude` from Claude Code's hooks with `tools/claude-hook`, names only, under a key of their own.
- Faster to use: a page opens as the finger lands rather than as it lifts, an update arriving no longer leaves the screen a frame a second, and the connections to Home Assistant, Jellyfin and MQTT work beside the screen rather than on its core.
- The radar follows the most notable aircraft in its view rather than the nearest: an emergency first, then military aircraft (a military helicopter only as a 777, as it hangs about the one place), then the giants as the A380 and 747, the large wide-bodies, the other wide-bodies, and what cruises high, the nearer of two alike. On Home and the Radar page, it goes back to following a minute after the screen was last touched. A crosshair in the radar's corner shows it following, lit; with an aircraft chosen by hand it fades and a ring round it fills over the minute, and a tap on it follows again at once. Every three minutes it moves on to the next of the three most notable in view, the crosshair's ring filling in the accent until it does, and turns to an emergency at once. A long press on the crosshair holds what is on show until it is tapped again. Its zoom is kept across a restart. An aircraft only one of the two feeds has, as one placed by MLAT, is kept through the other feed's turn rather than blinking out every other reading, and what one feed leaves out of an aircraft is taken from the other.
- `/bench?pages` on a development build: each page switched to, what comes before its frame and what the frame costs.
- `/desk` on a development build: what the desk heard and was asked, and a preset tapped from afar.

## v0.7.0, 30 September 2026

Tested and built in CI.

- Host tests under GoogleTest and CTest, from one CMake project with presets, with coverage from gcovr.
- CI on every push: the host tests with coverage in the summary, both firmware builds in Espressif's ESP-IDF 5.5.5 image, and the simulator's screenshots on Linux.
- Each firmware build keeps a whole-flash image to write at 0x0 and the image for an update. A version tag puts them on a GitHub release.
- The simulator builds without ESP-IDF, runs offline and on Linux, and renders the main views to screenshots.
- A clean checkout builds at the first go.
- The README tells what it is and why, with a tour, wiring diagrams and the companion board. The insides are in `docs/how-it-works.md`.

## v0.6.0, 30 September 2026

One network, shared workers, and knowing when something went wrong.

- `net`: every HTTP request goes through one scheduler with priorities, merging, retries and rest for a failing host, and waits while there is no network.
- The live connections, Home Assistant's websocket and MQTT and Jellyfin's websocket, are kept by `net` too, with back-off and keep-alive.
- Periodic work runs as jobs on two shared workers instead of a task each, which freed most of the internal RAM the radio and TLS need.
- Settings are records each component keeps for itself, written behind.
- Fullscreen views and popups open, close and stack in one place.
- A watchdog on the shared workers, the last crash kept whole in a core dump partition, and a notice when an update was rolled back.
- The Wi-Fi co-processor is power cycled at boot, and a whole-chip restart from cold when it stays out of reach.
- Faster startup without a flash of blue: the splash is lit first and the pages are built behind it while the radio joins.
- The display DMA runs round a ring of frames by itself, so a busy moment no longer costs a frame.
- A chosen plane's trail grows back from it along where it flew.
- The simulator's feeds go through the same `net` core, and it can only reach the read-only feeds.

## v0.5.0, 28 September 2026

A simulator, and a panel that lasts on its battery.

- The simulator: the panel's own UI in a window on the Mac, with Home Assistant, the media, notices and updates played by stubs.
- Home Assistant notices shown as cards, kept up to ten minutes or until tapped.
- The battery's charge counted from its current, with its current and power as sensors in Home Assistant.
- Power saving while the screen is dark.
- The radar fullscreen, with its map and planes to every edge, zoom in even steps, trails from before the panel saw a plane, and two feeds taken in turn.
- The panel's last log lines kept across a crash.
- A softer chime.

## v0.4.0, 27 September 2026

Films and series.

- Jellyfin followed live over its own socket, with Skip intro and Next episode.
- Cinema mode.
- Spotify favourites from the media card.
- The focus timer keeps its place across a restart, opens fullscreen, and counts down on its tab.
- The chosen plane's trail on the radar.
- A development build that can be looked at over Wi-Fi.
- Icons drawn as pictures: the lamps, the desk, the phone.

## v0.3.0, 25 September 2026

Taken apart into components, and updated over the air.

- The desk in a component of its own with one driver task, so nothing queues behind a stop.
- Whichever board holds the wire travels to presets 5 and 6.
- The screen split into a file per part, asking for things instead of fetching them itself.
- One log for the whole panel, filterable, and diagnostics described as data.
- The focus timer, 25 minutes on and 5 off.
- The screen turned by the IMU.
- Both boards updated over the air, the companion through the panel, with an update kept ready until it is asked for.
- Faded rather than grey for what cannot be used.

## v0.2.0, 23 September 2026

The day, and the companion board.

- The timetable from the calendar feeds, with the next thing up top.
- When to leave, with two ways there and every leg of each.
- A design language for the pages, and real icons.
- A map under the radar's scope.
- The screen can be turned off, and woken by a tap.
- LVGL and TLS kept out of internal memory.
- Jellyfin on the media card when the speaker is quiet.
- Presets 5 and 6.
- The companion's carrier board.

## v0.1.0, 21 September 2026

The first panel on the desk.

- The LoctekMotion desk driven over the M5-Bus UART, or over a Bluetooth companion instead of a wire.
- The battery, the charger, Wi-Fi, the clock and the Home Assistant bridge.
- The home page on Home Assistant's websocket.
- Presence from the phone over Bluetooth.
- What the speaker is playing, with cover, progress and volume.
- A startup screen, and Setup as diagnostics and settings.
- A flight radar with lookups and photos.
