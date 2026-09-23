#!/usr/bin/env python3
"""Emit fab-ready BOM and CPL (placement) CSVs from the board.

  make_bom.py <board.kicad_pcb> <outdir> <basename>

kicad-cli has no PCB BOM exporter -- it wants a schematic, and this project has
none -- so properties are read straight off the board. Footprints flagged
exclude_from_bom (test pads, mounting holes) are skipped in both files.

Columns match JLCPCB's own sample workbooks exactly: the BOM is four columns and
the CPL writes coordinates with an explicit "mm" suffix. Extra columns or bare
numbers are what usually gets an upload rejected. MPNs are kept in a separate
reference file that you do NOT upload.

CPL coordinates: KiCad's own position export uses Y-up with the origin at the
board's TOP-left, so every Y comes out negative. Fabs expect the origin at the
BOTTOM-left, so Y is shifted by the board height here and comes out positive.
"""
import csv, re, sys
from collections import defaultdict

pcb, outdir, base = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(pcb).read()

ys = [float(m) for m in re.findall(r'\(gr_line\s+\(start [\d.-]+ ([\d.-]+)\)', s)]
ys += [float(m) for m in re.findall(r'\(gr_line\s+\(start [\d.-]+ [\d.-]+\)\s+\(end [\d.-]+ ([\d.-]+)\)', s)]
H = max(ys) if ys else 0.0

parts = []
for m in re.finditer(r'\(footprint "([^"]+)"(.*?)\n\t\)\n', s, re.S):
    fpname, body = m.group(1), m.group(2)
    if "exclude_from_bom" in body:
        continue
    def prop(n):
        g = re.search(r'\(property "%s" "([^"]*)"' % n, body)
        return g.group(1) if g else ""
    ref = prop("Reference")
    if not ref:
        continue
    at = re.search(r'\n\t\t\(at ([\d.-]+) ([\d.-]+)(?: ([\d.-]+))?\)', body)
    x, y, rot = float(at.group(1)), float(at.group(2)), float(at.group(3) or 0)
    side = "Bottom" if re.search(r'\n\t\t\(layer "B\.Cu"\)', body) else "Top"
    parts.append(dict(ref=ref, val=prop("Value"), fp=fpname, mpn=prop("MPN"),
                      lcsc=prop("LCSC"),
                      x=x, y=y, rot=rot % 360, side=side,
                      hand=prop("Assembly") == "Hand"))

def refkey(r):
    m = re.match(r"([A-Za-z]+)(\d+)", r)
    return (m.group(1), int(m.group(2)), r) if m else (r, 0, r)

parts.sort(key=lambda p: refkey(p["ref"]))

# Split: the assembler only ever sees what it can actually place.
smt  = [p for p in parts if not p["hand"]]
hand = [p for p in parts if p["hand"]]

groups = defaultdict(list)
for p in smt:
    groups[(p["val"], p["fp"], p["mpn"], p["lcsc"])].append(p["ref"])
hand_groups = defaultdict(list)
for p in hand:
    hand_groups[(p["val"], p["fp"], p["mpn"], p["lcsc"])].append(p["ref"])

ordered = sorted(groups.items(), key=lambda kv: refkey(sorted(kv[1], key=refkey)[0]))

bom = f"{outdir}/{base}-bom.csv"
with open(bom, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Comment", "Designator", "Footprint", "JLCPCB Part #（optional）"])
    for (val, fp, mpn, lcsc), refs in ordered:
        w.writerow([val, ",".join(sorted(refs, key=refkey)), fp, lcsc])

# Our own record of what each line item actually is. Not for upload.
ref_csv = f"{outdir}/{base}-hand-solder.csv"
with open(ref_csv, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Designator", "Qty", "Value", "LCSC", "MPN", "KiCad footprint"])
    for (val, fp, mpn, lcsc), refs in sorted(hand_groups.items(), key=lambda kv: refkey(sorted(kv[1], key=refkey)[0])):
        w.writerow([",".join(sorted(refs, key=refkey)), len(refs), val, lcsc, mpn, fp])

cpl = f"{outdir}/{base}-cpl.csv"
with open(cpl, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
    for p in smt:
        w.writerow([p["ref"], f'{p["x"]:.4f}mm', f'{H - p["y"]:.4f}mm', p["side"], f'{round(p["rot"]):d}'])

print(f"  {bom}: {len(groups)} line items, {len(smt)} parts to be assembled")
print(f"  {cpl}: {len(smt)} placements (origin bottom-left, board height {H:g} mm)")
print(f"  {ref_csv}: {len(hand)} parts you fit yourself -- do not upload")
