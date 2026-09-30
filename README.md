<h1 align="center">Smart Flexispot</h1>

<p align="center">
  A room controller that took the place of my standing desk's keypad.
</p>

<p align="center">
  <a href="https://github.com/wtb04/smart-flexispot/actions/workflows/ci.yml"><img src="https://github.com/wtb04/smart-flexispot/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <img src="https://img.shields.io/badge/ESP--IDF-5.5.5-e7352c" alt="ESP-IDF 5.5.5">
  <img src="https://img.shields.io/badge/LVGL-9-343839" alt="LVGL 9">
  <img src="https://img.shields.io/badge/board-M5Stack%20Tab5-orange" alt="M5Stack Tab5">
</p>

<p align="center">
  <a href="#a-tour">A tour</a> |
  <a href="#getting-started">Getting started</a> |
  <a href="#try-it-without-the-hardware">Simulator</a> |
  <a href="#faq-and-troubleshooting">FAQ</a> |
  <a href="docs/how-it-works.md">How it works</a>
</p>

![The home page](docs/screenshots/home.png)

Smart Flexispot is firmware for an [M5Stack Tab5](https://docs.m5stack.com/en/core/Tab5) that sits on my desk and drives the Flexispot under it. It replaces the desk's own keypad and controls the room from there: the lights and the heating through Home Assistant, and the desk itself. Along the way it also picked up what is playing on Jellyfin, my timetable with when to leave for it, the planes going over, and a focus timer.

> [!NOTE]
> **This is a showcase, not a product.** It is built around my desk, my room, my Home Assistant, my calendars and my phone, so a lot of it only makes sense in my setup. Treat it as a demo of what a desk panel can do and take whatever ideas or pieces are useful. Getting it running for yourself will mean changing things.

> [!NOTE]
> **Built with AI.** I came up with what it should do and it runs on my desk every day, but most of the code was written with the help of AI rather than by hand. For something this big, that is what made it a project of weeks instead of months. Read the code with that in mind.

I am not affiliated with Flexispot, LoctekMotion or M5Stack.

## Highlights

- **The room.** The lights, the heating and the air from Home Assistant, and the panel itself shows up in Home Assistant over MQTT.
- **The desk.** Height, Stand and Sit, six presets instead of the control box's four, over a cable or over Bluetooth.
- **Films and series.** Follows what plays on Jellyfin, with a cinema view that has the lights and the desk within reach.
- **Planes overhead.** A live radar with who is flying, where to, their photo and their trail.
- **The day.** The next lecture, a countdown, and when to leave to get there on time.
- **Focus timer**, a **presence sensor** from your phone, and **diagnostics** for every part, on the panel itself.
- **A simulator** that runs the real UI on a Mac, and host tests with coverage in CI.

## Why

I wanted one place to control my room from, mostly the lights and the heating. The spot where the desk's keypad sat, right at the edge of the desk, turned out to be the perfect place for it. Then I found [LoctekMotion_IoT](https://github.com/iMicknl/LoctekMotion_IoT), which works out what the control box and its keypad say to each other, so I could build my own controller and replace the keypad entirely.

Once the screen was there, the extra features kept coming, each one making it a bit more useful: what is playing, when to leave for the next lecture, what that plane is, a timer for focusing.

## A tour

### The room

![A notice over the cinema view](docs/screenshots/notice.png)

The home page is my Home Assistant: CO2, VOC, humidity and PM2.5 with a dot for how each is doing, the thermostat, the lights, and what is playing. The panel also shows up in Home Assistant itself over MQTT, with its height, presets, screen and battery, so automations can move the desk or send a notice to the screen.

### The desk

The rail down the left is always there: the height as the control box reports it, Stand and Sit, and an arrow to the other presets. Tap a preset to go there, hold it to save the current height.

- **Six presets instead of four.** The control box has four of its own. The panel drives the desk to the other two itself, and learns how far the desk rolls on after the keys are let go, so it stops where you asked.
- **No cable across the room, if you want.** The desk can be wired straight to the Tab5, or to a small ESP32 left at the desk that the panel talks to over Bluetooth (see [option 2](#option-2-over-bluetooth-with-a-companion)).
- **Safe by default.** A key that stops changing the height is let go of, a move from Home Assistant stops unless it keeps being asked for, and the byte that would factory-reset the control box is pinned by a test.

### Films and series

![The cinema view](docs/screenshots/cinema.png)

When something plays on Jellyfin the media card follows it, and holding the card sends the desk to its viewing height. The cinema view has the episode's still, when it ends, skip intro, next episode, subtitles, volume, and the lights and the desk within reach. Players that cannot be controlled from elsewhere (the Streamyfin app, for one) are still followed, their buttons just fade out.

### Planes overhead

| Radar | Fullscreen |
|---|---|
| ![The radar](docs/screenshots/radar.png) | ![The radar fullscreen](docs/screenshots/radar-full.png) |

This one is purely because it's fun. A radar of everything flying within 20 to 160 km, on a map of the coast and the borders, each plane coloured by its height. Tap one to see the airline, the aircraft, the route and a photo, and its trail draws itself back along where it has been. The pictures above are over Schiphol.

Positions come from [adsb.lol](https://adsb.lol) and [adsb.fi](https://adsb.fi), aircraft and routes from [adsbdb](https://www.adsbdb.com), photos from [Planespotters](https://www.planespotters.net).

### The day

![The calendar](docs/screenshots/calendar.png)

The calendar page reads iCal feeds, mine come from [CalendarChanger](https://github.com/wtb04/CalendarChanger), and puts the next thing up top with a countdown. With a journey planner set up it also tells you when to leave: every leg by bike, train, bus or on foot, with delays and cancellations as they come in.

### Focus

| Focus | Fullscreen |
|---|---|
| ![Focus](docs/screenshots/focus.png) | ![Focus fullscreen](docs/screenshots/focus-full.png) |

A focus timer in rounds, 25 minutes on and 5 off by default with a long break after four. The countdown stays on the tab in the nav bar on every page, and if the panel restarts halfway through a round it picks it up where it should be.

### Away from the desk

The panel knows my phone over Bluetooth. When I walk away the screen goes dark and it stops fetching anything only the screen would show, and when I come back it wakes up. Turn it upside down and the picture turns with it.

### Setup and diagnostics

| Setup | Diagnostics |
|---|---|
| ![Setup](docs/screenshots/setup.png) | ![Diagnostics](docs/screenshots/diagnostics.png) |

Brightness, accent colour, which side the rail is on, the focus lengths. Diagnostics has a card for every part (Wi-Fi, Home Assistant, the desk and its link, the battery, the radar, the calendar) and a log you can filter, so when something is off you can see why without a laptop.

## Will it work with my desk?

Probably, if the control box speaks the HS01B / HS13B dialect, which most LoctekMotion boxes (and so most Flexispots) do. The HCB2xx series does not.

The protocol and pinouts come from [iMicknl/LoctekMotion_IoT](https://github.com/iMicknl/LoctekMotion_IoT), which did the hard work of figuring out what the control box says. If your box is not covered there, it will not work here either.

## Getting started

### 1. Get the hardware

- An M5Stack Tab5. Early units have an ESP32-P4 v1.x chip, which `sdkconfig.defaults` targets. Check yours with `esptool.py --port <port> chip_id`. Both display revisions (ILI9881C and ST7123) work.
- A Flexispot with a supported control box (see above) and an RJ45 cable you do not mind cutting.
- Optionally, any ESP32 as the companion.

### 2. Connect the desk

There are two ways to reach the control box. Either the Tab5 is wired to it directly, or a small ESP32 stays at the desk on the wire and the Tab5 talks to that over Bluetooth, so the panel can go anywhere in the room on its battery.

```mermaid
flowchart TB
    subgraph one ["Option 1: a cable"]
        direction LR
        tab5a["Tab5"] ---|"UART, RJ45 cable"| box1["Control box"]
    end
    subgraph two ["Option 2: Bluetooth"]
        direction LR
        tab5b["Tab5"] -. "Bluetooth" .- esp["ESP32 companion"]
        esp ---|"UART, RJ45 cable"| box2["Control box"]
    end
    one ~~~ two
```

> [!WARNING]
> **The control box talks at 5 V and the Tab5's pins are 3.3 V**, with no level shifter anywhere on the M5-Bus ([measured here](https://github.com/iMicknl/LoctekMotion_IoT/issues/34)). People wire it directly and it works for them, but it is out of spec. A level shifter, or at least a series resistor on RX and the wake line, is cheap insurance.

The pins below are the HS01B-1 / HS13B-1 pinout. **Measure your own first**: the HS13A-1 puts 29 V on pins 7 and 8, and people have killed control boxes by trusting the wrong table.

#### Option 1: a cable to the Tab5

```mermaid
flowchart LR
    subgraph tab5 ["Tab5, M5-Bus"]
        t_tx["pin 14, GPIO37, TX"]
        t_rx["pin 13, GPIO38, RX"]
        t_wake["pin 23, GPIO47, wake"]
        t_gnd["pin 1, 3 or 5, GND"]
    end
    subgraph rj45 ["Control box, RJ45"]
        r6["pin 6"]
        r5["pin 5"]
        r4["pin 4, PIN 20"]
        r7["pin 7, GND"]
    end
    t_tx --- r6
    t_rx --- r5
    t_wake --- r4
    t_gnd --- r7
    linkStyle 0 stroke:#f58a3a,stroke-width:3px
    linkStyle 1 stroke:#4a90d9,stroke-width:3px
    linkStyle 2 stroke:#5bb974,stroke-width:3px
    linkStyle 3 stroke:#888888,stroke-width:3px
```

#### Option 2: over Bluetooth, with a companion

The companion is an ESP32 DevKit V1 (30-pin) on a small carrier board with an RJ45 jack. The board takes its power from the desk, shifts the box's 5 V signal down to 3.3 V for the ESP32, and uses the two LEDs in the jack for Bluetooth (`BT`) and the desk link (`LINK`).

| Top | Bottom |
|---|---|
| ![The carrier board, top](pcb/img/board-top.png) | ![The carrier board, bottom](pcb/img/board-bottom.png) |

![The carrier board in 3D, with the DevKit socket and the RJ45 jack](pcb/img/board-3d.png)

```mermaid
flowchart LR
    subgraph rj45 ["Control box, RJ45"]
        p8["pin 8, 5 V"]
        p7["pin 7, GND"]
        p6["pin 6, box TX"]
        p5["pin 5, box RX"]
        p4["pin 4, wake"]
    end
    subgraph board ["Carrier board"]
        fuse["PTC fuse, diode"]
        shift["10k / 15k divider, clamp"]
        rtx["220 Ω"]
        rwake["220 Ω"]
    end
    subgraph esp ["ESP32 DevKit"]
        vin["VIN"]
        gnd["GND"]
        rx["GPIO16, RX2"]
        tx["GPIO17, TX2"]
        wake["GPIO23"]
    end
    p8 --- fuse --- vin
    p7 --- gnd
    p6 --- shift --- rx
    p5 --- rtx --- tx
    p4 --- rwake --- wake
    linkStyle 0,1 stroke:#d9534f,stroke-width:3px
    linkStyle 2 stroke:#888888,stroke-width:3px
    linkStyle 3,4 stroke:#4a90d9,stroke-width:3px
    linkStyle 5,6 stroke:#f58a3a,stroke-width:3px
    linkStyle 7,8 stroke:#5bb974,stroke-width:3px
```

<details>
<summary>The schematic</summary>

![The carrier board's schematic](pcb/img/schematic.png)

</details>

Everything to have it made is in [pcb/](pcb/): the KiCad project, the [schematic as a PDF](pcb/fab/tab5-desk-ctrl-schematic.pdf), and the gerbers, BOM and placement files for JLCPCB.

Flash `proxy/` onto the DevKit with `cd proxy && idf.py build flash monitor`. Its defaults are for the breadboard it was first built on, which had TX and RX the other way round, so for the board set `CONFIG_LOCTEK_TX_GPIO=17` and `CONFIG_LOCTEK_RX_GPIO=16` under `idf.py menuconfig`, *Loctek desk control*.

### 3. Install ESP-IDF

```sh
brew install cmake ninja dfu-util python3
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32p4 esp32
. ~/esp/esp-idf/export.sh     # in every new shell
```

`esp32` is only needed for the companion.

### 4. Fill in your secrets

Everything the panel talks to has a `*_secrets.example.h` template next to where its real one goes. Copy each to `*_secrets.h` in the same folder and fill in what you have. Anything left empty is just not started, so the desk alone works with no secrets at all.

| File | What goes in it |
|---|---|
| `components/wifi/include/wifi_secrets.h` | Your Wi-Fi |
| `components/hass/include/hass_secrets.h` | Home Assistant's address and a token, and the MQTT broker |
| `components/jellyfin/include/jellyfin_secrets.h` | The Jellyfin server and an API key |
| `components/ical/include/ical_secrets.h` | Your calendar feeds |
| `components/travel/include/travel_secrets.h` | The journey planner and its key |
| `components/ble/include/ble_secrets.h` | Your phone's Bluetooth identity key, for presence |
| `components/ota/include/ota_secrets.h` | A key for updates over Wi-Fi |

### 5. Flash it

Plug the Tab5 in over USB-C and:

```sh
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

Ctrl-] leaves the monitor. If the port does not show up, hold BOOT while plugging in.

It boots into a splash that shows the desk, the network and Home Assistant coming up, and is on the home page in about ten seconds.

Every CI run also keeps a `smart_flexispot-full.bin` that can be written at 0x0 without building anything:

```sh
esptool.py --chip esp32p4 -p /dev/cu.usbmodem* write_flash 0x0 smart_flexispot-full.bin
```

Those are built from the templates though, so they have no network: enough to try the screen and the desk, not the rest.

### 6. Update over Wi-Fi

After the first cable flash, neither board needs the cable again:

```sh
tools/ota.sh panel            # the panel, over Wi-Fi
tools/ota.sh companion        # the companion, passed on by the panel over Bluetooth
tools/ota.sh both             # both, companion first
tools/ota.sh panel --now      # install right away instead of when you tap Update now
```

An update waits on the Setup page until you tap it, and is refused while the desk moves. A new firmware is on trial until it gets back on Wi-Fi. If it does not within three minutes, the panel goes back to the version before and tells you so.

## Try it without the hardware

The simulator runs the panel's own UI in a window on your Mac, with the desk, Home Assistant and Jellyfin played by stubs you drive from the keyboard:

```sh
sim/run.sh
```

The calendar, journey and radar are real, and those are the only things it is allowed to reach, so it can never move a real desk or turn off your lights. All screenshots here are from it. See [sim/README.md](sim/README.md) for the keys.

## FAQ and troubleshooting

**Do I need Home Assistant?**

No. Leave its secrets empty and the home page stays empty, while the desk, calendar and focus timer work without it. The radar does need it: it takes its centre from Home Assistant's home zone.

**The desk does not move on the first tap**

A control box that has been left alone goes completely silent and ignores movement until it is woken. That is what the wake line (RJ45 pin 4) is for, so check it is wired and that the wake GPIO matches where the wire actually is. Driving the wrong pin looks exactly like a box that will not wake.

**Nothing comes back from the control box at all**

The pin labels are from the control panel's side, so the connection is straight, not crossed: box TX goes to your TX. On the companion, try swapping TX and RX before anything else. The box answers every frame it gets, so silence means it is not hearing you.

**The Tab5 will not boot with the desk plugged in**

GPIO37 and 38 are strapping pins on the P4. The control box idles at the safe level, but if the Tab5 ever refuses to boot with the desk attached, unplug it before blaming the firmware.

**Which pin is reset?**

Not M5-Bus pin 6. That is `SOC_RST` and resets the Tab5 itself. The one you want is the control box's PIN 20, RJ45 pin 4.

**An update did not stick**

The panel rolled back because the new firmware did not get back on the network in time. It says so in a notice, and Home Assistant's *Last update* sensor reads *rolled back* until the next one takes. A change to `partitions.csv` always needs the cable.

## Under the hood

- **One network scheduler.** Every request and every live connection (Home Assistant's websocket and MQTT, Jellyfin's websocket) goes through one place that decides what goes first, retries, backs off and waits for Wi-Fi.
- **Shared workers** for everything periodic, instead of a task each, which freed most of the internal RAM the radio and TLS need.
- **A watchdog, crash dumps and rollback.** A stuck worker restarts the panel, a crash leaves its dump in flash, and an update that does not reach the network is rolled back.
- **A display that does not flicker.** The screen is fed by DMA round a ring of frames, so a busy moment no longer shows as a blue flash.
- **Tested off the device.** The schedulers, the desk protocol and every parser are plain C++, tested on the host under GoogleTest.

The whole story, and how to work on it, is in [docs/how-it-works.md](docs/how-it-works.md).

## Credits

- [iMicknl/LoctekMotion_IoT](https://github.com/iMicknl/LoctekMotion_IoT) for working out the control box protocol and pinouts. This project would not exist without it.
- [adsb.lol](https://adsb.lol), [adsb.fi](https://adsb.fi), [adsbdb](https://www.adsbdb.com) and [Planespotters](https://www.planespotters.net) for the planes, and the photographers for their photos.
- [Natural Earth](https://www.naturalearthdata.com) for the map.

Made by [Wouter ten Brinke](https://woutertenbrinke.nl).
