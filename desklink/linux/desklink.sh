#!/bin/sh
# gtk4-layer-shell has to load before libwayland, which Python cannot arrange itself.
export LD_PRELOAD="${LD_PRELOAD:+$LD_PRELOAD:}libgtk4-layer-shell.so.0"
export PYTHONPATH="@SHARE@${PYTHONPATH:+:$PYTHONPATH}"
exec python3 -m desklink "$@"
