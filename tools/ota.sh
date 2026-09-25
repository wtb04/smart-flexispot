#!/bin/sh
# Builds and sends firmware over the air: the panel's, the companion's (which
# the panel passes on over Bluetooth), or both, companion first.
#
#   tools/ota.sh panel|companion|both [host]
#
# host defaults to smart-flexispot, the name the panel gives the router; the
# panel's address from Setup, Diagnostics works too. The key comes from
# components/ota/include/ota_secrets.h.
set -eu
cd "$(dirname "$0")/.."

what=${1:-}
host=${2:-smart-flexispot}
secrets=components/ota/include/ota_secrets.h
key=$(sed -n 's/^#define OTA_KEY "\(.*\)"/\1/p' "$secrets" 2>/dev/null || true)
[ -n "$key" ] || { echo "no OTA_KEY in $secrets" >&2; exit 1; }

. "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null 2>&1

send() {  # route, image
    echo "sending $2 to $host"
    curl --fail-with-body --silent --show-error --max-time 300 \
        -H "X-Update-Key: $key" --data-binary @"$2" "http://$host/update/$1"
}

case "$what" in
    panel | companion | both) ;;
    *) echo "usage: $0 panel|companion|both [host]" >&2; exit 2 ;;
esac

echo "running now: $(curl --silent --max-time 5 "http://$host/version" || echo "no answer from $host")"
if [ "$what" != panel ]; then
    (cd proxy && idf.py build >/dev/null)
    send companion proxy/build/desk_companion.bin
fi
if [ "$what" != companion ]; then
    idf.py build >/dev/null
    send panel build/smart_flexispot.bin
fi
