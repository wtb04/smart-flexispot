#!/usr/bin/env python3
"""Tie each PCB footprint to its schematic symbol.

KiCad matches board to schematic by a UUID path stored on the footprint. Without
it the two files are strangers and `--schematic-parity` cannot check anything --
which is why this project had no parity check for its whole life.

Run with KiCad's bundled python, after finish.py:
  python3 link_sch.py <board.kicad_pcb> <sheet.kicad_sch>
"""
import re, sys, pcbnew

pcb = sys.argv[1] if len(sys.argv) > 1 else "tab5-desk-ctrl.kicad_pcb"
sch = sys.argv[2] if len(sys.argv) > 2 else "tab5-desk-ctrl.kicad_sch"

# ref -> symbol uuid, read from the schematic's instance table
text = open(sch).read()
ref_uuid = {}
for m in re.finditer(r'\(path "/([0-9a-f-]+)"\s*\n?\s*\(reference "([^"]+)"', text):
    ref_uuid[m.group(2)] = m.group(1)

b = pcbnew.LoadBoard(pcb)
linked, missing = 0, []
for fp in b.GetFootprints():
    ref = fp.GetReference()
    u = ref_uuid.get(ref)
    if u:
        fp.SetPath(pcbnew.KIID_PATH("/" + u))
        linked += 1
    else:
        missing.append(ref)
b.Save(pcb)
print(f"linked {linked} footprints to schematic symbols")
if missing:
    print("  no schematic symbol for:", ", ".join(sorted(missing)))
