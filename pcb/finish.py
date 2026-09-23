#!/usr/bin/env python3
"""Post-process the routed board: place silkscreen reference designators, fill zones.

kiutils writes every reference field at the footprint origin, i.e. straight on
top of the part's own pads. Positioning them here, with KiCad's own API, is both
simpler and authoritative -- pcbnew is what finally writes the file.

Run with KiCad's bundled python:
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 finish.py <board>
"""
import re, sys, pcbnew

# Both silk bands outboard of the sockets are full of DevKit pin names now, so
# U1A/U1B sit in the only gap left: between the test-pad labels and pin 1.
REF_POS = {
    "RJ45_1": (11.0, 29.0), "F1": (37.0, 22.9), "D1": (44.0, 22.1),
    "C1": (52.0, 17.0), "C2": (48.0, 19.1), "D2": (48.0, 12.5),
    "R1": (42.0, 8.7), "R2": (42.0, 17.1), "R3": (47.0, 8.7), "R4": (36.0, 8.7),
    "R5": (18.2, 14.4), "R6": (18.2, 20.6),   # in the sliver, either side of DESK
    "U1A": (18.6, 2.75), "U1B": (18.6, 32.45),
    "H1": (9.5, 3.2), "H2": (9.5, 32.0), "H3": (50.5, 10.7), "H4": (50.5, 24.7),
}

f = sys.argv[1] if len(sys.argv) > 1 else "tab5-desk-ctrl.kicad_pcb"
b = pcbnew.LoadBoard(f)

# kiutils writes "(remove_unused_layers yes)" whatever value it is given, which
# lets KiCad strip the annular ring off every through-hole pad on any layer the
# net does not actually use. On this board that left 41 of the 44 THT pads with
# copper on one side only, and the unconnected ones with none at all.
#
# Two traps here, both found the hard way:
#   * SetRemoveUnconnected(False) is a deprecated shim in KiCad 9 and does not
#     stick. The live setting is the padstack's unconnected-layer mode.
#   * Writing "(remove_unused_layers no)" into the file does NOT disable it --
#     KiCad reads the token's presence, not its argument, so the "corrected"
#     file still plotted stripped pads. Only KEEP_ALL below actually works.
# Verified by reading the plotted gerbers back, not by trusting the setting.
for fp in b.GetFootprints():
    for p in fp.Pads():
        p.SetUnconnectedLayerMode(pcbnew.PADSTACK.UNCONNECTED_LAYER_MODE_KEEP_ALL)

for fp in b.GetFootprints():
    ref = fp.GetReference()
    t = fp.Reference()
    if ref in REF_POS:
        x, y = REF_POS[ref]
        t.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
    t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(0.8), pcbnew.FromMM(0.8)))
    t.SetTextThickness(pcbnew.FromMM(0.15))   # JLC silk minimum
    t.SetTextAngle(pcbnew.EDA_ANGLE(0, pcbnew.DEGREES_T))
    # test points are labelled by signal name; mounting holes are not parts and
    # do not need a designator cluttering a board this dense
    t.SetVisible(not (ref.startswith("TP") or ref.startswith("H")))
    fp.Value().SetVisible(False)
    # KiCad's stock footprints draw their silk outlines at 0.12 mm, under
    # JLCPCB's 0.15 mm minimum. Lift every silk line in every footprint.
    for g in fp.GraphicalItems():
        if g.GetLayer() in (pcbnew.F_SilkS, pcbnew.B_SilkS) and hasattr(g, "SetWidth"):
            if g.GetWidth() < pcbnew.FromMM(0.15):
                g.SetWidth(pcbnew.FromMM(0.15))

# Two 19-pin through-hole rows perforate B.Cu almost end to end, so the pour
# fragments and thermal-relief spokes can end up feeding an orphaned island.
# Solid pad connections keep the copper contiguous. Trade-off: through-hole GND
# pads sink more heat, so use a decent iron on J1 and the headers.
for z in b.Zones():
    if z.GetNetname() == "GND" and not z.GetIsRuleArea():
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)

# the RJ45's 3D model: KiCad's 3D pack ships only three RJ45 models and this is
# not one of them, so the real model was pulled from LCSC (easyeda2kicad) and
# lives in the project. ${KIPRJMOD} keeps the reference project-relative.
MODEL = "${KIPRJMOD}/3dmodels/lcsc.3dshapes/RJ45-TH_R-RJ45R08P-C000.wrl"
for fp in b.GetFootprints():
    if fp.GetReference() != "RJ45_1":
        continue
    models = fp.Models()
    while len(models):
        models.pop()
    m = pcbnew.FP_3DMODEL()
    m.m_Filename = MODEL
    m.m_Show = True
    # The LCSC model is aligned to easyeda2kicad's own footprint, which numbers the
    # pins in the opposite direction from KiCad's Connfly footprint -- easyeda pad 1
    # is at (3.57, 3.30) and pad 8 at (-3.57, 1.51), KiCad's are (0, 0) and
    # (7.14, 1.78). The two frames differ by a 180 deg spin about Z plus a shift, so
    # the model needs both: easyeda's origin lands at (3.57, 3.30) in KiCad's frame,
    # and 3D space has Y inverted relative to the footprint.
    m.m_Offset = pcbnew.VECTOR3D(3.57, -3.30, 0.0)
    m.m_Rotation = pcbnew.VECTOR3D(0.0, 0.0, 180.0)
    fp.Models().push_back(m)
    print(f"  3D model attached to RJ45_1")

# Put the drill/place origin on the board's bottom-left corner. KiCad's default
# is the page origin, which plots the board at NEGATIVE gerber Y while the CPL
# is written Y-up from the board's bottom edge -- so every part reads as one
# board-height above the outline and the fab asks whether to "auto-align" it.
# With the origin here, and make_fab.sh exporting from it, gerbers and CPL share
# one frame and agree exactly.
# Anchor on the outline's CENTRELINE, not GetBoardEdgesBoundingBox(), which
# includes half the edge line width and would leave gerbers and CPL 0.05 mm
# apart -- harmless but needless.
_pts = []
for _d in b.GetDrawings():
    if _d.GetLayer() != pcbnew.Edge_Cuts:
        continue
    _pts += [_d.GetStart(), _d.GetEnd()]
    if _d.GetShape() == pcbnew.SHAPE_T_ARC:
        _pts.append(_d.GetArcMid())
_x0 = min(q.x for q in _pts)
_y1 = max(q.y for q in _pts)
b.GetDesignSettings().SetAuxOrigin(pcbnew.VECTOR2I(_x0, _y1))
print(f"  drill/place origin -> outline bottom-left ({_x0/1e6:.2f}, {_y1/1e6:.2f})")

ok = pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(f)
print(f"placed {len(REF_POS)} refs, filled {len(list(b.Zones()))} zones (ok={ok})")

# Board stackup: black soldermask, white silk. Not reachable through the Python
# API, so it goes in as text after pcbnew has written the file. This is what the
# 3D viewer colours the board from.
STACKUP = """\t\t(stackup
\t\t\t(layer "F.SilkS" (type "Top Silk Screen") (color "White"))
\t\t\t(layer "F.Paste" (type "Top Solder Paste"))
\t\t\t(layer "F.Mask" (type "Top Solder Mask") (color "Black") (thickness 0.01))
\t\t\t(layer "F.Cu" (type "copper") (thickness 0.035))
\t\t\t(layer "dielectric 1" (type "core") (thickness 1.51) (material "FR4") (epsilon_r 4.5) (loss_tangent 0.02))
\t\t\t(layer "B.Cu" (type "copper") (thickness 0.035))
\t\t\t(layer "B.Mask" (type "Bottom Solder Mask") (color "Black") (thickness 0.01))
\t\t\t(layer "B.Paste" (type "Bottom Solder Paste"))
\t\t\t(layer "B.SilkS" (type "Bottom Silk Screen") (color "White"))
\t\t\t(copper_finish "ENIG")
\t\t\t(dielectric_constraints no)
\t\t)
"""
src = open(f).read()

# Belt and braces: if anything ever writes the token back, strip it entirely.
# Setting it to "no" is not enough -- KiCad honours the token's presence.
n_ru = src.count("(remove_unused_layers yes)") + src.count("(remove_unused_layers no)")
src = re.sub(r"\n\s*\(remove_unused_layers (?:yes|no)\)", "", src)
if n_ru:
    print(f"  stripped remove_unused_layers from {n_ru} pads")
open(f, "w").write(src)

if "(stackup" not in src:
    i = src.index("\t(setup\n") + len("\t(setup\n")
    src = src[:i] + STACKUP + src[i:]
    open(f, "w").write(src)
    print("  stackup added: black soldermask, white silk")
else:
    print("  stackup already present")
