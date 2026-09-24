# tab5-hello

Standing-desk controller for the M5Stack Tab5 (ESP32-P4), in ESP-IDF + LVGL 9
(C++). Shows the live height reported by a LoctekMotion / Flexispot control box
and drives it up and down from two hold-to-move buttons.

## One-time: ESP-IDF on macOS

```sh
brew install cmake ninja dfu-util python3
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32p4
```

Source the environment in every new shell (worth aliasing to `get_idf`):

```sh
. ~/esp/esp-idf/export.sh
```

## Build and flash

```sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

On a clean tree the first `idf.py build` fails while configuring
`espressif/libjpeg-turbo`: that component asks CMake for the include path of a
target named `idf::libjpeg-turbo`, but the component manager installs it under
its namespace, so the target is `idf::espressif__libjpeg-turbo` and the
expression never resolves. The top-level `CMakeLists.txt` rewrites the line, but
the component is only downloaded part-way through that same run, so it takes
effect on the second. **Run `idf.py build` again and it goes through.** The
component is there for aircraft photographs, which are progressive jpegs; the
part's own engine reads baseline only.

`set-target` regenerates `sdkconfig` from `sdkconfig.defaults`, and the
component manager pulls `espressif/m5stack_tab5` and its dependencies (LVGL,
`esp_lvgl_port`, panel and touch drivers) into `managed_components/` on the
first build. Ctrl-] exits the monitor.

If the port does not show up, hold BOOT while plugging in the USB-C cable to
force download mode.

## Layout

| Path | Purpose |
| --- | --- |
| `main/` | `app_main` and the glue: wiring handlers, telemetry to Home Assistant, diagnostics, the clock |
| `components/board/` | Power rails, panel, touch, LVGL port task |
| `components/ui/` | The screen: `ui.h` is its interface, one file per part, updates applied on the LVGL task |
| `components/desk/` | Presets, the link supervisor and what the desk is doing, over the wire or Bluetooth |
| `components/loctek/` | The control box's frames and the one driver task that writes them; shared with the companion |
| `components/deskproto/` | The panel-to-companion protocol and the desk's vocabulary; shared with the companion |
| `components/ble/` | Phone presence (`ble.h`) and the companion's client (`ble_desk.h`) |
| `components/hass/` | MQTT discovery and state, and the websocket for entities and service calls |
| `components/room/` | The home page's presenter: Home Assistant entities in, taps out |
| `components/wifi/` | Joining and staying joined |
| `components/jpeg/` | The one JPEG engine and its software fallback |
| `components/media/`, `radar/`, `ical/`, `travel/` | Cover art, planes overhead, calendars, journeys |
| `components/power/`, `rtc/`, `sound/`, `settings/`, `logbuf/` | Battery, backup clock, chime, saved settings, the log ring |
| `components/*/test/` | Host-side tests, no hardware needed: `sh components/<name>/test/run.sh` |
| `proxy/` | The desk companion's firmware |
| `sdkconfig.defaults` | Target, chip revision, flash, PSRAM, PPA, LVGL |

`idf.py menuconfig` is authoritative for config; `sdkconfig` is generated and
stays out of git. `dependencies.lock` is committed so builds are reproducible.

## Wiring

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

Two naming traps worth knowing:

- The pinout labels are written from the *control panel's* side, so this is a
  **straight** connection, not a crossed one: box "TX" (pin 6) goes to your TX.
- The "RESET"-ish line you need is the control box's **PIN 20** (RJ45 pin 4),
  which wakes its display. It is *not* M5-Bus pin 6, which is `SOC_RST` and
  resets the Tab5 itself. Do not connect those.
- The wake pin must match where the wire physically is. Driving the wrong one
  is indistinguishable from a control box that refuses to wake, which is a
  miserable thing to debug.

### The proxy board

The panel runs off a battery, so tying it to the desk by a cable defeats the
point. `proxy/` is a second firmware for an ESP32 that stays behind with the
desk and is driven over the air. It shares `components/loctek`, so the frames,
the failsafes and their tests have one home. So does the travel to presets 5
and 6, which the box has no key for: whichever board holds the wire steers the
desk there, and the panel only ever sends the height.

| Signal | ESP32 GPIO | Control box RJ45 |
| --- | --- | --- |
| TX (ESP32 → box) | 16 | pin 5 |
| RX (box → ESP32) | 17 | pin 6 |
| Wake ("PIN 20") | 23 | pin 4 |
| GND | GND | pin 7 |

If nothing decodes, try these two the other way round before touching anything
else. The box answers every frame it receives, so a silent box with a clean
console means it is not hearing us rather than that we are not hearing it, and
the two faults look identical from the receiving end.

The same three signals and the same warnings as above: straight rather than
crossed, meter pins 7 and 8 before trusting any table, and the box's 5 V UART
against a part whose GPIOs are not 5 V tolerant.

One thing differs in the proxy's favour. Pin 8's 5 V can feed the board's
`5V`/`VIN` pin, which is what that pin and its regulator are for, so the proxy
needs no supply of its own -- unlike the Tab5, whose M5-Bus power pins are a
6-24 V raw rail. Meter pin 8 first: on an HS13A-1 it is 29 V and will destroy
the board.

Build it with `cd proxy && idf.py build flash monitor`. It needs the Xtensa
toolchain, which `install.sh esp32` adds alongside the panel's RISC-V one.

Pins, the repeat interval and the travel failsafe are all under
`idf.py menuconfig` → *Loctek desk control*.

The console moves to USB Serial/JTAG in `sdkconfig.defaults`, which is what
frees UART0 (GPIO37/38) for the desk. Flashing and `idf.py monitor` still work
over the USB-C port. If you would rather keep the console on UART0, GPIO6/7
(bus pins 16/15, `PC_TX`/`PC_RX`) are the alternative pair.

## How it fits together

`app_main` brings up the board, builds the UI, starts the desk link, and
returns — the LVGL, driver, receive and supervisor tasks keep running.

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

**Presets**: six, of which the control box has four. A tap sends the desk to
one of those -- the box runs the move itself and ignores a plain stop while it
does, so tapping the same preset again is what cancels it. A long press stores
the current height there, as the M key followed by the preset key.

Presets 5 and 6 are the panel's own. A long press remembers the height; a tap
has the desk driven there by whichever board holds the wire, with one steering
decision per height the box reports rather than on a clock. The desk runs on
after the last key frame, so the keys are released early by a run-on the
driver learns from where each travel comes to rest, per direction, kept in
flash: the first travel after a fresh flash lands a few millimetres past, the
next ones on it, as far as the display can be read -- above a metre it shows
whole centimetres. Any key ends the travel, as does the height ceasing to
change. Home Assistant knows them all as Preset 1 to 6, or Between; the screen
has its own names for them.

A key press is a stream of frames for as long as a finger is down, not a single
frame; sending a preset once registers only sometimes. Presses repeat for
`CONFIG_LOCTEK_PRESS_MS` and end with an explicit release, holding the transmit
lock throughout so the idle poll cannot land in the middle of one and read as an
early release.

> **Five seconds of the M key puts the control box into factory reset.** The
> press duration must stay well clear of that; a `static_assert` in
> `loctek.cpp` enforces it, and the byte is pinned by a test.

Button callbacks run on the LVGL task and only post a direction to a queue.
`LV_EVENT_PRESS_LOST` is wired alongside `LV_EVENT_RELEASED` so a finger
sliding off a button cannot leave the desk travelling, and the transmit task
carries a `CONFIG_LOCTEK_MOVE_TIMEOUT_MS` failsafe behind that.

**A control box left alone goes completely silent** -- no height, no heartbeat
-- and in that state it ignores movement frames too, so the first button press
does nothing. Waking it takes an edge on the wake line: the line is dropped low
briefly, then raised, which is `turnon()` from the upstream Arduino sketch.

Unlike that sketch we then *leave the line high*. Returning it low is what lets
the box go silent again. Held high it keeps streaming heartbeats, the height
stays current and the desk stays responsive. An idle "no keys pressed" frame
once a second keeps it that way; nothing upstream does this, because those
projects are command-driven and can afford to have the first command swallowed.

Received bytes go through a CRC-checked frame parser; frames arrive
concatenated in a single UART read, so it is a byte-stream state machine.
Heights are three 7-segment patterns with the decimal point on the middle
digit. Blank displays, the `S-1` preset menu and error codes such as `E01` are
all rejected rather than decoded into a bogus number.

Anything calling `lv_*` from your own task must sit between
`lvgl_port_lock()` / `lvgl_port_unlock()` (or the BSP's `bsp_display_lock()`).
Calls inside LVGL event callbacks already hold the lock.

Task layout: LVGL on core 1 at priority 4; the desk transmit and receive tasks
on core 0 at priority 6; the link supervisor at priority 3. Task stacks and
queues are statically allocated.

## Tests

The protocol is pure and has no ESP-IDF dependency, so it tests on the host:

```sh
components/loctek/test/run.sh
```

It checks every command frame against the published bytes, pins the preset and
M key mapping, decodes real height captures from control boxes in the wild, and
exercises the parser's framing and resynchronisation. The command frames are
`constexpr`, so a broken CRC is a compile error rather than a test failure.

## Notes

- The panel is natively 720x1280 portrait, hence the 90-degree rotation in
  `board_init()`. `flags.sw_rotate` looks like software rotation but routes
  through the P4's PPA when `CONFIG_LVGL_PORT_ENABLE_PPA` is set — leave it on.
- `board_init()` raises the LCD and touch rails before `bsp_display_start*()`.
  The BSP tells the two display revisions apart by probing the touch chip, and
  an ST7123 needs the rail up first or detection asserts
  ([esp-bsp#829](https://github.com/espressif/esp-bsp/issues/829)).
- The BSP covers both Tab5 display revisions (ILI9881C and ST7123).
- Early Tab5 units carry an ESP32-P4 v1.x die; `sdkconfig.defaults` targets
  that. Check yours with `esptool.py --port <port> chip_id`.
- Wi-Fi lives on the ESP32-C6 co-processor and is reached over `esp_hosted`.
- GPIO37/38 are ESP32-P4 strapping pins (boot mode). The control box's TX idles
  high, which is the safe state, but if the Tab5 ever refuses to boot with the
  desk attached, unplug it before blaming the firmware.
- Protocol details and the RJ45 pinouts come from
  [iMicknl/LoctekMotion_IoT](https://github.com/iMicknl/LoctekMotion_IoT).
  Not every control box speaks this dialect — the HCB2xx series is known not
  to.
