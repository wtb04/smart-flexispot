#!/bin/sh
# The board's pictures in img/, from KiCad's 3D view on an opaque background.
# Angles are positive: kicad-cli reads "-50" as an option.
cd "$(dirname "$0")" || exit 1
KC=${KICAD_CLI:-/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli}
render() {  # name, then the view
    name=$1
    shift
    "$KC" pcb render tab5-desk-ctrl.kicad_pcb -o "img/$name.png" -w 1600 -h 1000 --quality high \
        --background opaque --floor --light-top 0.45 --light-camera 0.25 --light-side 0.35 "$@" >/dev/null &&
        echo "img/$name.png"
}
render board-top --side top
render board-bottom --side bottom
render board-3d --rotate 310,0,35
render board-front --rotate 305,0,110
render board-back --rotate 310,0,215
