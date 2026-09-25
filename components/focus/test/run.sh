#!/bin/sh
# Host-side tests for the timer's rules. No ESP-IDF needed.
set -e
cd "$(dirname "$0")"
c++ -std=c++20 -Wall -Wextra -I../include -I../../units/include -o /tmp/focus_test test_focus.cpp
exec /tmp/focus_test
