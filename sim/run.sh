#!/bin/sh
# Builds the simulator and opens it. With --watch it builds and opens it again
# whenever components/ui or sim changes. The rest is passed on to it: --page N,
# --shot S, --splash.
set -e
cd "$(dirname "$0")"
if [ -f sim.env ]; then
    set -a
    . ./sim.env
    set +a
fi

if [ "$1" != "--watch" ]; then
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null
    ninja -C build
    exec ./build/sim "$@"
fi
shift

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null
ninja -C build
touch build/.watched
./build/sim "$@" &
pid=$!
while sleep 1; do
    kill -0 "$pid" 2>/dev/null || break  # the window was closed
    changed=$(find ../components/ui . -newer build/.watched \( -name '*.cpp' -o -name '*.c' -o -name '*.h' \) \
        -not -path './build/*' | head -1)
    [ -n "$changed" ] || continue
    touch build/.watched
    if ninja -C build >/dev/null; then
        kill "$pid" 2>/dev/null || true
        ./build/sim "$@" &
        pid=$!
    else
        ninja -C build | grep -E 'error' || true  # the old one stays open until it builds
    fi
done
