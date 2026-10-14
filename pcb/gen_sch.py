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
from kiutils.items.schitems import (SchematicSymbol, GlobalLabel, NoConnect, Connection,
                                    Junction, Text, Rectangle, SymbolInstance,
                                    HierarchicalSheetInstance)
from kiutils.items.common import (Position, Property, Effects, Font, Stroke,
                                  TitleBlock, Justify)

HERE = os.path.dirname(os.path.abspath(__file__))
SYM = os.environ.get("KICAD9_SYMBOL_DIR", "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols")
PRJSYM = os.path.join(HERE, "symbols")
OUT = os.path.join(HERE, "tab5-desk-ctrl.kicad_sch")

# A4 landscape, 297 x 210. The title block takes roughly x>177, y>164, so every
# block stays clear of that corner. Signals run left to right, desk to module:
#   left    15.24 ..  83.82   the jack and its LEDs
#   middle  91.44 .. 190.50   5 V input and the signal conditioning
#   right  198.12 .. 287.02   the module's sockets, mounting
# Every net label stands at the far end of its stub and runs on away from the
# wire; power is drawn as power symbols, named, never as labels.

# ref: (lib, symbol, x, y, angle, mirror, value, footprint)
# A two-pin part turned 90 has pin 1 on the left, turned 270 pin 1 on the right.
PLACE = {
 # 8P8C_LED_Shielded: signal pins 1-8 on the right, LED pins 9-12 on the left,
 # shield at the bottom -- matches the Ckmtw jack's pin numbering exactly
 "RJ45_1": ("Connector","8P8C_LED_Shielded", 48.26, 53.34, 0,None,"RJ45 8P8C 2xLED",
            "Connector_RJ:RJ45_Connfly_DS1128-09-S8xx-S_Horizontal"),
 "R5": ("Device","R",           49.53,110.49,270,None,"220R","Resistor_SMD:R_0805_2012Metric"),
 "R6": ("Device","R",           49.53,121.92,270,None,"220R","Resistor_SMD:R_0805_2012Metric"),
 "F1": ("Device","Polyfuse",   116.84, 35.56, 90,None,"PTC 500mA","Resistor_SMD:R_1206_3216Metric"),
 "D1": ("Device","D_Schottky", 139.70, 35.56,  0,"y","SS14","Diode_SMD:D_SMA"),
 # the bulk capacitor that carries the radio's start-up surge on desk power
 "C3": ("Device","C_Polarized",147.32, 45.72,  0,None,"1000uF 6.3V","Capacitor_THT:CP_Radial_D8.0mm_P3.50mm"),
 "C1": ("Device","C",          152.40, 45.72,  0,None,"22uF 25V","Capacitor_SMD:C_1206_3216Metric"),
 "C2": ("Device","C",          170.18, 45.72,  0,None,"100nF 50V","Capacitor_SMD:C_0805_2012Metric"),
 "R1": ("Device","R",          113.03, 81.28, 90,None,"10k","Resistor_SMD:R_0805_2012Metric"),
 "R2": ("Device","R",          127.00, 91.44,  0,None,"15k","Resistor_SMD:R_0805_2012Metric"),
 "D2": ("Diode","BAT54S",      147.32, 91.44,  0,"x","BAT54S","Package_TO_SOT_SMD:SOT-23"),
 "R3": ("Device","R",          124.46,139.70,270,None,"220R","Resistor_SMD:R_0805_2012Metric"),
 "R4": ("Device","R",          124.46,152.40,270,None,"220R","Resistor_SMD:R_0805_2012Metric"),
 "U1A": ("tab5-desk-ctrl","ESP32_DevKitV1_RowA", 274.32, 55.88, 0,None,"Socket 1x15 2.54mm",
         "Connector_PinSocket_2.54mm:PinSocket_1x15_P2.54mm_Vertical"),
 "U1B": ("tab5-desk-ctrl","ESP32_DevKitV1_RowB", 231.14, 55.88, 0,None,"Socket 1x15 2.54mm",
         "Connector_PinSocket_2.54mm:PinSocket_1x15_P2.54mm_Vertical"),
 "H1": ("Mechanical","MountingHole", 208.28,123.19, 0,None,"M3","MountingHole:MountingHole_3.2mm_M3"),
 "H2": ("Mechanical","MountingHole", 228.60,123.19, 0,None,"M3","MountingHole:MountingHole_3.2mm_M3"),
 "H3": ("Mechanical","MountingHole", 248.92,123.19, 0,None,"M3","MountingHole:MountingHole_3.2mm_M3"),
 "H4": ("Mechanical","MountingHole", 269.24,123.19, 0,None,"M3","MountingHole:MountingHole_3.2mm_M3"),
}
# power symbols: (symbol, x, y, angle). A GND turned 90 points right, 270 left;
# a +5V or +3V3 turned 90 points left.
POWER_SYMS = {
 "#PWR01": ("GND",   60.96, 45.72, 90),    # jack pin 7
 "#PWR02": ("GND",   35.56, 43.18,270),    # LED cathode, pin 9
 "#PWR03": ("GND",   35.56, 58.42,270),    # LED cathode, pin 11
 "#PWR04": ("GND",   48.26, 71.12,  0),    # shield
 "#PWR05": ("+5V",  157.48, 30.48,  0),
 "#PWR06": ("GND",  147.32, 52.07,  0),
 "#PWR07": ("GND",  152.40, 52.07,  0),
 "#PWR08": ("GND",  170.18, 52.07,  0),
 "#PWR09": ("GND",  127.00, 97.79,  0),
 "#PWR10": ("GND",  139.70, 96.52,  0),
 "#PWR11": ("+3V3", 160.02, 91.44,  0),
 "#PWR12": ("GND",  218.44, 71.12,270),    # U1B GND
 "#PWR13": ("+5V",  218.44, 73.66, 90),    # U1B VIN
 "#PWR14": ("GND",  261.62, 71.12,270),    # U1A GND
 "#PWR15": ("+3V3", 261.62, 73.66, 90),    # U1A 3V3
 "#FLG01": ("PWR_FLAG", 162.56, 30.48, 0), # +5V rail
 "#FLG02": ("PWR_FLAG",  53.34, 68.58, 0), # GND at the desk connector
 "#FLG03": ("PWR_FLAG", 165.10, 91.44, 0), # +3V3 off the module
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
 "RJ45_1": ((48.26, 33.02), (48.26, 35.56), "center"),
 "R5": ((49.53,107.95), (49.53,113.03), "center"),
 "R6": ((49.53,119.38), (49.53,124.46), "center"),
 "F1": ((116.84, 31.75), (116.84, 39.37), "center"),
 "D1": ((139.70, 31.75), (139.70, 39.37), "center"),
 "C3": ((144.78, 44.45), (144.78, 46.99), "right"),
 "C1": ((154.94, 44.45), (154.94, 46.99), "left"),
 "C2": ((172.72, 44.45), (172.72, 46.99), "left"),
 "R1": ((113.03, 78.74), (113.03, 83.82), "center"),
 "R2": ((129.54, 90.17), (129.54, 92.71), "left"),
 "D2": ((147.32, 96.52), (147.32, 99.06), "center"),
 "R3": ((124.46,137.16), (124.46,142.24), "center"),
 "R4": ((124.46,149.86), (124.46,154.94), "center"),
 "U1A": ((274.32, 31.75), (274.32, 34.29), "center"),
 "U1B": ((231.14, 31.75), (231.14, 34.29), "center"),
}
for h in ("H1","H2","H3","H4"):
    x, y = PLACE[h][2], PLACE[h][3]
    FIELDS[h] = ((x+3.81, y-1.27), (x+3.81, y+1.27), "left")
# the names of the +5V and +3V3 symbols; GND's glyph says it, PWR_FLAG needs none
POWER_NAMES = {"#PWR05": (157.48, 26.67), "#PWR11": (160.02, 86.36),
               "#PWR13": (213.36, 73.66), "#PWR15": (256.54, 73.66)}

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
LABELS = []   # (net, x, y, angle): 0 reads on to the right, 180 to the left
def stub(ref, pin_, x, net=None, angle=0):
    """A straight wire from a pin out to x, labelled at its far end."""
    a = P(ref, pin_)
    w(a, (x, a[1]))
    if net:
        LABELS.append((net, x, a[1], angle))

# --- DESK CONNECTOR: signals right, LEDs left, grounds at the pin ---------
for p, net in (("8","DESK_5V"), ("6","DESK_RX"), ("5","DESK_TX"), ("4","PIN20")):
    stub("RJ45_1", p, 63.50, net)
stub("RJ45_1", "7", 60.96)                                     # GND, pointing right
# Datasheet (both the PC-board-layout and the bottom view) marks the four LED
# pads "+ - + -" left to right with pin 1 on the right. KiCad's footprint draws
# the part rotated 180 deg (pin 1 on the left), so in its numbering the anodes
# are pads 10 and 12, not 9 and 11.
stub("RJ45_1", "10", 33.02, "LED_DESK_A", 180)
stub("RJ45_1", "12", 33.02, "LED_BT_A", 180)
stub("RJ45_1", "9", 35.56)                                     # cathodes to GND
stub("RJ45_1", "11", 35.56)
w(P("RJ45_1","SH"), (48.26, 71.12))                            # shield to GND
w((48.26, 68.58), (53.34, 68.58))                              # and its PWR_FLAG

# --- LED INDICATORS -------------------------------------------------------
for r, anode, gpio in (("R5","LED_BT_A","LED_BT"), ("R6","LED_DESK_A","LED_DESK")):
    stub(r, "2", 30.48, anode, 180)
    stub(r, "1", 68.58, gpio)

# --- 5 V INPUT ------------------------------------------------------------
stub("F1", "1", 101.60, "DESK_5V", 180)
w(P("F1","2"), (127.00, 35.56), P("D1","2"))
w((127.00, 35.56), (127.00, 33.02))                            # D5V_F, above the wire
LABELS.append(("D5V_F", 127.00, 33.02, 0))
w(P("D1","1"), (170.18, 35.56))                                # +5V rail
w((157.48, 30.48), (157.48, 35.56))                            # +5V symbol
w((162.56, 30.48), (162.56, 35.56))                            # PWR_FLAG
for c in ("C3", "C1", "C2"):
    x = P(c, "1")[0]
    w((x, 35.56), P(c, "1"));  w(P(c, "2"), (x, 52.07))

# --- RX LEVEL SHIFT -------------------------------------------------------
stub("R1", "1", 101.60, "DESK_TX", 180)
stub("R1", "2", 172.72, "RX_DIV")
w((127.00, 81.28), P("R2","1"));  w(P("R2","2"), (127.00, 97.79))
w(P("D2","3"), (147.32, 81.28))                                # clamp's COM to the node
w(P("D2","1"), (139.70, 91.44), (139.70, 96.52))               # A to GND
w(P("D2","2"), (160.02, 91.44))                                # K to +3V3
w((160.02, 91.44), (165.10, 91.44))                            # and its flag

# --- TX AND WAKE ----------------------------------------------------------
for r, desk, esp in (("R3","DESK_RX","ESP_TX"), ("R4","PIN20","ESP_P20")):
    stub(r, "2", 101.60, desk, 180)
    stub(r, "1", 149.86, esp)

# --- module sockets: row B is the module's left-hand column and row A its
# right-hand one -- the sheet is laid out to match the DevKit pinout you would
# look up
U1A_N = {"1":"ESP_P20","9":"ESP_TX","10":"RX_DIV","14":"GND","15":"+3V3"}
U1B_N = {"6":"LED_BT","7":"LED_DESK","14":"GND","15":"+5V"}
for ref, nets, x in (("U1B", U1B_N, 215.90), ("U1A", U1A_N, 259.08)):
    for p, n in nets.items():
        if n in ("GND", "+5V", "+3V3"):
            stub(ref, p, x + 2.54)                             # to its power symbol
        else:
            stub(ref, p, x, n, 180)

# ---- functional blocks: (x0, y0, x1, y1, title, subtitle) ----
BLOCKS = [
 ( 15.24, 15.24,  83.82,  86.36, "DESK CONNECTOR",  "RJ45 8P8C to the LoctekMotion control box"),
 ( 15.24, 93.98,  83.82, 132.08, "LED INDICATORS",  "LEDs are inside the jack, series limited"),
 ( 91.44, 15.24, 190.50,  60.96, "5 V INPUT",       "PTC, reverse block, bulk for the radio's start-up surge"),
 ( 91.44, 68.58, 190.50, 116.84, "RX LEVEL SHIFT",  "5 V desk -> 3.3 V ESP, clamped"),
 ( 91.44,124.46, 175.26, 160.02, "TX AND WAKE",     "series limit, 3.3 V needs no divider"),
 (198.12, 15.24, 287.02,  99.06, "ESP32 DevKit V1 30-pin socket",
                                 "pin names as silkscreened beside the holes"),
 (198.12,106.68, 287.02, 139.70, "MOUNTING",
                                 "4x M3, 71.8 x 28.8 pattern - H3/H4 sit under the module"),
]

# notes sit inside their block, left aligned with the title
NOTES = [( 17.78, 82.55, "pins 1-3 unused - the control box only uses 4..8"),
         ( 93.98,113.03, "R1/R2 divide, D2 clamps what is left to the 3V3 rail"),
         (200.66, 95.25, "RX2 = GPIO16    TX2 = GPIO17    D23 = GPIO23"),
         (200.66,135.89, "no test pads: every net is already on a through-hole pad")]

# ---- build ----
sch = Schematic.create_new(); sch.paper.paperSize = "A4"
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
                 effects=eff(hidden and not name_shown, just)),
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

# Global, not local: on the root sheet a local label names its net "/DESK_TX",
# and the board's nets carry no sheet path, so parity would call every one of
# them a conflict. "passive" keeps the label a plain box with no direction.
for t,x,y,a in LABELS:
    just = Justify(horizontally="right" if a in (180, 270) else "left")
    sch.globalLabels.append(GlobalLabel(text=t,position=Position(x,y,a),shape="passive",
        effects=Effects(font=Font(width=1.27,height=1.27), justify=just),uuid=str(uuid.uuid4())))

def left(size):
    return Effects(font=Font(width=size,height=size), justify=Justify(horizontally="left"))

# A KiCad Text is centred on its position unless told otherwise. Centring a long
# block title on the box's top-left corner is what threw every title half out of
# its own box and across its neighbour -- left-justify and inset instead.
for x0,y0,x1,y1,title,sub in BLOCKS:
    sch.shapes.append(Rectangle(start=Position(x0,y0), end=Position(x1,y1),
        stroke=Stroke(width=0.127, type="dash"), uuid=str(uuid.uuid4())))
    sch.texts.append(Text(text=title,position=Position(x0+2.54,y0+4.45,0),
        effects=left(1.8),uuid=str(uuid.uuid4())))
    sch.texts.append(Text(text=sub,position=Position(x0+2.54,y0+8.26,0),
        effects=left(1.2),uuid=str(uuid.uuid4())))
for x,y,t in NOTES:
    sch.texts.append(Text(text=t,position=Position(x,y,0),
        effects=left(1.2),uuid=str(uuid.uuid4())))

sch.sheetInstances.append(HierarchicalSheetInstance(instancePath="/",page="1"))
sch.to_file(OUT)
print(f"{len(sch.schematicSymbols)} symbols, {len(sch.graphicalItems)} wire segments "
      f"({split_count} splits), "
      f"{len(sch.junctions)} junctions, {len(sch.globalLabels)} labels, "
      f"{len(sch.noConnects)} no-connects")
