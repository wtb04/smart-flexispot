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

Everything else is played, and nothing but those read-only feeds leaves the Mac:

- the desk, in `hardware.cpp`, moving as the box moves it and holding its
  presets;
- the focus timer, on `focus_plan.h`;
- Home Assistant, in `home_assistant.cpp`: not answering at first, then with
  readings, lights and a thermostat that the page's taps change;
- the battery, the phone and Wi-Fi.

**H** lists the keys that put it in each of those states.

## Settings

`sim/sim.env`, gitignored, is read by `run.sh`:

```sh
SIM_HOME=lat,lon   # the radar's centre, in place of zone.home
SIM_ZOOM=0.75      # the window's scale, for a smaller screen
```

`--page N` opens a page (0 Home to 4 Setup), `--press KEYS` presses keys as
it starts, `am` for Home Assistant answering and music, `--shot S` saves a
screenshot after S seconds and quits, and `--splash` plays the twelve-second
splash the panel boots with. Together they take a picture of a state without
anyone at the keyboard:

```sh
sim/run.sh --press amm --page 0 --shot 3   # the home page with a Jellyfin episode on
```
