#!/bin/sh
# Host-side tests for the travel backend's answers. No ESP-IDF needed.
set -e
cd "$(dirname "$0")"
c++ -std=c++20 -Wall -Wextra -I.. -o /tmp/travel_test test_travel_parse.cpp
exec /tmp/travel_test
