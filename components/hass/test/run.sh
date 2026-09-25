#!/bin/sh
# Host-side protocol tests. Needs IDF_PATH only for its bundled cJSON.
set -e
cd "$(dirname "$0")"
IDF="${IDF_PATH:-$HOME/esp/esp-idf}"
CJSON="$IDF/components/json/cJSON"
# cJSON is third-party C: compile it as C and without warnings of its own.
cc -w -std=c11 -I"$CJSON" -c "$CJSON/cJSON.c" -o /tmp/cjson.o

c++ -std=c++20 -Wall -Wextra -I../include -I../../deskproto/include -I../../units/include -I"$CJSON" \
    -o /tmp/hass_test test_hass_protocol.cpp ../hass_protocol.cpp /tmp/cjson.o
c++ -std=c++20 -Wall -Wextra -I../include -I../../deskproto/include -I../../units/include -I"$CJSON" \
    -o /tmp/ha_ws_test test_ha_ws_protocol.cpp ../ha_ws_protocol.cpp /tmp/cjson.o

/tmp/hass_test
echo
exec /tmp/ha_ws_test
