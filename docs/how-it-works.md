# How it works

The inside of the panel, and how to work on it. The [README](../README.md) has
what it does and how to set one up.

## Working on it

### The simulator

```sh
sim/run.sh
```

opens the panel's UI in a window, built from the same `components/ui` with the
hardware and Home Assistant played by stubs you drive from the keyboard. The
calendar, journey and radar are fetched for real, through the same scheduling
core the panel uses, and only those: anything else is refused before it leaves
the Mac. [`sim/README.md`](../sim/README.md) has the keys and the options.

```sh
cmake --build sim/build --target screenshots
```

renders the main views, offline and the same each time, to
`sim/build/screenshots/`.

### Tests

The parts that decide and parse — the request scheduler, the stream and job
schedulers, the watchdog, the desk and companion protocols, the Home Assistant
and Jellyfin protocols, the calendar, journey and radar parsers, the battery
gauge, the focus timer — are plain C++ with no ESP-IDF in them, and are tested
on the host under GoogleTest:

```sh
cd test
cmake --workflow --preset host        # build and run them
cmake --workflow --preset coverage    # the same, measured; the report in build_tests/coverage/coverage/
```

The coverage workflow needs `gcovr` (`pipx install gcovr`). The tests live next
to what they test, in `components/<name>/test/`; `test/CMakeLists.txt` lists
them.

### CI

Every push runs [three jobs](../.github/workflows/ci.yml): the host tests with
coverage, whose table ends up in the run's summary; the firmware, both the
normal build and the development one, in Espressif's image for ESP-IDF 5.5.5,
from `sdkconfig.defaults` alone and the secrets' templates; and the simulator
on Linux, whose screenshots are kept with the run. Each firmware build keeps
its images: `smart_flexispot-full.bin` for the whole flash at 0x0, and
`smart_flexispot.bin` for an update. A tag such as `v1.2.0` also puts the
release build's two on a GitHub release.

### The development build

```sh
idf.py -B build_remote -DCMAKE_CXX_FLAGS="-DREMOTE_ENABLED=1" build
```

adds pages to the panel's web server, all asking for the update key.
`tools/remote.py` reads the first few.

| Page | What it gives |
| --- | --- |
| `/log` | The recent log |
| `/screen` | A picture of the screen |
| `/panel` | The frame the panel itself shows, as its buffer holds it |
| `/heap`, `/power` | Memory, and the pack's draw on battery |
| `/streams` | Each live connection and how it is doing; `?restart=N` begins one again |
| `/jobs` | Each shared worker's jobs; `?poke=N` runs one now |
| `/restart` | Why this run started, and what the last one left behind |
| `/coredump` | The last crash, whole, for `idf.py coredump-info` with the build's ELF |
| `/bench` | The radar's map timed: whole frames, a tile, a trail growing, a zoom; `?rotate` the PPA, `?full` and `?back` leave it open and put it back; `?sheet` the desk's fold-out coming out, frame by frame |
| `/desk` | What the desk heard and was asked lately, a line each with its milliseconds since boot; `?tap=N` taps preset N as a finger on its button would, `?clear` starts the trace over |
| `/stall`, `/crash` | A held-off interrupt, a crash on purpose |

## How it is put together

`app_main` brings up the board, puts up the splash, builds the rest of the UI
behind it while the radio joins, starts the desk link and the network clients,
and returns; everything after that runs on tasks of its own.

| Path | Purpose |
| --- | --- |
| `main/` | `app_main` and the glue: handlers, what goes to Home Assistant, diagnostics, the clock, the development pages |
| `components/board/` | Power rails, panel, touch, the LVGL port, the backlight |
| `components/esp_lcd/` | ESP-IDF's panel driver with the display DMA running round a ring of frames |
| `components/ui/` | The screen: `ui.h` is its interface, one file per part, every update applied on the LVGL task |
| `components/net/` | Every request the panel makes, and the live connections |
| `components/jobs/` | The shared workers periodic work runs on |
| `components/app_state/` | What the whole panel goes by: the screen lit, the network up |
| `components/watchdog/` | Notices a shared worker stuck on one thing |
| `components/settings/` | Everything kept across a restart |
| `components/desk/`, `loctek/`, `deskproto/` | The desk, the control box's frames, the panel-to-companion protocol |
| `components/hass/`, `room/` | Home Assistant, and the home page's presenter |
| `components/jellyfin/`, `media/`, `jpeg/` | Jellyfin, cover art, the JPEG engine and its software fallback |
| `components/radar/`, `ical/`, `travel/` | Planes overhead, calendars, journeys |
| `components/ble/`, `wifi/`, `ota/` | Presence and the companion's link, Wi-Fi, updates |
| `components/power/`, `rtc/`, `sound/`, `logbuf/`, `focus/`, `imu/` | Battery, backup clock, chime, the log ring, the focus timer, the accelerometer |
| `components/esp_hosted/` | The Wi-Fi co-processor's driver, with one change to its receive buffers |
| `proxy/` | The companion's firmware |
| `sim/`, `test/` | The simulator and the host tests |

**The network.** Every HTTP request goes through `net`: it says what it is for
(something waited on at the screen, something due, or something wanted
before it is asked for), and `net` decides when it goes, on a connection
kept for its host. It merges or replaces requests with the same key, retries
after a delay, rests a host that is failing or says it is asked too much, and
waits while there is no network. The live connections, Home Assistant's
websocket and MQTT and Jellyfin's websocket, are kept by `net` too: connected
when the network is, begun again with a back-off when they drop, and let go
when they open and never get going.

**Periodic work** runs as jobs on two shared workers rather than a task each:
one for the quick ones (the clock, orientation, the battery, the network
publish, the radar's planning), one for those that wait on a fetch (the
calendar, the journey, the favourites). The stacks that saved were most of the
internal RAM the radio and TLS need.

**When things go wrong.** A shared worker stuck on one thing past its limit
is named in the log and restarts the panel. A crash leaves its last log lines
in memory the restart keeps, and the whole crash in the core dump partition,
both in the next run's `/restart`. An update that does not reach the network
in time is rolled back. The Wi-Fi co-processor is switched off and on at every
boot, and if it stays out of reach the panel restarts from cold, at most twice
in a row.

**The display** is fed by DMA from PSRAM, and the panel shows flat blue for
any frame it goes without. The DMA runs round a ring of eight frames by itself
(`components/esp_lcd`), so an interrupt held off for a while no longer costs a
frame, and the panel's interrupts run through flash writes. The backlight is
held dark through a restart, and lit only once there is a drawn frame to show.

PSRAM's bandwidth, which the panel's own reading takes a good share of, is
what a frame costs: a whole screen is about 88 ms, of which turning it onto
the portrait panel with the PPA is 42. So as little as possible is drawn
again. There are three frame buffers, so a frame is drawn while the panel is
still taking up the last, which it does a frame or two after it is asked to;
each buffer is brought up to date only where it is behind, by the CPU, which
copies PSRAM faster than the PPA. The radar's map keeps track of which
32 px tiles of its picture the planes and the trail were drawn into, puts the
map back only there, and draws again only the tiles whose pixels changed; a
trail growing redraws only where it runs.

Anything calling `lv_*` from its own task must sit between `lvgl_port_lock()`
and `lvgl_port_unlock()`; LVGL's own callbacks already hold it. Updates from
other tasks go through `ui.h`, which only copies them for the LVGL task to
draw.

### The desk, in detail

**The desk moves one short step per frame it receives**, so holding a button
means retransmitting: the driver task repeats the key frame every
`CONFIG_LOCTEK_REPEAT_MS`. Releasing sends the "no keys pressed" frame rather
than merely going quiet, which stops the desk promptly instead of letting it
coast. While idle, that same frame doubles as a keep-awake poll — the control
box only reports its height in reply to something, and its panel sleeps after
about ten seconds.

**One driver task writes every key frame.** Callers post what they want into
mailboxes that hold only the latest wish, so a Stop can never queue behind
anything; the receive task only decodes and hands heights on. Any key whose
height stops changing for 2.5 s is released, whatever asked for it, and a
travel that sees the desk move away from its target gives up. A move asked
for over the network is let go of after 1.5 s unless it is asked for again,
since nobody's finger is on it.

**Presets**: a tap sends the desk to one of the box's four, and the box runs
the move itself and ignores a plain stop while it does, so tapping the same
preset again is what cancels it. A long press stores the current height there,
as the M key followed by the preset key. Presets 5 and 6 are the panel's own: a
tap has the desk driven there by whichever board holds the wire, with one
steering decision per height the box reports rather than on a clock. The desk
runs on after the last key frame, so the keys are let go early by a run-on
learnt from where each travel comes to rest, per direction, kept in flash.

A key press is a stream of frames for as long as a finger is down, not a single
frame; sending a preset once registers only sometimes. Presses repeat for
`CONFIG_LOCTEK_PRESS_MS` and end with an explicit release, holding the transmit
lock throughout so the idle poll cannot land in the middle of one and read as an
early release.

> **Five seconds of the M key puts the control box into factory reset.** The
> press duration must stay well clear of that; a `static_assert` in
> `loctek.cpp` enforces it, and the byte is pinned by a test.

Button callbacks only post a direction to a queue. `LV_EVENT_PRESS_LOST` is
wired alongside `LV_EVENT_RELEASED` so a finger sliding off a button cannot
leave the desk travelling, and the transmit task carries a
`CONFIG_LOCTEK_MOVE_TIMEOUT_MS` failsafe behind that.

**A control box left alone goes completely silent** — no height, no heartbeat —
and in that state it ignores movement frames too, so the first button press
does nothing. Waking it takes an edge on the wake line: dropped low briefly,
then raised, which is `turnon()` from the upstream Arduino sketch. Unlike that
sketch the line is then *left high*: returning it low is what lets the box go
silent again, while held high it keeps streaming heartbeats and the height
stays current.

Received bytes go through a CRC-checked frame parser; frames arrive
concatenated in a single UART read, so it is a byte-stream state machine.
Heights are three 7-segment patterns with the decimal point on the middle
digit. Blank displays, the `S-1` preset menu and error codes such as `E01` are
all rejected rather than decoded into a bogus number.

## Odds and ends

- The panel is natively 720x1280 portrait, turned a quarter by the P4's PPA as
  each frame is drawn.
- `board::init()` raises the LCD and touch rails before the display starts. The
  BSP tells the two display revisions apart by probing the touch chip, and an
  ST7123 needs the rail up first or detection asserts
  ([esp-bsp#829](https://github.com/espressif/esp-bsp/issues/829)).
- Wi-Fi lives on the ESP32-C6 co-processor and is reached over `esp_hosted`.
- The map is Natural Earth's, drawn to the panel's scale by `tools/make_map.py`.
- Protocol details and the RJ45 pinouts come from
  [iMicknl/LoctekMotion_IoT](https://github.com/iMicknl/LoctekMotion_IoT).
