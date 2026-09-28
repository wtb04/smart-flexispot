#!/bin/sh
# Host-side tests for the charge gauge. No ESP-IDF needed.
set -e
cd "$(dirname "$0")"
c++ -std=c++20 -Wall -Wextra -I../include -o /tmp/gauge_test test_gauge.cpp
exec /tmp/gauge_test
