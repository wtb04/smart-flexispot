#!/bin/sh
# Host-side tests of which job runs when. No ESP-IDF needed.
set -e
cd "$(dirname "$0")"
c++ -std=c++20 -Wall -Wextra -I../include -o /tmp/jobs_test test_job_core.cpp ../job_core.cpp
exec /tmp/jobs_test
