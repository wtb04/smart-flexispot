# Simulator

The panel's screen in a window on the Mac, to work on the UI without flashing:
`components/ui` built as it is, drawn by the same LVGL into SDL, the mouse as
the touchscreen.

```sh
brew install cmake ninja sdl2
sim/run.sh            # builds and opens it
sim/run.sh --watch    # and again whenever components/ui or sim changes
```

LVGL comes from `managed_components/`, so build the firmware once first; without
it, the same release is fetched from GitHub.

## What is real

The feeds the panel reads from the internet are fetched for real and read by
the panel's own parsers: the calendar and the way there with the secrets in
`components/ical` and `components/travel`, and the radar round `SIM_HOME`,
where the panel has Home Assistant's `zone.home`. The home page's layout comes
from `components/room`.

They go through the panel's own `net` core, which decides what goes when, with
curl in place of the ESP client (`live/net_desktop.cpp`). Its list of the hosts
the simulator may read is the one place that decides what leaves the Mac: any
other host, and anything but a read, is refused and logged.

Everything else is played, and nothing but those read-only feeds leaves the Mac:

- the desk, in `hardware.cpp`, moving as the box moves it and holding its
  presets, with the notices the panel gives for them;
- the focus timer, on `focus_plan.h`, with its notices as each part runs out;
- Home Assistant, in `home_assistant.cpp`: not answering at first, then with
  readings, lights and a thermostat that the page's taps change;
- what plays, in `media_stub.cpp`: the speaker with music, and a Jellyfin
  episode with its intro, the episodes either side, subtitles and cinema mode;
- notices from Home Assistant, in `notices.cpp`, read by the panel's own parser;
- firmware updates, in `updates.cpp`, arriving and waiting to be installed;
- the battery, the phone and Wi-Fi.

**H** lists the keys that put it in each of those states. Any notice Home
Assistant could send goes to a running simulator with

```sh
sim/send.sh notify '{"title":"Hello","message":"Kept until tapped","level":"warning","timeout_s":0}'
```

## Settings

`SIM_OFFLINE=1` leaves the live feeds off, as CI does for its screenshots.

`sim/sim.env`, gitignored, is read by `run.sh`:

```sh
SIM_HOME=lat,lon   # the radar's centre, in place of zone.home
SIM_ZOOM=0.75      # the window's scale, for a smaller screen
```

`--page N` opens a page (0 Home to 4 Setup), `--press KEYS` presses keys as
it starts, `am` for Home Assistant answering and music, `--tap X,Y` taps the
screen there, as often as given, `--shot S` saves a
screenshot after S seconds and quits, and `--splash` plays the twelve-second
splash the panel boots with. Together they take a picture of a state without
anyone at the keyboard:

```sh
sim/run.sh --press amm --page 0 --shot 3   # the home page with a Jellyfin episode on
```
