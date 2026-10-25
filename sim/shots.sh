#!/bin/sh
# The README's pictures, from the simulator: the radar over Schiphol, the calendar
# without work, 9:41 on the clock. One at a time, as the feeds turn a crowd away.
cd "$(dirname "$0")" || exit 1
S=../docs/screenshots
export SIM_HOME=52.3105,4.7683
export SIM_CLOCK=9:41  # the same time in every picture
export SIM_NO_WORK=1  # the timetable alone, never the work calendar
# the episode past its intro, which its Skip in the bar would cover the title of
./build/sim --press amm --page 0 --pick-above 5000 --wait 7000 --tap 1035,64 --shot 16 --out $S/home.png >/dev/null 2>&1
./build/sim --press am --page 0 --pick-above 5000 --shot 16 --out $S/home-music.png >/dev/null 2>&1
./build/sim --press am --page 0 --pick-above 5000 --tap 70,653 --tap 346,653 --shot 9 --out $S/focus.png >/dev/null 2>&1
./build/sim --press amm --page 0 --tap 70,653 --tap 346,653 --tap 521,272 --shot 8 --out $S/focus-full.png >/dev/null 2>&1
./build/sim --press amm --page 0 --tap 1040,51 --tap 872,187 --shot 6 --out $S/cinema.png >/dev/null 2>&1
./build/sim --press ammmm --page 0 --tap 1040,51 --shot 6 --out $S/laptop-card.png >/dev/null 2>&1
./build/sim --press ammmm --page 0 --tap 1040,51 --tap 872,187 --shot 6 --out $S/laptop.png >/dev/null 2>&1
./build/sim --press amcc --page 0 --pick-above 5000 --wait 1500 --tap 640,180 --wait 800 --tap 464,51 --shot 8 --out $S/claude-waiting.png >/dev/null 2>&1
./build/sim --press am --page 3 --shot 4 --out $S/setup.png >/dev/null 2>&1
./build/sim --press am --page 3 --tap 290,408 --shot 5 --out $S/diagnostics.png >/dev/null 2>&1
# one run for both, so Home and the radar page show the same airliner, well up
./build/sim --press a --page 0 --pick-above 9000 --shot 20 --out $S/radar.png --tap2 70,548 --out2 $S/radar-full.png >/dev/null 2>&1
./build/sim --press am --page 2 --shot 16 --out $S/calendar.png >/dev/null 2>&1
./build/sim --press amc --page 0 --pick-above 5000 --wait 12000 --tap 464,51 --shot 4 --out $S/claude.png >/dev/null 2>&1
./build/sim --press am --page 0 --pick-above 5000 --wait 14000 --then n --shot 2 --out $S/room-notice.png >/dev/null 2>&1
