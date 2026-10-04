#!/usr/bin/env python3
"""Generate tab5-desk-ctrl.kicad_pcb : FlexiSpot / LoctekMotion BLE desk companion.

rev1 -- Wouter ten Brinke.
An ESP32 DevKit V1 socketed onto a carrier that speaks the control box's 5 V
UART over RJ45, driven over BLE by the Tab5 panel.

Placement only -- routing is done by route.py, DRC by kicad-cli.

Module assumptions (verify against your physical board):
  PINS_PER_ROW = 15          # measured: 30-pin board, 2x15
  ROW_SPACING  = 25.4 mm  (1.0 in)
  pin 1 is at the antenna end, pin numbering runs toward the USB end.
"""
import copy, math, os
from kiutils.board import Board
from kiutils.footprint import Footprint, Attributes
from kiutils.items.common import Position, Net
from kiutils.items.gritems import GrLine, GrArc, GrText
from kiutils.items.zones import Zone, ZonePolygon, Hatch, KeepoutSettings, FillSettings

LIB = os.environ.get("KICAD9_FOOTPRINT_DIR", "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints")
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "tab5-desk-ctrl.kicad_pcb")

W, H = 60.0, 35.2
PINS_PER_ROW = 15          # measured: 30-pin board, 2x15
PITCH = 2.54
ROW_SPACING = 25.4        # 1.0 in -- confirmed: on a breadboard the module
                          # leaves one column free on one side only, which is
                          # only true at 1.0 in (0.9 leaves one each side, 1.1 none)
# Module runs along the board with pin 1 (the antenna end) at the LEFT, pointing
# inboard, so that pin 15 -- the USB end -- reaches the RIGHT board edge and stays
# accessible. Headers are at rot 90, so pin n sits at PIN1_X + (n-1)*PITCH.
# The RF cost of this is real and accepted.
PIN1_X = 21.5
# Row inset is set by MOUNTING, not by the silk. The RJ45's courtyard reaches
# +/- 10.70 about the module axis and an M3 needs 3.45 of courtyard plus a
# margin, so the pocket above the jack only becomes usable once the axis sits at
# y >= 18.4 -- which is what puts the rows here and makes H 35.2.
ROW_A_Y = 4.9
ROW_B_Y = ROW_A_Y + ROW_SPACING          # 30.3, module centred on y = 17.6
PINEND_X = PIN1_X + (PINS_PER_ROW - 1) * PITCH  # 57.06, USB end at the right edge
HDR = f"PinSocket_1x{PINS_PER_ROW}_P{PITCH}mm_Vertical"   # female: the devboard slots in

# RJ45_1 faces out of the LEFT board edge, on the module's centreline.
#
# Which way the jack opens is NOT the LED-pad side -- that was wrong until the
# 3D model was checked. The LED through-holes sit at the BACK of the housing
# (local y = +6.6) and their leads run forward inside it to the lenses in the
# face. Both the 3D model (lenses at local y = -7.9, high up) and KiCad's own
# F.Fab outline (y -7.94 .. +7.81) put the opening at local **-y**.
#
# At rot 90 the footprint maps (px,py) -> (py, -px), so local -y becomes the
# leftmost point: origin at x = 7.92 puts the jack face flush with x = 0.
# y = 21.17 centres the 8-pin array (local x 0..7.14) on the module axis 17.6.
RJ_X, RJ_Y = 7.92, 21.17

# The antenna now sits over the board, so this keepout is load-bearing rather
# than belt-and-braces: it holds copper out from under the antenna end.
KEEPOUT = (19.5, 7.1, 33.0, 28.4)

NETS = ["", "GND", "+5V", "DESK_5V", "D5V_F", "DESK_TX", "LED_BT", "LED_BT_A",
        "LED_DESK", "LED_DESK_A",
        "RX_DIV", "DESK_RX", "ESP_TX", "PIN20", "ESP_P20", "+3V3"]
NETIDX = {n: i for i, n in enumerate(NETS)}

# DOIT ESP32 DevKit V1, 30-pin. Read off the module itself (photo), not assumed.
# The module's silkscreen has USB on the LEFT; our board has USB on the RIGHT, so
# the module is rotated 180 deg in-plane -- that reverses each row AND swaps which
# row is which. Socket pin 1 = LEFT (antenna) end, pin 15 = RIGHT (USB) end:
#   row A (y=4.0)  D23 D22 TX0 RX0 D21 D19 D18  D5 TX2 RX2  D4  D2 D15 GND 3V3
#   row B (y=29.4)  EN  VP  VN D34 D35 D32 D33 D25 D26 D27 D14 D12 D13 GND VIN
#
# RJ45 direction, measured on a real control box. The published LoctekMotion
# tables have pins 5 and 6 the other way round, written from the keypad's side.
# On this board:
#   pin 6 = control box TX, 5 V  -> R1/R2 divider + BAT54S clamp -> our RX
#   pin 5 = control box RX       <- R3 220R <- our TX
#   pin 4 = wake ("PIN 20")      <- R4 220R <- our GPIO23
#
# Signals land on the module pins whose silkscreen matches their role:
#   ESP_TX  -> row A pin  9 = TX2 = GPIO17
#   RX_DIV  -> row A pin 10 = RX2 = GPIO16
#   ESP_P20 -> row A pin  1 = D23 = GPIO23
#
# The companion's defaults in proxy/sdkconfig.defaults match: TX=17, RX=16, WAKE=23.
U1A_NETS = {"1": "ESP_P20",
            "9": "ESP_TX", "10": "RX_DIV", "14": "GND", "15": "+3V3"}
# LEDs on row B pins 6/7 (D32=GPIO32, D33=GPIO33). Row B is the bottom row, so
# these runs use the empty bottom channel instead of the crowded top one.
U1B_NETS = {"6": "LED_BT", "7": "LED_DESK", "14": "GND", "15": "+5V"}

PARTS = [
    # Chanklement R-RJ45R08P-C000 (LCSC C386757). Footprint verified against the
    # datasheet: signal span 7.14, row offset 1.78, LED spans 13.72 / 9.14,
    # shield 16.26, mounting 12.70, drills 0.9 / 1.8 / 3.25 -- all exact.
    # Pins 9/10 are the left LED, 11/12 the right; + is the lower-numbered pin.
    ("RJ45_1", "Connector_RJ", "RJ45_Connfly_DS1128-09-S8xx-S_Horizontal",
     RJ_X, RJ_Y, 90, "RJ45 8P8C 2xLED",
     {"4": "PIN20", "5": "DESK_RX", "6": "DESK_TX", "7": "GND", "8": "DESK_5V",
      "9": "GND", "10": "LED_DESK_A", "11": "GND", "12": "LED_BT_A", "SH": "GND"}),
    # F1/D1 live under the module (nothing to reach); only the test pads
    # need to stay accessible, so only they sit in the top strip.
    # D1 (SS14) already blocks back-feed from the module toward the desk, so the
    # old JP1 enable jumper was redundant and has been removed.
    ("F1", "Resistor_SMD", "R_1206_3216Metric", 37.0, 24.9, 0, "PTC 500mA",
     {"1": "DESK_5V", "2": "D5V_F"}),
    ("D1", "Diode_SMD", "D_SMA", 44.0, 24.9, 180, "SS14", {"2": "D5V_F", "1": "+5V"}),
    # decoupling next to the module's 5V pin (row B pin 15, VIN, at the USB end)
    # 22uF, not the 100uF inherited from revA: at 5V across it a 10V-rated
    # X5R derates to a fraction of nominal anyway, the devkit carries its own
    # input bulk, and 100uF/1206 is a pricey non-stocked part for no benefit.
    ("C1", "Capacitor_SMD", "C_1206_3216Metric", 52.0, 19.2, 0, "22uF 25V", {"1": "+5V", "2": "GND"}),
    ("C2", "Capacitor_SMD", "C_0805_2012Metric", 48.0, 20.9, 0, "100nF 50V", {"1": "+5V", "2": "GND"}),
    # signal passives under the module, ordered to match their row B pins
    ("R4", "Resistor_SMD", "R_0805_2012Metric", 36.0, 10.9, 0, "220R",
     {"1": "ESP_P20", "2": "PIN20"}),
    ("R3", "Resistor_SMD", "R_0805_2012Metric", 47.0, 10.9, 0, "220R",
     {"1": "ESP_TX", "2": "DESK_RX"}),
    ("R1", "Resistor_SMD", "R_0805_2012Metric", 42.0, 10.9, 0, "10k",
     {"1": "DESK_TX", "2": "RX_DIV"}),
    ("R2", "Resistor_SMD", "R_0805_2012Metric", 42.0, 14.9, 0, "15k",
     {"1": "RX_DIV", "2": "GND"}),
    # LED series limits. The LEDs live inside the jack; these sit near the module
    # so the GPIO runs stay short.
    ("R5", "Resistor_SMD", "R_0805_2012Metric", 18.2, 11.9, 90, "220R",
     {"1": "LED_BT", "2": "LED_BT_A"}),
    ("R6", "Resistor_SMD", "R_0805_2012Metric", 18.2, 22.9, 90, "220R",
     {"1": "LED_DESK", "2": "LED_DESK_A"}),
    ("D2", "Package_TO_SOT_SMD", "SOT-23", 48.0, 14.9, 0, "BAT54S",
     {"1": "GND", "2": "+3V3", "3": "RX_DIV"}),
    ("U1A", "Connector_PinSocket_2.54mm", HDR, PIN1_X, ROW_A_Y, 90, "Socket 1x15 2.54mm", U1A_NETS),
    ("U1B", "Connector_PinSocket_2.54mm", HDR, PIN1_X, ROW_B_Y, 90, "Socket 1x15 2.54mm", U1B_NETS),
    # Four M3, spanning 51.8 x 29.2 -- this board screws to the underside of a desk
# and carries its own load, so the mounting pattern drives the outline rather
# than the other way round. H1/H2 are true corners, in the pockets the jack
# leaves once the test pads move off the left flank. H3/H4 are as far apart as
# the component field allows and sit under the module, so screw all four down
# before plugging the module in. None is in the antenna keepout: a steel screw
# and standoff under the antenna would detune it far worse than copper.
    ("H1", "MountingHole", "MountingHole_3.2mm_M3",  4.2,  3.2, 0, "M3", {}),
    ("H2", "MountingHole", "MountingHole_3.2mm_M3",  4.2, 32.0, 0, "M3", {}),
    ("H3", "MountingHole", "MountingHole_3.2mm_M3", 56.0, 10.7, 0, "M3", {}),
    ("H4", "MountingHole", "MountingHole_3.2mm_M3", 56.0, 24.7, 0, "M3", {}),
]

# all six probe points in one row along the bottom edge, in signal order
# Manufacturer part numbers, so a fab's BOM matcher has something to resolve.
# VERIFY THESE BEFORE ORDERING -- they are the common part for each footprint,
# not parts anyone has confirmed against your build. J1 especially: the KiCad
# footprint name carries a literal wildcard ("54602-x08"), so the variant digit
# is yours to pin down, and it must match the jack you actually buy.
MPN = {
    "RJ45_1": "R-RJ45R08P-C000",  # Ckmtw 8P8C, shielded, 2 LEDs
    "U1A": "1x15 female header 2.54mm", "U1B": "1x15 female header 2.54mm",
    "F1":  "SMD1206P050TF/15", # PTTC PTC, 500 mA hold, 1206
    "D1":  "SS14-E3/61T",      # Vishay Schottky 40 V 1 A, SMA
    "D2":  "BAT54S,215",       # Nexperia dual Schottky, SOT-23
}
# JLCPCB catalogue numbers. Everything except C1 and F1 is JLCPCB's OWN match
# from an earlier upload of this BOM, so those are authoritative for their
# library. C1's value changed from 100uF to 22uF, so its old code no longer
# applies. F1's is from LCSC and is NOT confirmed present in JLCPCB's assembly
# library -- if it comes back unmatched, F1 goes back to hand-soldering.
LCSC = {
    "C2": "C49678",       # 100nF 50V X7R 0805        (Basic)
    "D1": "C2480",        # SS14 SMA                  (Basic)
    "D2": "C7420333",     # BAT54S SOT-23, series     (Extended)
    "R1": "C17414",       # 10k 0805 1%               (Basic)
    # R2 was 20k (C4328); 15k after the box's TX measured 5.01-5.17 V.
    "R2": "C17475",       # 15k 0805 1%  0805W8F1502T5E  (Basic) -- picked at upload
    "C1": "C12891",       # 22uF 25V X5R 1206  CL31A226KAHNNNE (Basic) -- picked at upload
    "R3": "C17557",       # 220R 0805 1%              (Basic)
    "R4": "C17557",
    "F1": "C106264",      # SMD1206P050TF/15: 0.5 A hold, 15 V max (Extended)
    "R5": "C17557", "R6": "C17557",
    "U1A": "C124408", "U1B": "C124408",   # 1x15 female header, 2.54 mm
    "RJ45_1": "C386757",  # Chanklement R-RJ45R08P-C000, from the datasheet supplied
}

# Not orderable parts: bare copper and holes. Keep them out of the BOM entirely.
NO_BOM = {"H1", "H2", "H3", "H4"}

# Fitted by hand, not by the assembler. JLCPCB stocks none of these: the PTC is
# not in their catalogue, a 1x15 socket is an odd length, and the only RJ45 they
# matched was a modular PLUG (male, cable-end) with no stock -- the wrong part.
# Tagged on the board so the BOM/CPL split stays in step with the design.
HAND_SOLDER = {"RJ45_1", "U1A", "U1B"}   # F1 moved to assembly, see LCSC below

# four signal pads hug the jack, where those signals enter; power refs below it
# No test pads. Every net they carried is already on a through-hole pad that is
# exposed on the underside and named on the top silk: GND at U1A.14/U1B.14/
# RJ45_1.7/9/11/SH, +5V at U1B.15, RX_DIV at U1A.10, and DESK_TX/DESK_RX/PIN20
# at RJ45_1 pins 6/5/4. Six more pads bought nothing but board height.
TESTPADS = []


def rot_offset(px, py, deg):
    """KiCad footprint rotation applied to a pad offset (y axis points down)."""
    a = math.radians(deg)
    return (px * math.cos(a) + py * math.sin(a),
            -px * math.sin(a) + py * math.cos(a))


board = Board.create_new()
board.nets = [Net(i, n) for i, n in enumerate(NETS)]
PADS = {}   # (ref, padnum) -> (x, y, w, h, layerset)


def place(ref, lib, fpname, x, y, rot, value, netmap):
    fp = copy.deepcopy(Footprint.from_file(f"{LIB}/{lib}.pretty/{fpname}.kicad_mod"))
    fp.libraryNickname, fp.entryName = lib, fpname
    fp.position = Position(x, y, rot if rot else None)
    props = {"Reference": ref, "Value": value}
    if ref in MPN:
        props["MPN"] = MPN[ref]
    if ref in LCSC:
        props["LCSC"] = LCSC[ref]
    if ref in HAND_SOLDER:
        props["Assembly"] = "Hand"
    fp.properties = props
    if ref in NO_BOM:
        fp.attributes = fp.attributes or Attributes()
        fp.attributes.excludeFromBom = True
        fp.attributes.excludeFromPosFiles = True
    for gi in fp.graphicItems:
        if getattr(gi, "type", None) == "reference":
            gi.text = ref
            if ref in REF_OFF and gi.layer == "F.SilkS":
                dx, dy = REF_OFF[ref]
                if rot:                      # store in footprint-local coords
                    a = math.radians(-rot)
                    dx, dy = (dx * math.cos(a) + dy * math.sin(a),
                              -dx * math.sin(a) + dy * math.cos(a))
                gi.position = Position(dx, dy, rot if rot else None)
                gi.effects.font.width = gi.effects.font.height = 0.7
                gi.effects.font.thickness = 0.12
        elif getattr(gi, "type", None) == "value":
            gi.text = value
            gi.hide = True          # values on silk would collide all over a board this dense
    for pad in fp.pads:
        # kiutils parses "(remove_unused_layers no)" as True -- a round-trip bug
        # that would strip annular rings off the through-hole pads. Force the
        # library's actual value back.
        pad.removeUnusedLayers = False
        pad.keepEndLayers = False
        if pad.number and str(pad.number) in netmap:
            n = netmap[str(pad.number)]
            pad.net = Net(NETIDX[n], n)
        if rot:
            pad.position.angle = (pad.position.angle or 0) + rot
        ox, oy = rot_offset(pad.position.X, pad.position.Y, rot)
        sx, sy = (pad.size.Y, pad.size.X) if rot in (90, 270) else (pad.size.X, pad.size.Y)
        if pad.number:
            PADS[(ref, str(pad.number))] = (x + ox, y + oy, sx, sy, list(pad.layers))
        else:  # NPTH mounting slots inside the RJ45
            PADS[(ref, f"npth{len(PADS)}")] = (x + ox, y + oy, sx, sy, ["*.Cu"])
    board.footprints.append(fp)


for p in PARTS:
    place(*p)
for ref, net, x, y in TESTPADS:
    place(ref, "TestPoint", "TestPoint_Pad_1.5x1.5mm", x, y, 0, net, {"1": net})

# board outline
R = 3.0                       # corner radius
K = R * (1 - 0.70710678)      # arc midpoint offset from the corner
for a, b in [((R, 0), (W - R, 0)), ((W, R), (W, H - R)),
             ((W - R, H), (R, H)), ((0, H - R), (0, R))]:
    board.graphicItems.append(GrLine(start=Position(*a), end=Position(*b),
                                     layer="Edge.Cuts", width=0.1))
for s, m, e in [((0, R), (K, K), (R, 0)),
                ((W - R, 0), (W - K, K), (W, R)),
                ((W, H - R), (W - K, H - K), (W - R, H)),
                ((R, H), (K, H - K), (0, H - R))]:
    board.graphicItems.append(GrArc(start=Position(*s), mid=Position(*m), end=Position(*e),
                                    layer="Edge.Cuts", width=0.1))

# antenna region marked on silk: invisible once the module is on, but it is what
# tells anyone looking at the bare board why that area is deliberately empty
ax0, ay0, ax1, ay1 = KEEPOUT[0] + 0.5, KEEPOUT[1] + 0.5, KEEPOUT[2] - 0.5, KEEPOUT[3] - 0.5
for a, b in [((ax0, ay0), (ax1, ay0)), ((ax1, ay0), (ax1, ay1)),
             ((ax1, ay1), (ax0, ay1)), ((ax0, ay1), (ax0, ay0))]:
    board.graphicItems.append(GrLine(start=Position(*a), end=Position(*b),
                                     layer="F.SilkS", width=0.15))


def hatch(x0, y0, x1, y1, pitch=2.0):
    """45-degree fill, clipped to the box. Lines run x - y = k."""
    k = x0 - y1
    while k <= x1 - y0:
        ya, yb = max(y0, x0 - k), min(y1, x1 - k)
        if yb - ya > 0.2:
            board.graphicItems.append(GrLine(start=Position(round(ya + k, 3), round(ya, 3)),
                                             end=Position(round(yb + k, 3), round(yb, 3)),
                                             layer="F.SilkS", width=0.15))
        k += pitch


# hatched above and below, leaving a clear band for the text
hatch(ax0 + 0.4, ay0 + 0.4, ax1 - 0.4, 14.5)
hatch(ax0 + 0.4, 21.3, ax1 - 0.4, ay1 - 0.4)

# Test points carry their signal name on silk instead of a TPn designator --
# nothing is assembled onto them, so the net name is the useful thing to read
# with a probe in your hand. The TPn references still exist in the file.
# DevKit V1 30-pin names, pin 1 first. Row A is the module's D23..3V3 side and
# row B its EN..VIN side -- confirmed against this board's own net assignment:
# A9=TX2 and A10=RX2 carry ESP_TX/RX_DIV, A14/A15 are GND/3V3, B6=D32 and
# B7=D33 drive the LEDs, B14/B15 are GND/VIN. All eight land where they should.
ROW_A_NAMES = ["D23","D22","TX0","RX0","D21","D19","D18","D5",
               "TX2","RX2","D4","D2","D15","GND","3V3"]
ROW_B_NAMES = ["EN","VP","VN","D34","D35","D32","D33","D25",
               "D26","D27","D14","D12","D13","GND","VIN"]

# The module covers everything between the two rows once it is plugged in, so
# the names go OUTBOARD of each row -- the only place they stay readable.
# BT and LINK sit in the sliver beside the jack, one at each end. Anchor them
# to the jack's own silk outline (footprint F.SilkS spans local x -4.99..11.88,
# which at rot 90 becomes board y = RJ_Y - 11.88 .. RJ_Y + 4.99) so the top of
# "BT" lands on the jack's top edge and the bottom of "LINK" on its bottom edge.
# Hard-coded numbers drifted 0.6 mm apart the last time RJ_Y moved.
JACK_TOP, JACK_BOT = RJ_Y - 11.88, RJ_Y + 4.99
SILK = [("BT",   18.2, JACK_TOP + 0.4, "F.SilkS", 0.8, 0),   # 0.4 = half text height
        ("LINK", 18.2, JACK_BOT - 0.4, "F.SilkS", 0.8, 0),
        ("DESK", 17.2, 17.6, "F.SilkS", 0.8, 90),
        ("ANTENNA", 26.25, 16.5, "F.SilkS", 1.1, 0),
        ("KEEP CLEAR", 26.25, 19.3, "F.SilkS", 0.8, 0),
        # identity block on the back, where the module does not cover it
        ("FlexiSpot / LoctekMotion", 36.0, 10.0, "B.SilkS", 1.0, 0),
        ("BLE desk companion", 36.0, 14.5, "B.SilkS", 1.4, 0),
        ("woutertenbrinke.nl", 36.0, 19.0, "B.SilkS", 1.0, 0),
        ("ESP32 DevKit V1 30-pin", 36.0, 23.5, "B.SilkS", 0.9, 0),
        # B.SilkS mirrors, so the item at the HIGHER x appears on the left when
        # you look at the back -- swap them to read "name ... rev" the normal way.
        ("Wouter ten Brinke", 45.0, 27.0, "B.SilkS", 1.0, 0),
        ("rev1", 28.5, 27.0, "B.SilkS", 1.0, 0)]


for n, nm in enumerate(ROW_A_NAMES):
    SILK.append((nm, PIN1_X + n * PITCH, 2.75, "F.SilkS", 0.8, 0))
for n, nm in enumerate(ROW_B_NAMES):
    SILK.append((nm, PIN1_X + n * PITCH, 32.45, "F.SilkS", 0.8, 0))

for txt, x, y, layer, size, ang in SILK:
    t = GrText(text=txt, position=Position(x, y, ang), layer=layer)
    t.effects.font.width = t.effects.font.height = size
    t.effects.font.thickness = 0.15
    if layer.startswith("B."):
        t.effects.justify.mirror = True
    board.graphicItems.append(t)


def zone_poly(pts, layers, net=""):
    z = Zone()
    z.layers = layers
    z.net, z.netName = (NETIDX.get(net, 0), net)
    z.hatch = Hatch("edge", 0.5)
    z.polygons = [ZonePolygon(coordinates=[Position(*p) for p in pts])]
    z.fillSettings = FillSettings(yes=True, thermalGap=0.3, thermalBridgeWidth=0.3)
    return z


def zone(layers, x0, y0, x1, y1, net="", keepout=False):
    z = Zone()
    z.layers = layers
    z.net, z.netName = (NETIDX.get(net, 0), net)
    z.hatch = Hatch("edge", 0.5)
    z.polygons = [ZonePolygon(coordinates=[Position(x0, y0), Position(x1, y0),
                                           Position(x1, y1), Position(x0, y1)])]
    if keepout:
        # user asked for tracks kept out of the antenna region, so unlike revA
        # this forbids tracks and vias too -- KiCad DRC now enforces what the
        # Specctra export was silently enforcing anyway.
        z.keepoutSettings = KeepoutSettings(tracks="not_allowed", vias="not_allowed",
                                            pads="allowed", copperpour="not_allowed",
                                            footprints="allowed")
    else:
        z.fillSettings = FillSettings(yes=True, thermalGap=0.3, thermalBridgeWidth=0.3)
    return z


# the pour is chamfered at the corners: a plain rectangle inset from a board with
# 3 mm rounded corners still pokes outside the arc
board.zones.append(zone_poly([(4.0,0.8),(W-4.0,0.8),(W-0.8,4.0),(W-0.8,H-4.0),
                              (W-4.0,H-0.8),(4.0,H-0.8),(0.8,H-4.0),(0.8,4.0)],
                             ["B.Cu"], net="GND"))
board.zones.append(zone(["F.Cu", "B.Cu"], *KEEPOUT, keepout=True))

# One rule area per mounting hole. The router already keeps its own tracks out,
# but the GND pour would otherwise fill right up to the drill, leaving copper
# under the screw head with nothing but soldermask between steel and copper.
# An M3 pan head plus washer is ~6 mm across, so keep a 3 mm radius clear --
# approximated as a square, which is what the pour filler wants anyway.
SCREW_KEEP = 3.0
for ref, lib, fpname, hx, hy, *_ in PARTS:
    if not ref.startswith("H"):
        continue
    board.zones.append(zone(["F.Cu", "B.Cu"],
                            hx - SCREW_KEEP, hy - SCREW_KEEP,
                            hx + SCREW_KEEP, hy + SCREW_KEEP, keepout=True))

board.to_file(OUT)
print(f"wrote {OUT}: {len(board.footprints)} footprints, {len(board.nets)} nets")
print(f"board {W} x {H} mm, rows y={ROW_A_Y}/{ROW_B_Y}, pin1 x={PIN1_X}")
for k in ("1", "2", str(PINS_PER_ROW)):
    print(f"   U1A.{k} -> {PADS[('U1A', k)][0]:.2f},{PADS[('U1A', k)][1]:.2f}")
