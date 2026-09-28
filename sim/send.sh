#!/bin/sh
# Sends a running simulator what Home Assistant would send the panel:
#   sim/send.sh notify '{"title":"Hello","message":"From the desk","level":"warning","timeout_s":0}'
#   sim/send.sh notify 'Plain text works too'
# On localhost only: nothing leaves this Mac.
set -e
if [ "$1" != "notify" ] || [ -z "$2" ]; then
    sed -n '2,5p' "$0"
    exit 1
fi
printf 'notify %s' "$2" | nc -u -w 1 127.0.0.1 47311
