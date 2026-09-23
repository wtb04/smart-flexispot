# FlexiSpot / LoctekMotion BLE desk companion — rev1

Wouter ten Brinke · woutertenbrinke.nl

A carrier board that sockets an **ESP32 DevKit V1 (30-pin)** and plugs into a
LoctekMotion / FlexiSpot standing-desk control box over **RJ45**. The box
supplies 5 V and talks 5 V UART; the board level-shifts that to the ESP32,
which bridges it to BLE for the Tab5 panel. Firmware lives in [`../proxy`](../proxy).

| | |
|---|---|
| ![top](img/board-top.png) | ![bottom](img/board-bottom.png) |
| ![3d](img/board-3d.png) | ![schematic](img/schematic.png) |

**60 × 35.2 mm**, 2-layer, 1.6 mm FR4, black mask / white silk, 4× M3 on a
51.8 × 28.8 mm pattern. 18 footprints, 15 nets, 198 segments / 13 vias.
Every DevKit pin is named on the silkscreen beside its hole; the two LEDs built
into the RJ45 jack show BLE state (`BT`, yellow) and desk link (`LINK`, green).

```
ERC   0 violations
DRC   0 violations at all severities · 0 unconnected · 0 schematic-parity issues
      hole-to-hole enforced at JLCPCB's 0.5 mm, not KiCad's default
silk  0 text overlaps (check_sch.py) · every stroke ≥ 0.15 mm
```

## How it's wired

| RJ45 pin | control box | board | ESP32 |
|---|---|---|---|
| 8 | **5 V** | F1 PTC 500 mA → D1 SS14 → `+5V` | VIN |
| 7 | GND | pour | GND |
| 6 | box **TX** (5 V) | R1 10k / R2 15k divider, D2 BAT54S clamp to GND & 3V3 | GPIO16 (RX2) |
| 5 | box **RX** | R3 220 Ω | GPIO17 (TX2) |
| 4 | wake ("PIN 20") | R4 220 Ω | GPIO23 (D23) |
| 12 / 11 | LED anode / cathode | R5 220 Ω | GPIO32 (D32) — `BT` |
| 10 / 9 | LED anode / cathode | R6 220 Ω | GPIO33 (D33) — `LINK` |

Pins 4–8 were **metered on the actual control box**, not read from a table. The
divider is set for a box that measures 5.0–5.2 V: RX_DIV sits at 3.0–3.1 V,
with ~0.5 V of margin to both the ESP32's logic-high threshold and its 3.6 V
absolute maximum. Pins 1–3 are unused.

**Firmware on this board:** `CONFIG_LOCTEK_TX_GPIO=17`, `RX_GPIO=16`, `WAKE_GPIO=23`,
LEDs on 32/33. The flying-lead breadboard the firmware was developed on had TX/RX
the other way round — see the note in `../proxy/sdkconfig.defaults`.

## Files

| | |
|---|---|
| `tab5-desk-ctrl.kicad_pcb` / `.kicad_sch` / `.kicad_pro` | the board, the schematic, the project — all **generated**, don't hand-edit |
| `gen_pcb.py` | placement, outline, GND pour, antenna keepout, silk |
| `route.py` | 2-layer maze router, 0.1 mm grid |
| `finish.py` | reference placement, padstack fix, zone fill, drill origin — runs in KiCad's own python |
| `gen_sch.py` · `make_symbols.py` · `symbols/` | schematic from the same netlist; project symbols with the DevKit pin names |
| `link_sch.py` | ties footprints to symbols so `--schematic-parity` can check them |
| `check_sch.py` | flags overlapping schematic text — ERC doesn't |
| `make_fab.sh` · `make_bom.py` | gerbers, drills, BOM, CPL into `fab/` |
| `fab/` | the package as ordered for rev1 |
| `3dmodels/` | LCSC model for the RJ45, so the 3D view shows the real jack |
| `datasheet-RJ45-*.pdf` | the jack the footprint was verified against |
| `HANDOFF.md` | everything non-obvious: decisions, traps found, what to verify |

## Rebuild

```bash
pip install -r requirements.txt          # kiutils, numpy
KP=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3

python3 make_symbols.py
python3 gen_pcb.py && python3 route.py && $KP finish.py tab5-desk-ctrl.kicad_pcb
python3 gen_sch.py && $KP link_sch.py tab5-desk-ctrl.kicad_pcb tab5-desk-ctrl.kicad_sch
python3 check_sch.py tab5-desk-ctrl.kicad_sch
SCH_PY=python3 ./make_fab.sh
```

Idempotent — a placement change is one constant and a rerun. **Run the whole
chain every time**: `route.py` round-trips the board through kiutils and drops
the edits `finish.py` made on the previous pass.

## BOM

| Ref | Part | LCSC | |
|---|---|---|---|
| RJ45_1 | Ckmtw R-RJ45R08P-C000, 8P8C shielded, 2 LEDs | C386757 | hand |
| U1A, U1B | 1×15 female header 2.54 mm | C124408 | hand |
| F1 | PTC 0.5 A hold, 15 V (SMD1206P050TF/15) | C106264 | |
| D1 | SS14, DO-214AC | C2480 | |
| D2 | **BAT54S** — series variant; A and C break RX | C7420333 | |
| R1 | 10 kΩ 0805 | C17414 | |
| R2 | 15 kΩ 0805 | C17475 | |
| R3–R6 | 220 Ω 0805 | C17557 | |
| C1 | 22 µF 25 V X5R 1206 | C12891 | |
| C2 | 100 nF 50 V X7R 0805 | C49678 | |
| H1–H4 | M3 | — | |

## Ordering

Upload `fab/*-gerbers.zip`, `*-bom.csv`, `*-cpl.csv` to JLCPCB with Economic
SMT assembly, top side. `*-hand-solder.csv` is the three parts you fit yourself
— buy them from LCSC and combine shipping with the JLCPCB order (procedure in
HANDOFF). On the order form pick **black mask, white silk, 1.6 mm, tented vias,
"remove mark", "confirm production file"** — the stackup in the file is
KiCad-internal and JLC does not read it.

Before ordering, do the two checks nothing automated can: print
`fab/*-check-1to1.pdf` at 100 % and offer up the jack and the DevKit, and
confirm the diode on the parts screen says *BAT54S · series*.
