#!/bin/sh
# Host-side parser tests. No ESP-IDF needed.
set -e
cd "$(dirname "$0")"
c++ -std=c++20 -Wall -Wextra -I.. -o /tmp/radar_test -I../include test_radar_parse.cpp ../radar_parse.cpp ../radar_trail.cpp
exec /tmp/radar_test adsb_sample.json adsbdb_route.json adsbdb_aircraft.json \
    planespotters_photo.json planespotters_none.json trace_recent.json
