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
# block stays clear of that corner. Three columns, butted up on a common grid:
#   left    15.24 ..  88.90   desk side
#   middle  96.52 .. 172.72   signal conditioning
#   right  180.34 .. 287.02   module, probes, mounting

# ref: (lib, symbol, x, y, angle, mirror, value, footprint)
PLACE = {
 # 8P8C_LED_Shielded: signal pins 1-8 on the right, LED pins 9-12 on the left,
 # shield at the bottom -- matches the Ckmtw jack's pin numbering exactly
 "RJ45_1": ("Connector","8P8C_LED_Shielded", 52.07, 55.88, 0,None,"RJ45 8P8C 2xLED",
            "Connector_RJ:RJ45_Connfly_DS1128-09-S8xx-S_Horizontal"),
 "R5": ("Device","R", 48.26,125.73,270,None,"220R","Resistor_SMD:R_0805_2012Metric"),
 "R6": ("Device","R", 48.26,144.78,270,None,"220R","Resistor_SMD:R_0805_2012Metric"),
 "F1": ("Device","Polyfuse",   123.19, 33.02, 90,None,"PTC 500mA","Resistor_SMD:R_1206_3216Metric"),
 "D1": ("Device","D_Schottky", 146.05, 33.02,  0,"y","SS14","Diode_SMD:D_SMA"),
 "C1": ("Device","C",          152.40, 43.18,  0,None,"22uF 25V","Capacitor_SMD:C_1206_3216Metric"),
 "C2": ("Device","C",          168.91, 43.18,  0,None,"100nF 50V","Capacitor_SMD:C_0805_2012Metric"),
 "R1": ("Device","R",          123.19, 83.82, 90,None,"10k","Resistor_SMD:R_0805_2012Metric"),
 "R2": ("Device","R",          137.16, 96.52,  0,None,"15k","Resistor_SMD:R_0805_2012Metric"),
 "D2": ("Diode","BAT54S",      152.40,102.87,  0,"x","BAT54S","Package_TO_SOT_SMD:SOT-23"),
 "R3": ("Device","R",          123.19,140.97,270,None,"220R","Resistor_SMD:R_0805_2012Metric"),
 "R4": ("Device","R",          123.19,160.02,270,None,"220R","Resistor_SMD:R_0805_2012Metric"),
 "U1A": ("tab5-desk-ctrl","ESP32_DevKitV1_RowA", 264.16, 50.80, 0,None,"Socket 1x15 2.54mm",
         "Connector_PinSocket_2.54mm:PinSocket_1x15_P2.54mm_Vertical"),
 "U1B": ("tab5-desk-ctrl","ESP32_DevKitV1_RowB", 215.90, 50.80, 0,None,"Socket 1x15 2.54mm",
         "Connector_PinSocket_2.54mm:PinSocket_1x15_P2.54mm_Vertical"),
 "H1": ("Mechanical","MountingHole", 195.58, 97.79, 0,None,"M3","MountingHole:MountingHole_3.2mm_M3"),
 "H2": ("Mechanical","MountingHole", 218.44, 97.79, 0,None,"M3","MountingHole:MountingHole_3.2mm_M3"),
 "H3": ("Mechanical","MountingHole", 241.30, 97.79, 0,None,"M3","MountingHole:MountingHole_3.2mm_M3"),
 "H4": ("Mechanical","MountingHole", 264.16, 97.79, 0,None,"M3","MountingHole:MountingHole_3.2mm_M3"),
}
# power symbols, each with its PWR_FLAG alongside rather than in a block of their own
for ref,(sym,x,y) in {"#PWR08":("GND", 19.05, 83.82), "#PWR09":("GND", 52.07, 83.82),
                      "#PWR10":("GND", 68.58, 83.82), "#PWR02":("GND",152.40, 52.07),
                      "#PWR03":("GND",168.91, 52.07), "#PWR04":("GND",137.16,107.95),
                      "#PWR05":("GND",144.78,110.49),
                      "#PWR01":("+5V",158.75, 27.94), "#PWR06":("+3V3",160.02, 92.71),
                      "#FLG01":("PWR_FLAG",163.83, 27.94),   # +5V rail
                      "#FLG02":("PWR_FLAG", 80.01, 48.26),   # GND at the desk connector
                      "#FLG03":("PWR_FLAG",170.18,102.87),   # +3V3 off the module
                      }.items():
    # GND uses the project's own symbol: the three-bar earth glyph, but with
    # Value "GND" so the net keeps its name. power:Earth / power:GNDREF have the
    # right drawing and the wrong name -- a power symbol's Value IS the net.
    if sym == "GND":
        PLACE[ref] = ("tab5-desk-ctrl","GND_EARTH",x,y,0,None,"GND","")
    else:
        PLACE[ref] = ("power",sym,x,y,0,None,sym,"")

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

# --- DESK CONNECTOR: signals right, LEDs left, grounds down ---------------
for p in ("4","5","6","8"):
    w(P("RJ45_1",p), (72.39, P("RJ45_1",p)[1]))
w(P("RJ45_1","7"), (68.58, P("RJ45_1","7")[1]))
w((68.58, P("RJ45_1","7")[1]), (68.58, 83.82))                 # GND symbol
w((68.58, P("RJ45_1","7")[1]), (80.01, P("RJ45_1","7")[1]))    # PWR_FLAG
# Datasheet (both the PC-board-layout and the bottom view) marks the four LED
# pads "+ - + -" left to right with pin 1 on the right. KiCad's footprint draws
# the part rotated 180 deg (pin 1 on the left), so in its numbering the anodes
# are pads 10 and 12, not 9 and 11.
for p in ("10","12"):
    w(P("RJ45_1",p), (35.56, P("RJ45_1",p)[1]))
w(P("RJ45_1","9"),  (19.05, P("RJ45_1","9")[1]))               # LED cathodes
w(P("RJ45_1","11"), (19.05, P("RJ45_1","11")[1]))
w((19.05, P("RJ45_1","9")[1]), (19.05, 83.82))
w(P("RJ45_1","SH"), (52.07, 83.82))                            # shield to GND

# --- LED INDICATORS -------------------------------------------------------
w((31.75,125.73), P("R5","2"));  w(P("R5","1"), (67.31,125.73))
w((31.75,144.78), P("R6","2"));  w(P("R6","1"), (67.31,144.78))

# --- 5 V INPUT ------------------------------------------------------------
w((109.22,33.02), P("F1","1"))
w(P("F1","2"), (130.81,33.02));  w((130.81,33.02), P("D1","2"))
w((130.81,33.02), (130.81,41.91))                              # D5V_F label stub
w(P("D1","1"), (168.91,33.02))                                 # +5V rail
w((158.75,33.02), (158.75,27.94))                              # +5V symbol
w((163.83,33.02), (163.83,27.94))                              # PWR_FLAG
w((152.40,33.02), P("C1","1"));  w(P("C1","2"), (152.40,52.07))
w((168.91,33.02), P("C2","1"));  w(P("C2","2"), (168.91,52.07))

# --- RX LEVEL SHIFT -------------------------------------------------------
w((109.22,83.82), P("R1","1"))
w(P("R1","2"), (161.29,83.82))                                 # RX_DIV node
w((137.16,83.82), P("R2","1"));  w(P("R2","2"), (137.16,107.95))
w((152.40,83.82), P("D2","3"))
w(P("D2","1"), (144.78,110.49))                                # clamp to GND
w(P("D2","2"), (160.02, 92.71))                                # clamp to +3V3
w(P("D2","2"), (170.18, P("D2","2")[1]))                       # PWR_FLAG

# --- TX AND WAKE ----------------------------------------------------------
w((109.22,140.97), P("R3","2"));  w(P("R3","1"), (146.05,140.97))
w((109.22,160.02), P("R4","2"));  w(P("R4","1"), (146.05,160.02))

# --- module socket stubs --------------------------------------------------
U1A_N = {"1":"ESP_P20","9":"ESP_TX","10":"RX_DIV","14":"GND","15":"+3V3"}
U1B_N = {"6":"LED_BT","7":"LED_DESK","14":"GND","15":"+5V"}
# row B is the module's left-hand column and row A its right-hand one -- the
# sheet is laid out to match the DevKit pinout you would look up
for p in U1B_N: w(P("U1B",p), (195.58, P("U1B",p)[1]))
for p in U1A_N: w(P("U1A",p), (243.84, P("U1A",p)[1]))

# ---- labels: net name at the far end of a stub ----
LABELS = [("DESK_5V", 72.39, P("RJ45_1","8")[1], 0),
          ("DESK_TX", 72.39, P("RJ45_1","6")[1], 0),
          ("DESK_RX", 72.39, P("RJ45_1","5")[1], 0),
          ("PIN20",   72.39, P("RJ45_1","4")[1], 0),
          ("LED_DESK_A", 35.56, P("RJ45_1","10")[1], 180),
          ("LED_BT_A",   35.56, P("RJ45_1","12")[1], 180),
          ("LED_BT_A", 31.75,125.73,180), ("LED_BT",   67.31,125.73,0),
          ("LED_DESK_A",31.75,144.78,180),("LED_DESK", 67.31,144.78,0),
          ("DESK_5V",109.22, 33.02,180), ("D5V_F",  130.81, 41.91,270),
          ("DESK_TX",109.22, 83.82,180), ("RX_DIV", 161.29, 83.82,0),
          ("DESK_RX",109.22,140.97,180), ("ESP_TX", 146.05,140.97,0),
          ("PIN20",  109.22,160.02,180), ("ESP_P20",146.05,160.02,0)]
for p,n in U1B_N.items(): LABELS.append((n,195.58,P("U1B",p)[1],180))
for p,n in U1A_N.items(): LABELS.append((n,243.84,P("U1A",p)[1],180))

# ---- functional blocks: (x0, y0, x1, y1, title, subtitle) ----
BLOCKS = [
 ( 15.24, 15.24,  88.90,  96.52, "DESK CONNECTOR",  "RJ45 8P8C to the LoctekMotion control box"),
 ( 15.24,104.14,  88.90, 156.21, "LED INDICATORS",  "LEDs are inside the jack, series limited"),
 ( 96.52, 15.24, 172.72,  57.15, "5 V INPUT",       "PTC, reverse block, bulk"),
 ( 96.52, 64.77, 172.72, 117.48, "RX LEVEL SHIFT",  "5 V desk -> 3.3 V ESP, clamped"),
 ( 96.52,119.38, 172.72, 171.45, "TX AND WAKE",     "series limit, 3.3 V needs no divider"),
 (180.34, 15.24, 287.02,  74.93, "ESP32 DevKit V1 30-pin socket",
                                 "pin names as silkscreened beside the holes"),
 (180.34, 82.55, 287.02, 111.76, "MOUNTING",
                                 "4x M3, 51.8 x 29.2 pattern - H3/H4 sit under the module"),
]

# notes sit inside their block, left aligned with the title
NOTES = [( 17.78, 92.71, "pins 1-3 unused - the control box only uses 4..8"),
         ( 99.06,114.30, "R1/R2 divide, D2 clamps what is left to the 3V3 rail"),
         (182.88, 71.12, "RX2 = GPIO16    TX2 = GPIO17    D23 = GPIO23"),
         (182.88,106.68, "no test pads: every net is already on a through-hole pad")]

# ---- build ----
sch = Schematic.create_new(); sch.paper.paperSize = "A4"
sch.titleBlock = TitleBlock(title="FlexiSpot / LoctekMotion BLE desk companion",
                            revision="rev1", company="Wouter ten Brinke",
                            comments={1: "ESP32 DevKit V1 30-pin carrier, RJ45 to the control box"})
seen = set()
for ref,(lib,name,x,y,ang,mir,val,fp) in PLACE.items():
    key = f"{lib}:{name}"
    if key not in seen:
        s = proto(lib,name); s.libId = key; s.libraryNickname, s.entryName = lib, name
        sch.libSymbols.append(s); seen.add(key)
    hidden = ref.startswith("#") or ref.startswith("TP")
    inst = SchematicSymbol(libraryNickname=lib, entryName=name,
                           position=Position(x,y,ang), unit=1,   # angle must be written even when 0
                           inBom=not (hidden or ref.startswith("TP") or ref.startswith("H")),
                           onBoard=True, mirror=mir, uuid=str(uuid.uuid4()))
    eff = lambda hide: Effects(font=Font(width=1.27,height=1.27), hide=hide)
    # NOTE: Property's third positional is `id`, not `position` -- passing a
    # Position there writes a malformed file that KiCad simply refuses to open.
    # Ref and value go clear of the body: above a tall connector, above and
    # below a rotated two-pin part, beside an upright one.
    if name == "8P8C_LED_Shielded":
        rx, ry0, ry1 = x, y - 20.32, y - 17.78
    elif name.startswith("ESP32_DevKitV1"):
        rx, ry0, ry1 = x, y - 24.13, y - 21.59
    elif ang in (90, 270):
        # KiCad rotates a field with its symbol, so these read bottom-to-top and
        # need 6.35 of clearance -- and rows of rotated parts need 19.05 between
        # them or one part's value lands on the next one's reference.
        rx, ry0, ry1 = x, y - 6.35, y + 6.35
    elif ref in ("C1", "C2"):
        rx, ry0, ry1 = x - 7.62, y - 2.54, y + 2.54
    elif ref == "D1":
        # fields to the left, clear of C1's tap off the rail
        rx, ry0, ry1 = x - 7.62, y - 2.54, y + 2.54
    elif ref == "D2":
        # SOT-23 body is 15 mm wide in the symbol; +5.08 lands inside it
        rx, ry0, ry1 = x + 10.16, y - 2.54, y + 2.54
    else:
        rx, ry0, ry1 = x + 5.08, y - 2.54, y + 2.54
    inst.properties = [
        Property(key="Reference", value=ref, position=Position(rx,ry0,0), effects=eff(hidden)),
        Property(key="Value",     value=val, position=Position(rx,ry1,0), effects=eff(hidden)),
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

for t,x,y,a in LABELS:
    sch.globalLabels.append(GlobalLabel(text=t,position=Position(x,y,a),shape="input",
        effects=Effects(font=Font(width=1.27,height=1.27)),uuid=str(uuid.uuid4())))

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
