#!/bin/sh
# Host-side tests of net's core: what goes when, and what becomes of each
# request. No ESP-IDF needed.
set -e
cd "$(dirname "$0")"
c++ -std=c++20 -Wall -Wextra -I../include -o /tmp/net_test test_net_core.cpp ../net_core.cpp
exec /tmp/net_test
