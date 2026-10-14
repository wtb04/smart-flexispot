#!/bin/sh
# Desk Link's state for a Wayle custom module in watch mode: a line now and one per change.
status="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/desklink/status.json"
show() { cat "$status" 2>/dev/null || echo '{"alt":"off","text":"","tooltip":"Desk Link is not running"}'; }
mkdir -p "$(dirname "$status")"
show
inotifywait -m -q -e moved_to,close_write --format '%f' "$(dirname "$status")" | while read -r name; do
    [ "$name" = status.json ] && show
done
