#!/usr/bin/env python3
"""Report overlapping text in the schematic.

ERC does not care what collides visually, so a readable drawing has to be
checked separately. This compares every visible text item -- block titles,
notes, net labels, and symbol reference/value fields -- against each other and
against symbol bodies and wires, and prints anything that intersects.

Run it after gen_sch.py:
  <python-with-kiutils> check_sch.py tab5-desk-ctrl.kicad_sch

Exits non-zero if anything overlaps, so it can gate make_fab.sh.
"""
import math, sys
from kiutils.schematic import Schematic

f = sys.argv[1] if len(sys.argv) > 1 else "tab5-desk-ctrl.kicad_sch"
sch = Schematic.from_file(f)
lib = {f"{s.libraryNickname}:{s.entryName}": s for s in sch.libSymbols}

# Text advance is about 0.95 of the font height per character in KiCad's stroke
# font, and the cap-to-descender band about 1.3 -- close enough to catch real
# collisions without flagging every near miss.
ADV, LINE = 0.95, 1.3

def unit_points(u):
    p = []
    for g in u.graphicItems:
        for attr in ("start", "end", "center", "position"):
            q = getattr(g, attr, None)
            if q is not None: p.append((q.X, q.Y))
        r, c = getattr(g, "radius", None), getattr(g, "center", None)
        if r and c: p += [(c.X - r, c.Y - r), (c.X + r, c.Y + r)]
        for q in (getattr(g, "points", None) or []): p.append((q.X, q.Y))
    p += [(q.position.X, q.position.Y) for q in u.pins]
    return p

def sym_bbox(inst):
    s = lib.get(f"{inst.libraryNickname}:{inst.entryName}")
    if not s: return None
    pts = [q for u in s.units for q in unit_points(u)]
    if not pts: return None
    a, out = math.radians(inst.position.angle or 0), []
    for px, py in pts:
        if inst.mirror == "y": px = -px
        if inst.mirror == "x": py = -py
        out.append((inst.position.X + px*math.cos(a) - py*math.sin(a),
                    inst.position.Y - px*math.sin(a) - py*math.cos(a)))
    xs, ys = [q[0] for q in out], [q[1] for q in out]
    return (inst.properties[0].value, min(xs), max(xs), min(ys), max(ys))

boxes = [b for b in (sym_bbox(i) for i in sch.schematicSymbols)
         if b and not b[0].startswith("#")]
wires = [(g.points[0].X, g.points[0].Y, g.points[1].X, g.points[1].Y)
         for g in sch.graphicalItems if getattr(g, "type", None) == "wire"]

texts = []
for t in sch.texts:
    sz = t.effects.font.height
    lj = bool(t.effects.justify and t.effects.justify.horizontally == "left")
    w, h = len(t.text)*sz*ADV, sz*LINE
    x, y = t.position.X, t.position.Y
    texts.append(("note", t.text, x if lj else x-w/2, x+w if lj else x+w/2, y-h/2, y+h/2))
for g in sch.globalLabels:
    w, h = len(g.text)*1.27*ADV + 3.2, 1.7          # +3.2 for the label's arrow
    x, y, a = g.position.X, g.position.Y, g.position.angle or 0
    texts.append(("label", g.text, *((x, x+w, y-h/2, y+h/2) if a == 0 else
                                     (x-w, x, y-h/2, y+h/2) if a == 180 else
                                     (x-h/2, x+h/2, y-w, y) if a == 90 else
                                     (x-h/2, x+h/2, y, y+w))))
for inst in sch.schematicSymbols:
    for pr in inst.properties:
        if pr.key not in ("Reference", "Value") or not pr.value: continue
        if pr.effects and pr.effects.hide: continue
        if pr.value.startswith("#"): continue
        sz = pr.effects.font.height
        w, h = len(pr.value)*sz*ADV, sz*LINE
        if (pr.position.angle or 0) in (90, 270): w, h = h, w
        texts.append(("field", pr.value, pr.position.X-w/2, pr.position.X+w/2,
                      pr.position.Y-h/2, pr.position.Y+h/2))

def ov(a0, a1, b0, b1): return not (a1 <= b0 or b1 <= a0)
hits = []
for i, a in enumerate(texts):
    for b in texts[i+1:]:
        if ov(a[2], a[3], b[2], b[3]) and ov(a[4], a[5], b[4], b[5]):
            hits.append(f"{a[0]} {a[1]!r} overlaps {b[0]} {b[1]!r}")
    for bx in boxes:
        if ov(a[2], a[3], bx[1], bx[2]) and ov(a[4], a[5], bx[3], bx[4]):
            hits.append(f"{a[0]} {a[1]!r} sits on symbol {bx[0]}")
    for x1, y1, x2, y2 in wires:
        if ov(a[2], a[3], min(x1,x2), max(x1,x2)) and ov(a[4], a[5], min(y1,y2), max(y1,y2)):
            hits.append(f"{a[0]} {a[1]!r} sits on a wire at ({x1:.1f},{y1:.1f})-({x2:.1f},{y2:.1f})")

for h in hits: print("  " + h)
print(f"{len(texts)} text items vs {len(boxes)} symbols and {len(wires)} wires: "
      f"{len(hits)} overlap(s)")
sys.exit(1 if hits else 0)
