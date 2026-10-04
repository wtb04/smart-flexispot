#!/bin/sh
# Sends a running simulator what Home Assistant, or a laptop's Claude Code,
# would send the panel:
#   sim/send.sh notify '{"title":"Hello","message":"From the desk","level":"warning","timeout_s":0}'
#   sim/send.sh notify 'Plain text works too'
#   sim/send.sh claude '{"session":"s1","machine":"MbP","project":"smart-flexispot","event":"UserPromptSubmit"}'
# On localhost only: nothing leaves this Mac.
set -e
if { [ "$1" != "notify" ] && [ "$1" != "claude" ]; } || [ -z "$2" ]; then
    sed -n '2,7p' "$0"
    exit 1
fi
printf '%s %s' "$1" "$2" | nc -u -w 1 127.0.0.1 47311
