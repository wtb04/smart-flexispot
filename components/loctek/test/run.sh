#!/bin/sh
# Host-side protocol tests -- no hardware, no ESP-IDF needed.
set -e
cd "$(dirname "$0")"
c++ -std=c++20 -Wall -Wextra -I../include -I../../units/include -o /tmp/loctek_test test_loctek_proto.cpp ../loctek_proto.cpp
exec /tmp/loctek_test
