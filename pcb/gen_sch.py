#!/usr/bin/env python3
"""Generate tab5-desk-ctrl.kicad_sch: FlexiSpot / LoctekMotion BLE desk companion.

Laid out the conventional way -- functional blocks boxed and titled, real wires
drawn inside each block, net labels only where a signal crosses between blocks,
power symbols on the rails. Everything sits on a 2.54 mm grid so pins land on
KiCad's 1.27 mm connection grid; a pin between grid points looks connected and
is not.

The module sockets use project symbols (symbols/tab5-desk-ctrl.kicad_sym) whose
pins carry the DevKit V1 names -- D23, TX2, RX2, D32 ... -- instead of the
generic Conn_01x15's Pin_1..Pin_15. Those are the same names silkscreened beside
the holes on the board, so the drawing and the PCB read against each other.

Linked to the PCB by link_sch.py, which is what lets --schematic-parity confirm
the copper matches this drawing.
"""
import math, os, uuid
from kiutils.schematic import Schematic
from kiutils.symbol import SymbolLib
from kiutils.items.schitems import (SchematicSymbol, LocalLabel, NoConnect, Connection,
                                    Junction, Text, Rectangle, SymbolInstance,
                                    HierarchicalSheetInstance)
from kiutils.items.common import (Position, Property, Effects, Font, Stroke,
                                  TitleBlock, Justify)

HERE = os.path.dirname(os.path.abspath(__file__))
SYM = os.environ.get("KICAD9_SYMBOL_DIR", "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols")
PRJSYM = os.path.join(HERE, "symbols")
OUT = os.path.join(HERE, "tab5-desk-ctrl.kicad_sch")

# A5 landscape, 210 x 148: packed, so the sheet reads at a size you can use. The
# title block takes roughly x>90, y>102. Two columns:
#   left   12.70 ..  88.90   the jack and its LEDs, the module's sockets
#   right  91.44 .. 197.30   5 V input, the signal conditioning, notes
# Signal nets carry plain labels, written over the stub they name; power is
# drawn as power symbols, named, never as labels.

# ref: (lib, symbol, x, y, angle, mirror, value, footprint)
# A two-pin part turned 90 has pin 1 on the left, turned 270 pin 1 on the right.
PLACE = {
 # 8P8C_LED_Shielded: signal pins 1-8 on the right, LED pins 9-12 on the left,
 # shield at the bottom -- matches the Ckmtw jack's pin numbering exactly
 "RJ45_1": ("Connector","8P8C_LED_Shielded", 48.26, 43.18, 0,None,"RJ45 8P8C 2xLED",
            "Connector_RJ:RJ45_Connfly_DS1128-09-S8xx-S_Horizontal"),
 "R5": ("Device","R",           48.26, 67.31,270,None,"220R","Resistor_SMD:R_0805_2012Metric"),
 "R6": ("Device","R",           48.26, 74.93,270,None,"220R","Resistor_SMD:R_0805_2012Metric"),
 "F1": ("Device","Polyfuse",   109.22, 27.94, 90,None,"PTC 500mA","Resistor_SMD:R_1206_3216Metric"),
 "D1": ("Device","D_Schottky", 127.00, 27.94,  0,"y","SS14","Diode_SMD:D_SMA"),
 # the bulk capacitor that carries the radio's start-up surge on desk power
 "C3": ("Device","C_Polarized",148.59, 35.56,  0,None,"470uF 10V","Capacitor_THT:CP_Radial_D8.0mm_P3.50mm"),
 "C1": ("Device","C",          153.67, 35.56,  0,None,"22uF 25V","Capacitor_SMD:C_1206_3216Metric"),
 "C2": ("Device","C",          171.45, 35.56,  0,None,"100nF 50V","Capacitor_SMD:C_0805_2012Metric"),
 "R1": ("Device","R",          109.22, 60.96, 90,None,"10k","Resistor_SMD:R_0805_2012Metric"),
 "R2": ("Device","R",          119.38, 69.85,  0,None,"15k","Resistor_SMD:R_0805_2012Metric"),
 "D2": ("Diode","BAT54S",      134.62, 69.85,  0,"x","BAT54S","Package_TO_SOT_SMD:SOT-23"),
 "R3": ("Device","R",          180.34, 60.96,270,None,"220R","Resistor_SMD:R_0805_2012Metric"),
 "R4": ("Device","R",          180.34, 73.66,270,None,"220R","Resistor_SMD:R_0805_2012Metric"),
 "U1A": ("tab5-desk-ctrl","ESP32_DevKitV1_RowA", 76.20,115.57, 0,None,"Socket 1x15 2.54mm",
         "Connector_PinSocket_2.54mm:PinSocket_1x15_P2.54mm_Vertical"),
 "U1B": ("tab5-desk-ctrl","ESP32_DevKitV1_RowB", 35.56,115.57, 0,None,"Socket 1x15 2.54mm",
         "Connector_PinSocket_2.54mm:PinSocket_1x15_P2.54mm_Vertical"),
 "H1": ("Mechanical","MountingHole", 176.53, 93.98, 0,None,"M3","MountingHole:MountingHole_3.2mm_M3"),
 "H2": ("Mechanical","MountingHole", 181.61, 93.98, 0,None,"M3","MountingHole:MountingHole_3.2mm_M3"),
 "H3": ("Mechanical","MountingHole", 186.69, 93.98, 0,None,"M3","MountingHole:MountingHole_3.2mm_M3"),
 "H4": ("Mechanical","MountingHole", 191.77, 93.98, 0,None,"M3","MountingHole:MountingHole_3.2mm_M3"),
}
# power symbols: (symbol, x, y, angle). A GND turned 90 points right, 270 left;
# a +5V or +3V3 turned 90 points left.
POWER_SYMS = {
 "#PWR01": ("GND",   60.96, 35.56, 90),    # jack pin 7
 "#PWR02": ("GND",   35.56, 33.02,270),    # LED cathode, pin 9
 "#PWR03": ("GND",   35.56, 48.26,270),    # LED cathode, pin 11
 "#PWR04": ("GND",   48.26, 60.96,  0),    # shield
 "#PWR05": ("+5V",  135.89, 22.86,  0),
 "#PWR06": ("GND",  148.59, 41.91,  0),
 "#PWR07": ("GND",  153.67, 41.91,  0),
 "#PWR08": ("GND",  171.45, 41.91,  0),
 "#PWR09": ("GND",  119.38, 76.20,  0),
 "#PWR10": ("GND",  127.00, 74.93,  0),
 "#PWR11": ("+3V3", 147.32, 69.85,  0),
 "#PWR12": ("GND",   22.86,130.81,270),    # U1B GND
 "#PWR13": ("+5V",   22.86,133.35, 90),    # U1B VIN
 "#PWR14": ("GND",   63.50,130.81,270),    # U1A GND
 "#PWR15": ("+3V3",  63.50,133.35, 90),    # U1A 3V3
 "#FLG01": ("PWR_FLAG", 140.97, 22.86, 0), # +5V rail
 "#FLG02": ("PWR_FLAG",  53.34, 58.42, 0), # GND at the desk connector
 "#FLG03": ("PWR_FLAG", 152.40, 69.85, 0), # +3V3 off the module
}
for ref,(sym,x,y,a) in POWER_SYMS.items():
    # GND uses the project's own symbol: the three-bar earth glyph, but with
    # Value "GND" so the net keeps its name. power:Earth / power:GNDREF have the
    # right drawing and the wrong name -- a power symbol's Value IS the net.
    if sym == "GND":
        PLACE[ref] = ("tab5-desk-ctrl","GND_EARTH",x,y,a,None,"GND","")
    else:
        PLACE[ref] = ("power",sym,x,y,a,None,sym,"")

# where each visible field goes: ref -> ((ref x, y), (value x, y), justify)
FIELDS = {
 "RJ45_1": ((48.26, 25.40), (48.26, 27.94), "center"),
 "R5": ((48.26, 64.77), (48.26, 69.85), "center"),
 "R6": ((48.26, 72.39), (48.26, 77.47), "center"),
 "F1": ((109.22, 24.13), (109.22, 31.75), "center"),
 "D1": ((127.00, 24.13), (127.00, 31.75), "center"),
 "C3": ((146.05, 34.29), (146.05, 36.83), "right"),
 "C1": ((156.21, 34.29), (156.21, 36.83), "left"),
 "C2": ((173.99, 34.29), (173.99, 36.83), "left"),
 "R1": ((109.22, 58.42), (109.22, 63.50), "center"),
 "R2": ((121.92, 68.58), (121.92, 71.12), "left"),
 "D2": ((134.62, 74.93), (134.62, 77.47), "center"),
 "R3": ((180.34, 58.42), (180.34, 63.50), "center"),
 "R4": ((180.34, 71.12), (180.34, 76.20), "center"),
 "U1A": ((76.20, 91.44), (76.20, 93.98), "center"),
 "U1B": ((35.56, 91.44), (35.56, 93.98), "center"),
}
for h in ("H1","H2","H3","H4"):
    x, y = PLACE[h][2], PLACE[h][3]
    FIELDS[h] = ((x, y+3.81), (x, y+3.81), "center")
HIDE_VALUE = {"H1", "H2", "H3", "H4"}          # "M3" is in the note above them
# the names of the +5V and +3V3 symbols; GND's glyph says it, PWR_FLAG needs none
POWER_NAMES = {"#PWR05": (135.89, 19.05), "#PWR11": (147.32, 64.77),
               "#PWR13": (17.78, 133.35), "#PWR15": (58.42, 133.35)}

# pins drawn without their numbers: every two-pin part, the clamp and the rails
HIDE_PIN_NUMBERS = {"R", "C", "C_Polarized", "Polyfuse", "D_Schottky", "BAT54S",
                    "GND", "+5V", "+3V3", "PWR_FLAG", "GND_EARTH"}

libs, protos = {}, {}
def proto(lib,name):
    if (lib,name) not in protos:
        if lib not in libs:
            root = PRJSYM if lib == "tab5-desk-ctrl" else SYM
            libs[lib] = SymbolLib.from_file(f"{root}/{lib}.kicad_sym")
        protos[(lib,name)] = [s for s in libs[lib].symbols if s.entryName==name][0]
    return protos[(lib,name)]

def pin(ref, num):
    """Absolute connection point of a pin, after mirror then rotation."""
    lib,name,x,y,ang,mir,_,_ = PLACE[ref]
    for u in proto(lib,name).units:
        for p in u.pins:
            if str(p.number) != str(num): continue
            px,py = p.position.X, p.position.Y
            if mir == "y": px = -px
            if mir == "x": py = -py
            a = math.radians(ang)
            sx = x + px*math.cos(a) - py*math.sin(a)
            sy = y - px*math.sin(a) - py*math.cos(a)
            return (round(sx,2), round(sy,2))
    raise SystemExit(f"{ref} has no pin {num}")

P = pin
def seg(*pts): return [tuple(p) for p in pts]

# ---- wires: each entry is a polyline of absolute points ----
WIRES = []
def w(*pts): WIRES.append(seg(*pts))
# (net, x, y, angle): a label at the outer end of its stub, its text running back
# over the wire -- 0 when the stub runs right from there, 180 when it runs left
LABELS = []
def stub(ref, pin_, x, net=None):
    """A straight wire from a pin out to x, labelled at its far end."""
    a = P(ref, pin_)
    w(a, (x, a[1]))
    if net:
        LABELS.append((net, x, a[1], 0 if x < a[0] else 180))

# --- DESK CONNECTOR AND LEDS: signals right, LEDs left, grounds at the pin ---
for p, net in (("8","DESK_5V"), ("6","DESK_RX"), ("5","DESK_TX"), ("4","PIN20")):
    stub("RJ45_1", p, 76.20, net)
stub("RJ45_1", "7", 60.96)                                     # GND, pointing right
# Datasheet (both the PC-board-layout and the bottom view) marks the four LED
# pads "+ - + -" left to right with pin 1 on the right. KiCad's footprint draws
# the part rotated 180 deg (pin 1 on the left), so in its numbering the anodes
# are pads 10 and 12, not 9 and 11.
stub("RJ45_1", "10", 20.32, "LINK_A")
stub("RJ45_1", "12", 20.32, "BT_A")
stub("RJ45_1", "9", 35.56)                                     # cathodes to GND
stub("RJ45_1", "11", 35.56)
w(P("RJ45_1","SH"), (48.26, 60.96))                            # shield to GND
w((48.26, 58.42), (53.34, 58.42))                              # and its PWR_FLAG
for r, anode, gpio in (("R5","BT_A","BT_LED"), ("R6","LINK_A","LINK_LED")):
    stub(r, "2", 20.32, anode)
    stub(r, "1", 76.20, gpio)

# --- 5 V INPUT ------------------------------------------------------------
stub("F1", "1", 93.98, "DESK_5V")
w(P("F1","2"), P("D1","2"))
LABELS.append(("D5V_F", *P("D1","2"), 180))
w(P("D1","1"), (171.45, 27.94))                                # +5V rail
w((135.89, 22.86), (135.89, 27.94))                            # +5V symbol
w((140.97, 22.86), (140.97, 27.94))                            # PWR_FLAG
for c in ("C3", "C1", "C2"):
    x = P(c, "1")[0]
    w((x, 27.94), P(c, "1"));  w(P(c, "2"), (x, 41.91))

# --- RX LEVEL SHIFT -------------------------------------------------------
stub("R1", "1", 93.98, "DESK_TX")
stub("R1", "2", 157.48, "RX_DIV")
w((119.38, 60.96), P("R2","1"));  w(P("R2","2"), (119.38, 76.20))
w(P("D2","3"), (134.62, 60.96))                                # clamp's COM to the node
w(P("D2","1"), (127.00, 69.85), (127.00, 74.93))               # A to GND
w(P("D2","2"), (147.32, 69.85))                                # K to +3V3
w((147.32, 69.85), (152.40, 69.85))                            # and its flag

# --- TX AND WAKE ----------------------------------------------------------
for r, desk, esp in (("R3","DESK_RX","ESP_TX"), ("R4","PIN20","ESP_P20")):
    stub(r, "2", 165.10, desk)
    stub(r, "1", 195.58, esp)

# --- module sockets: row B is the module's left-hand column and row A its
# right-hand one -- the sheet is laid out to match the DevKit pinout you would
# look up
U1A_N = {"1":"ESP_P20","9":"ESP_TX","10":"RX_DIV","14":"GND","15":"+3V3"}
U1B_N = {"6":"BT_LED","7":"LINK_LED","14":"GND","15":"+5V"}
for ref, nets, x in (("U1B", U1B_N, 13.97), ("U1A", U1A_N, 53.34)):
    for p, n in nets.items():
        if n in ("GND", "+5V", "+3V3"):
            stub(ref, p, P(ref, p)[0] - 2.54)                  # to its power symbol
        else:
            stub(ref, p, x, n)

# ---- functional blocks: (x0, y0, x1, y1, title, subtitle) ----
BLOCKS = [
 ( 12.70, 12.70,  88.90,  78.74, "DESK CONNECTOR AND LEDS", "RJ45 to the control box; the two LEDs are inside the jack"),
 ( 91.44, 12.70, 197.30,  45.72, "5 V INPUT",       "PTC, reverse block, bulk"),
 ( 91.44, 48.26, 160.02,  81.28, "RX LEVEL SHIFT",  "5 V box TX -> 3.3 V ESP, clamped"),
 (162.56, 48.26, 197.30,  81.28, "TX AND WAKE",     "3.3 V, series limited"),
 ( 12.70, 81.28,  88.90, 136.40, "ESP32 DevKit V1 30-pin sockets",
                                 "pin names as on the silkscreen, both sides"),
 ( 91.44, 83.82, 197.30, 101.60, "NOTES", ""),
]

# notes sit inside their block, left aligned with the title
NOTES = [( 93.98, 90.68, "Box pinout: 8 +5V, 7 GND, 6 box RX, 5 box TX (5 V), 4 wake, 1-3 unused"),
         ( 93.98, 92.96, "Idle: pin 5 ~5.0 V, RX2 ~3.0 V, VIN ~4.5 V; wake high = awake"),
         ( 93.98, 95.25, "GPIO16 RX2, GPIO17 TX2, GPIO23 wake, GPIO32 BT, GPIO33 LINK"),
         ( 93.98, 97.54, "BT solid: panel connected. LINK solid: box answers, fast: no frames"),
         ( 93.98, 99.82, "C3: long leg to +5V, at most 11.5 mm tall. Rev1 had pins 5 and 6 swapped"),
         (176.53, 90.17, "4x M3, 71.8 x 28.8")]

# ---- build ----
sch = Schematic.create_new(); sch.paper.paperSize = "A5"
sch.titleBlock = TitleBlock(title="FlexiSpot / LoctekMotion BLE desk companion",
                            revision="rev2", company="Wouter ten Brinke",
                            comments={1: "ESP32 DevKit V1 30-pin carrier, RJ45 to the control box"})
seen = set()
for ref,(lib,name,x,y,ang,mir,val,fp) in PLACE.items():
    key = f"{lib}:{name}"
    if key not in seen:
        s = proto(lib,name); s.libId = key; s.libraryNickname, s.entryName = lib, name
        # kiutils drops KiCad 9's "(pin_numbers (hide yes))", so set it again
        if name in HIDE_PIN_NUMBERS:
            s.hidePinNumbers = True
            s.pinNamesHide = True
        sch.libSymbols.append(s); seen.add(key)
    hidden = ref.startswith("#") or ref.startswith("TP")
    inst = SchematicSymbol(libraryNickname=lib, entryName=name,
                           position=Position(x,y,ang), unit=1,   # angle must be written even when 0
                           inBom=not (hidden or ref.startswith("TP") or ref.startswith("H")),
                           onBoard=True, mirror=mir, uuid=str(uuid.uuid4()))
    # A field turns with its symbol, so a part turned 90 or 270 gets its fields
    # turned the same way to read level on the sheet.
    fang = ang if ang in (90, 270) else 0
    def eff(hide, just="center"):
        j = Justify() if just == "center" else Justify(horizontally=just)
        return Effects(font=Font(width=1.27,height=1.27), hide=hide, justify=j)
    if ref in FIELDS:
        (rx, ry0), (vx, ry1), just = FIELDS[ref]
    else:
        rx, ry0, vx, ry1, just = x, y, x, y, "center"
    name_shown = ref in POWER_NAMES
    if name_shown:
        vx, ry1 = POWER_NAMES[ref]
    inst.properties = [
        Property(key="Reference", value=ref, position=Position(rx,ry0,fang), effects=eff(hidden, just)),
        Property(key="Value",     value=val, position=Position(vx,ry1,fang),
                 effects=eff((hidden and not name_shown) or ref in HIDE_VALUE, just)),
        Property(key="Footprint", value=fp,  position=Position(x,y,0), effects=eff(True)),
        Property(key="Datasheet", value="~", position=Position(x,y,0), effects=eff(True))]
    sch.schematicSymbols.append(inst)
    sch.symbolInstances.append(SymbolInstance(path=f"/{inst.uuid}",reference=ref,unit=1,
                                              value=val,footprint=fp))
# unused connector pins
for ref,used,total in (("RJ45_1",{"4","5","6","7","8","9","10","11","12"},12),
                       ("U1A",set(U1A_N),15),("U1B",set(U1B_N),15)):
    for n in range(1,total+1):
        if str(n) not in used:
            sch.noConnects.append(NoConnect(position=Position(*P(ref,str(n))),uuid=str(uuid.uuid4())))

# A KiCad (wire ...) is a single segment, and KiCad only makes a connection at a
# segment's ENDPOINTS -- a wire landing on the middle of a longer one does not
# join it, junction dot or not. Real KiCad files are always split at those
# points, so do the same: break every run into segments, then break each segment
# wherever another wire's end touches it.
raw = []
for pts in WIRES:
    raw.extend(zip(pts, pts[1:]))

touch = set()
for pts in WIRES:
    touch.add(pts[0]); touch.add(pts[-1])

def between(p, a, b):
    if p in (a, b): return False
    if abs((b[0]-a[0])*(p[1]-a[1]) - (b[1]-a[1])*(p[0]-a[0])) > 1e-6: return False
    return (min(a[0],b[0])-1e-6 <= p[0] <= max(a[0],b[0])+1e-6 and
            min(a[1],b[1])-1e-6 <= p[1] <= max(a[1],b[1])+1e-6)

split_count = 0
for a, b in raw:
    cuts = sorted([p for p in touch if between(p, a, b)],
                  key=lambda p: (p[0]-a[0])**2 + (p[1]-a[1])**2)
    chain = [a] + cuts + [b]
    split_count += len(cuts)
    for u_, v_ in zip(chain, chain[1:]):
        sch.graphicalItems.append(Connection(type="wire",
            points=[Position(*u_), Position(*v_)], uuid=str(uuid.uuid4())))

# junctions wherever a wire end lands on another wire's interior, or 3+ ends meet
from collections import Counter
ends = Counter()
segs = []
for pts in WIRES:
    for a,b in zip(pts,pts[1:]): segs.append((a,b))
    ends[pts[0]] += 1; ends[pts[-1]] += 1
def on_seg(p,a,b):
    if p in (a,b): return False
    cross = (b[0]-a[0])*(p[1]-a[1]) - (b[1]-a[1])*(p[0]-a[0])
    if abs(cross) > 1e-6: return False
    return min(a[0],b[0])-1e-6 <= p[0] <= max(a[0],b[0])+1e-6 and \
           min(a[1],b[1])-1e-6 <= p[1] <= max(a[1],b[1])+1e-6
# a segment passing through a point contributes two branches, one ending there
# contributes one; three or more branches need a junction dot (a plain T counts)
for p,c in ends.items():
    touching = c + 2 * sum(1 for a,b in segs if on_seg(p,a,b))
    if touching >= 3:
        sch.junctions.append(Junction(position=Position(*p),uuid=str(uuid.uuid4())))

# Plain labels: on the root sheet KiCad names these nets "/DESK_TX", and
# gen_pcb.py gives the board's nets the same names.
for t,x,y,a in LABELS:
    just = Justify(horizontally="right" if a in (180, 270) else "left", vertically="bottom")
    sch.labels.append(LocalLabel(text=t,position=Position(x,y,a),
        effects=Effects(font=Font(width=1.27,height=1.27), justify=just),uuid=str(uuid.uuid4())))

def left(size):
    return Effects(font=Font(width=size,height=size), justify=Justify(horizontally="left"))

# A KiCad Text is centred on its position unless told otherwise. Centring a long
# block title on the box's top-left corner is what threw every title half out of
# its own box and across its neighbour -- left-justify and inset instead.
for x0,y0,x1,y1,title,sub in BLOCKS:
    sch.shapes.append(Rectangle(start=Position(x0,y0), end=Position(x1,y1),
        stroke=Stroke(width=0.127, type="dash"), uuid=str(uuid.uuid4())))
    sch.texts.append(Text(text=title,position=Position(x0+2.54,y0+3.81,0),
        effects=left(1.5),uuid=str(uuid.uuid4())))
    if sub:
        sch.texts.append(Text(text=sub,position=Position(x0+2.54,y0+6.60,0),
            effects=left(1.0),uuid=str(uuid.uuid4())))
for x,y,t in NOTES:
    sch.texts.append(Text(text=t,position=Position(x,y,0),
        effects=left(1.0),uuid=str(uuid.uuid4())))

sch.sheetInstances.append(HierarchicalSheetInstance(instancePath="/",page="1"))
sch.to_file(OUT)
print(f"{len(sch.schematicSymbols)} symbols, {len(sch.graphicalItems)} wire segments "
      f"({split_count} splits), "
      f"{len(sch.junctions)} junctions, {len(sch.labels)} labels, "
      f"{len(sch.noConnects)} no-connects")
