#!/bin/sh
# The README's pictures, from the simulator: the radar over Schiphol, the calendar
# without work, 9:41 on the clock. One at a time, as the feeds turn a crowd away.
cd "$(dirname "$0")" || exit 1
S=../docs/screenshots
export SIM_HOME=52.3105,4.7683
export SIM_DOCK=left  # the dock at the left in every picture
export SIM_CLOCK=9:41  # the same time in every picture
export SIM_NO_WORK=1  # the timetable alone, never the work calendar
./build/sim --press amm --page 0 --shot 16 --out $S/home.png >/dev/null 2>&1
./build/sim --press am --page 0 --shot 16 --out $S/home-music.png >/dev/null 2>&1
./build/sim --press amm --page 0 --tap 72,656 --tap 372,631 --shot 9 --out $S/focus.png >/dev/null 2>&1
./build/sim --press amm --page 0 --tap 72,656 --tap 372,631 --tap 538,423 --shot 8 --out $S/focus-full.png >/dev/null 2>&1
./build/sim --press amm --page 0 --tap 870,46 --tap 1210,152 --shot 6 --out $S/cinema.png >/dev/null 2>&1
./build/sim --press ammcc --page 0 --wait 1500 --tap 704,400 --wait 800 --tap 497,46 --shot 8 --out $S/claude-waiting.png >/dev/null 2>&1
./build/sim --press amm --page 3 --shot 4 --out $S/setup.png >/dev/null 2>&1
./build/sim --press amm --page 3 --tap 704,400 --shot 5 --out $S/diagnostics.png >/dev/null 2>&1
./build/sim --press a --page 0 --pick-above 9000 --shot 20 --out $S/radar.png --tap2 618,229 --out2 $S/radar-full.png >/dev/null 2>&1
./build/sim --page 2 --shot 16 --out $S/calendar.png >/dev/null 2>&1
./build/sim --press ammc --page 0 --wait 12000 --tap 497,46 --shot 4 --out $S/claude.png >/dev/null 2>&1
./build/sim --press amm --page 0 --wait 14000 --then n --shot 2 --out $S/room-notice.png >/dev/null 2>&1
