#!/bin/sh
# Host-side tests for the favourites and the Jellyfin segments. Needs IDF_PATH only for its bundled cJSON.
set -e
cd "$(dirname "$0")"
IDF="${IDF_PATH:-$HOME/esp/esp-idf}"
CJSON="$IDF/components/json/cJSON"
cc -w -std=c11 -I"$CJSON" -c "$CJSON/cJSON.c" -o /tmp/cjson_room.o
c++ -std=c++20 -Wall -Wextra -I"$CJSON" -o /tmp/picks_test test_picks.cpp ../picks.cpp /tmp/cjson_room.o
c++ -std=c++20 -Wall -Wextra -I"$CJSON" -o /tmp/segments_test test_segments.cpp ../segments.cpp /tmp/cjson_room.o
/tmp/picks_test
echo
exec /tmp/segments_test
