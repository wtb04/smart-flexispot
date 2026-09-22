#!/bin/sh
# Host-side tests for the backup clock's arithmetic. No ESP-IDF needed.
set -e
cd "$(dirname "$0")"
c++ -std=c++20 -Wall -Wextra -I.. -o /tmp/rtc_test test_clock_math.cpp
exec /tmp/rtc_test
