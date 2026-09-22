#!/usr/bin/env bash
# Every logging tag in the tree must be named in one of diagnostics.cpp's tag
# sets. An unnamed tag still reaches the System channel, but it lands there by
# accident rather than because somebody decided that is where it belongs.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 tools/check_log_tags.py
