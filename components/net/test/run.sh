#!/bin/sh
# Host-side tests of net's cores: what goes when, what becomes of each
# request, and how a live connection is kept. No ESP-IDF needed.
set -e
cd "$(dirname "$0")"
c++ -std=c++20 -Wall -Wextra -I../include -o /tmp/net_test test_net_core.cpp ../net_core.cpp
c++ -std=c++20 -Wall -Wextra -I../include -o /tmp/stream_test test_stream_core.cpp ../stream_core.cpp
/tmp/net_test
exec /tmp/stream_test
