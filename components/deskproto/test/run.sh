#!/bin/sh
# Host-side protocol tests -- no hardware, no ESP-IDF needed.
set -e
cd "$(dirname "$0")"
c++ -std=c++20 -Wall -Wextra -I../include -o /tmp/deskproto_test test_deskproto.cpp
exec /tmp/deskproto_test
