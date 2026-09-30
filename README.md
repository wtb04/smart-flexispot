# Smart Flexispot

A panel for the desk: an M5Stack Tab5 (ESP32-P4) that drives a Flexispot
standing desk and keeps the rest of the room within reach of it. The desk's
height and presets, the room through Home Assistant, whatever Jellyfin is
playing, the planes overhead, the day's timetable with when to leave for it,
and a focus timer. ESP-IDF 5.5 and LVGL 9, in C++.

![The home page](docs/screenshots/home.png)

The screenshots here come from the simulator in `sim/`, which runs the panel's
own UI on the desktop; the radar's are from the live feed over the North Sea.

## What it does

### The desk

The rail down the left shows the height as the control box reports it, to the
millimetre below a metre and the centimetre above, as its own display does.
Stand and Sit are one tap each; the arrow opens the rest of the six presets.
A long press stores the current height.

Four of the presets are the control box's own. Five and six are the panel's:
the box has no key for them, so the panel drives the desk there itself and
learns from each travel how far the desk runs on after the keys are let go.

The desk can be on the end of a cable to the Tab5, or on a small ESP32 left
behind at the desk and driven over Bluetooth (see [The companion](#the-companion)),
which is what keeps the panel free to move.

### The room

The home page is Home Assistant's: the air (CO2, VOC, humidity, PM2.5, each
with a dot for how it is doing), a thermostat to turn, the lights, and a card
for what is playing. Taps go straight to Home Assistant over its websocket; the
panel publishes itself over MQTT, so Home Assistant sees its height, presets,
screen, brightness and battery, can move the desk, and can send it notices.

![A notice over the cinema view](docs/screenshots/notice.png)

Notices from Home Assistant slide in over whatever is on screen, fullscreen
views included, and go after a while or when tapped.

### Films and series

When something plays on Jellyfin, the media card follows it: cover, episode,
where it is. Holding the card sends the desk to its viewing height. The cinema
view has the episode's still, the time it ends, skip-intro and on to the next
episode where Jellyfin knows them, subtitles, volume, and the lights and desk
within a thumb's reach.

![The cinema view](docs/screenshots/cinema.png)

A player that takes no commands from elsewhere, as the Streamyfin app does not,
is shown and followed but its controls are faded rather than offered.

### Planes overhead

A radar of what flies round the desk, from 20 to 160 kilometres out, on a map
of the coast and the borders, each plane coloured by its height. Tapping a plane shows who it is, where it is
going, and its photograph when one exists; its trail grows back from it along
where it has been. Fullscreen, the map goes to every edge.

| | |
| --- | --- |
| ![The radar](docs/screenshots/radar.png) | ![The radar fullscreen](docs/screenshots/radar-full.png) |

Positions come from adsb.lol and adsb.fi in turn, aircraft and routes from
adsbdb, photographs from Planespotters, the trails from adsb.lol's traces.

### The day

The calendar page reads iCal feeds (lectures, practicals, exams and work,
here) and puts the next thing up top with when to leave for it. The journey
comes from Reis, a small service of mine that plans it: each leg, by bike,
train, bus or on foot, with delays and cancellations as they come in, and the
next few departures to choose from.

### Focus

A focus timer in rounds, 25 minutes on and 5 off by default with a longer
break after four. The set's timetable sits beside the dial, the tab in the nav
bar counts down on every page, and fullscreen the dial fills the screen. A
restart halfway through a round picks it up where the clock says it is.

| | |
| --- | --- |
| ![Focus](docs/screenshots/focus.png) | ![Focus fullscreen](docs/screenshots/focus-full.png) |

### Away from the desk

The panel knows the phone by its Bluetooth identity key. When it has gone, the
screen goes dark, the Wi-Fi saves power, and nothing is fetched that only the
screen would show; it wakes when the phone comes back or the screen is touched.
The accelerometer turns the picture over when the panel is stood the other way
up.

### Setup and diagnostics

Setup has the brightness, the accent colour, which side the rail is on, the
orientation, the focus timer's lengths, and the restart. Diagnostics is a card
for each part (Wi-Fi, Home Assistant, presence, the desk and its link, the
battery, the radar, the calendar, the system) and a log you can filter by
part.

| | |
| --- | --- |
| ![Setup](docs/screenshots/setup.png) | ![Diagnostics](docs/screenshots/diagnostics.png) |

It starts on a splash that shows the desk, the network and Home Assistant
coming up, and gets to the home page in about ten seconds.

## Hardware

- An M5Stack Tab5. Early units carry an ESP32-P4 v1.x die, which
  `sdkconfig.defaults` targets; check yours with `esptool.py --port <port> chip_id`.
  Both display revisions (ILI9881C and ST7123) work.
- A LoctekMotion / Flexispot control box that speaks the HS01B / HS13B dialect.
  Not every one does: the HCB2xx series is known not to.
- Optionally, any ESP32 as the companion, to leave the cable at the desk.

### Wiring the Tab5 to the desk

⚠️ **The control box UART runs at 5 V, the Tab5's GPIOs are 3.3 V, and there is
no level shifter or series resistance anywhere on the M5-Bus.** Measured on a
scope in [esp-bsp#34](https://github.com/iMicknl/LoctekMotion_IoT/issues/34).
People do wire this direct and report it working, but it is out of spec for the
P4. A level shifter, or at minimum a series resistor on RX and on the wake
line, is cheap insurance. Your call — this is stated so it is a decision, not a
surprise.

Also: **do not feed the desk's 5 V (RJ45 pin 8) into the Tab5.** M5-Bus pins
25/27/29 are the 6–24 V raw input rail, not 5 V. The Tab5 powers itself.

| Signal | Tab5 M5-Bus | GPIO | Control box RJ45 |
| --- | --- | --- | --- |
| TX (Tab5 → box) | pin 14, `TXD0` | 37 | pin 6 |
| RX (box → Tab5) | pin 13, `RXD0` | 38 | pin 5 |
| Wake ("PIN 20") | pin 23 | 47 | pin 4 |
| GND | pin 1, 3 or 5 | — | pin 7 |

The RJ45 column is the HS01B-1 / HS13B-1 pinout, which upstream found works for
most control panels. **Meter your own panel first** — the HS13A-1 puts 29 V on
pins 7/8, and people have bricked control boxes by trusting the wrong table.

Three traps worth knowing:

- The pinout labels are written from the *control panel's* side, so this is a
  **straight** connection, not a crossed one: box "TX" (pin 6) goes to your TX.
- The "RESET"-ish line you need is the control box's **PIN 20** (RJ45 pin 4),
  which wakes its display. It is *not* M5-Bus pin 6, which is `SOC_RST` and
  resets the Tab5 itself. Do not connect those.
- The wake pin must match where the wire physically is. Driving the wrong one
  is indistinguishable from a control box that refuses to wake, which is a
  miserable thing to debug.

GPIO37/38 are ESP32-P4 strapping pins. The control box's TX idles high, which is
the safe state, but if the Tab5 ever refuses to boot with the desk attached,
unplug it before blaming the firmware. The console is on USB Serial/JTAG
(`sdkconfig.defaults`), which is what frees UART0 for the desk; flashing and
`idf.py monitor` still work over the USB-C port.

### The companion

`proxy/` is the firmware for an ESP32 that stays with the desk and is driven
over Bluetooth, so the panel can run off its battery anywhere in the room. It
shares `components/loctek`, so the frames, the failsafes and their tests have
one home, and so does the travel to presets 5 and 6: whichever board holds the
wire steers the desk there, and the panel only ever sends the height.

| Signal | ESP32 GPIO | Control box RJ45 |
| --- | --- | --- |
| TX (ESP32 → box) | 16 | pin 5 |
| RX (box → ESP32) | 17 | pin 6 |
| Wake ("PIN 20") | 23 | pin 4 |
| GND | GND | pin 7 |

If nothing decodes, try these two the other way round before touching anything
else. The box answers every frame it receives, so a silent box with a clean
console means it is not hearing us rather than that we are not hearing it, and
the two faults look identical from the receiving end. The same warnings apply:
straight rather than crossed, meter pins 7 and 8 before trusting any table.

One thing is in the companion's favour: pin 8's 5 V can feed the board's
`5V`/`VIN` pin, which is what that pin and its regulator are for, so it needs no
supply of its own. Meter pin 8 first: on an HS13A-1 it is 29 V and will destroy
the board.

Build it with `cd proxy && idf.py build flash monitor`. It needs the Xtensa
toolchain, which `install.sh esp32` adds alongside the panel's RISC-V one. Pins,
the repeat interval and the travel failsafe are under `idf.py menuconfig` →
*Loctek desk control*.

## Building

### ESP-IDF

```sh
brew install cmake ninja dfu-util python3
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32p4 esp32
. ~/esp/esp-idf/export.sh     # in every new shell
```

### Secrets

Each thing the panel talks to has its settings in a header that stays out of
git, next to a template that does not. Copy each `*_secrets.example.h` to
`*_secrets.h` in the same folder and fill in what you have; anything left empty
is simply not started.

| File | What goes in it |
| --- | --- |
| `components/wifi/include/wifi_secrets.h` | The network |
| `components/hass/include/hass_secrets.h` | Home Assistant's websocket address and token, and the MQTT broker |
| `components/jellyfin/include/jellyfin_secrets.h` | The Jellyfin server and an API key |
| `components/ical/include/ical_secrets.h` | The calendar feeds |
| `components/travel/include/travel_secrets.h` | The journey service and its key |
| `components/ble/include/ble_secrets.h` | The phone's Bluetooth identity key, for presence |
| `components/ota/include/ota_secrets.h` | The key updates and the development pages ask for |

### Flash

```sh
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

`sdkconfig` is generated from `sdkconfig.defaults` and stays out of git;
`dependencies.lock` is committed, so the component manager fetches the same
versions every time. Ctrl-] leaves the monitor. If the port does not show up,
hold BOOT while plugging in the USB-C cable.

### Over the air

Once both boards have been flashed by cable, neither needs it again:

```sh
tools/ota.sh panel            # the panel's, over Wi-Fi
tools/ota.sh companion        # the companion's, which the panel passes on over Bluetooth
tools/ota.sh both             # both, companion first
tools/ota.sh panel --now      # install straight away rather than keep it ready
```

An update is kept ready unless `--now` says otherwise: the rail shows it
arriving, a dot then sits on Setup, and the restart tile there reads Update now
until it is tapped. Updates are refused while the desk moves, and each board
refuses an image that is not its own.

A firmware booted from an update is on trial: the panel until it rejoins
Wi-Fi, the companion until the panel links. If it gets that far it stays; if
not within three minutes it restarts, and the bootloader returns to the
firmware before. The panel then says so, once, in a notice, and Home
Assistant's *Last update* sensor reads *rolled back* until the next update
takes.

A change to `partitions.csv` needs the cable: an update carries the app, not
the table.

## Developing

### The simulator

```sh
sim/run.sh
```

opens the panel's UI in a window, built from the same `components/ui` with the
hardware and Home Assistant played by stubs you drive from the keyboard. The
calendar, journey and radar are fetched for real, through the same scheduling
core the panel uses, and only those: anything else is refused before it leaves
the Mac. [`sim/README.md`](sim/README.md) has the keys and the options.

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

Every push runs [three jobs](.github/workflows/ci.yml): the host tests with
coverage, whose table ends up in the run's summary; the firmware, both the
normal build and the development one, in Espressif's image for ESP-IDF 5.5.5,
from `sdkconfig.defaults` alone and the secrets' templates; and the simulator
on Linux, whose screenshots are kept with the run.

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
| `/heap`, `/power` | Memory, and the pack's draw on battery |
| `/streams` | Each live connection and how it is doing; `?restart=N` begins one again |
| `/jobs` | Each shared worker's jobs; `?poke=N` runs one now |
| `/restart` | Why this run started, and what the last one left behind |
| `/coredump` | The last crash, whole, for `idf.py coredump-info` with the build's ELF |
| `/bench`, `/stall`, `/crash` | Timings, a held-off interrupt, a crash on purpose |

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

Anything calling `lv_*` from its own task must sit between `lvgl_port_lock()`
and `lvgl_port_unlock()`; LVGL's own callbacks already hold it. Updates from
other tasks go through `ui.h`, which only copies them for the LVGL task to
draw.

### The desk

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

## Notes

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
