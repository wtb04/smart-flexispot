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
where the panel has Home Assistant's `zone.home`. Nothing else goes out: Home
Assistant, the desk and Jellyfin are never contacted. The
home page's layout comes from `components/room`, as it looks until Home
Assistant answers.

Only the hardware is played, by `hardware.cpp`: the desk moves as the box moves
it and holds its presets, the focus timer runs on `focus_plan.h`, and these keys
change what nothing on the screen can:

| Key | |
|---|---|
| D | desk link lost, and back |
| B | battery: charging, on battery, low, none |
| P | phone away, and back, which hides the pages it gates as on the panel |
| W | Wi-Fi down, and back |
| S | screenshot into `sim/shots/` |

## Settings

`sim/sim.env`, gitignored, is read by `run.sh`:

```sh
SIM_HOME=lat,lon   # the radar's centre, in place of zone.home
SIM_ZOOM=0.75      # the window's scale, for a smaller screen
```

`--page N` opens a page (0 Home to 4 Setup), `--shot S` saves a screenshot
after S seconds and quits, and `--splash` plays the twelve-second splash the
panel boots with.
