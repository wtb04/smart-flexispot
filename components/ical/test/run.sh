#!/bin/sh
# Host-side tests for the iCalendar reader. No ESP-IDF needed.
set -e
cd "$(dirname "$0")"
c++ -std=c++20 -Wall -Wextra -I.. -I../../rtc/include -o /tmp/ical_test test_ical_parse.cpp
exec /tmp/ical_test
