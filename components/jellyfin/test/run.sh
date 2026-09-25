#!/bin/sh
# Host-side protocol tests. Needs IDF_PATH only for its bundled cJSON.
set -e
cd "$(dirname "$0")"
IDF="${IDF_PATH:-$HOME/esp/esp-idf}"
CJSON="$IDF/components/json/cJSON"
cc -w -std=c11 -I"$CJSON" -c "$CJSON/cJSON.c" -o /tmp/cjson_jellyfin.o
c++ -std=c++20 -Wall -Wextra -I../include -I"$CJSON" -o /tmp/jellyfin_test \
    test_jellyfin_protocol.cpp ../jellyfin_protocol.cpp /tmp/cjson_jellyfin.o
exec /tmp/jellyfin_test
