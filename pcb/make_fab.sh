#!/bin/bash
# Build a fab-neutral manufacturing package: Gerbers, drill files, BOM, centroid.
# Works with any fab. AISLER can also just take the .kicad_pcb directly.
set -euo pipefail
cd "$(dirname "$0")"

KC=${KICAD_CLI:-/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli}
PCB=${1:-tab5-desk-ctrl.kicad_pcb}
NAME=$(basename "$PCB" .kicad_pcb)
OUT=fab
LAYERS=F.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts

rm -rf "$OUT" && mkdir -p "$OUT/gerbers"

# --use-drill-file-origin / --drill-origin plot: plot everything from the
# board's bottom-left corner, which is the frame the CPL is written in.
# Without it the gerbers sit at negative Y and the assembler thinks every
# part is offset by one board height.
"$KC" pcb export gerbers --layers "$LAYERS" --no-protel-ext \
      --use-drill-file-origin -o "$OUT/gerbers" "$PCB"
"$KC" pcb export drill --format excellon --excellon-separate-th \
      --drill-origin plot \
      --generate-map --map-format gerberx2 -o "$OUT/gerbers/" "$PCB"
# BOM + CPL in the column layout fabs ask for ("BOM and CPL file")
python3 make_bom.py "$PCB" "$OUT" "$NAME"

# Readability of the schematic is not something ERC checks, so gate on it here.
# Set SCH_PY to a python with kiutils; skipped if unset.
if [ -n "${SCH_PY:-}" ]; then
  "$SCH_PY" check_sch.py "${NAME}.kicad_sch" | sed 's/^/  /'
fi

# schematic PDF, so the circuit is readable without opening KiCad
"$KC" sch export pdf -o "$OUT/${NAME}-schematic.pdf" "${NAME}.kicad_sch" 2>/dev/null || true

# KiCad-native position file as well, for anything that wants it
"$KC" pcb export pos --format csv --units mm --side both -o "$OUT/${NAME}-pos-kicad.csv" "$PCB"

# 1:1 print for offering the RJ45 up to the footprint before committing
"$KC" pcb export pdf --layers F.Cu,Edge.Cuts,F.SilkS --mode-single \
      --include-border-title -o "$OUT/${NAME}-check-1to1.pdf" "$PCB"

( cd "$OUT/gerbers" && zip -q -r "../${NAME}-gerbers.zip" . )
echo
echo "package in $OUT/:"
ls -1 "$OUT" "$OUT/gerbers" | sed 's/^/  /'
