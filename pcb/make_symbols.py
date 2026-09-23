#!/usr/bin/env python3
"""Generate symbols/tab5-desk-ctrl.kicad_sym -- the project's own symbols.

Two module-socket symbols carrying the DevKit V1 pin names (so the sheet reads
like the silkscreen), and a GND symbol drawn as the three-bar earth.

Why a custom GND: a KiCad power symbol's **Value is the net name**, so
power:Earth and power:GNDREF -- which have exactly the graphic wanted -- would
rename the net to "Earth"/"GNDREF" and break parity with the board. This is the
earth graphic with Value "GND", which is the only way to get both.

  python3 make_symbols.py
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "symbols", "tab5-desk-ctrl.kicad_sym")

ROW_A = ["D23","D22","TX0","RX0","D21","D19","D18","D5",
         "TX2","RX2","D4","D2","D15","GND","3V3"]
ROW_B = ["EN","VP","VN","D34","D35","D32","D33","D25",
         "D26","D27","D14","D12","D13","GND","VIN"]
PWR = {"GND", "3V3", "VIN"}
FONT = '(effects (font (size 1.27 1.27)))'


def socket(name, names, descr):
    o = [f'\t(symbol "{name}"',
         '\t\t(pin_names (offset 0.508))',
         '\t\t(exclude_from_sim no)(in_bom yes)(on_board yes)',
         f'\t\t(property "Reference" "U" (at 0 22.86 0){FONT})',
         f'\t\t(property "Value" "{name}" (at 0 20.32 0){FONT})',
         f'\t\t(property "Footprint" "" (at 0 0 0)(effects (font (size 1.27 1.27))(hide yes)))',
         f'\t\t(property "Datasheet" "" (at 0 0 0)(effects (font (size 1.27 1.27))(hide yes)))',
         f'\t\t(property "Description" "{descr}" (at 0 0 0)(effects (font (size 1.27 1.27))(hide yes)))',
         f'\t\t(symbol "{name}_0_1"',
         '\t\t\t(rectangle (start -5.08 19.05)(end 10.16 -19.05)',
         '\t\t\t\t(stroke (width 0.254)(type default))(fill (type background)))',
         '\t\t)',
         f'\t\t(symbol "{name}_1_1"']
    for i, nm in enumerate(names):
        o += [f'\t\t\t(pin {"power_in" if nm in PWR else "passive"} line '
              f'(at -10.16 {17.78 - i*2.54:g} 0)(length 5.08)',
              f'\t\t\t\t(name "{nm}" {FONT})',
              f'\t\t\t\t(number "{i+1}" {FONT}))']
    return "\n".join(o + ['\t\t)', '\t)'])


def gnd():
    """power:Earth's graphic -- stem plus three decreasing bars -- named GND."""
    bars = [[(0, 0), (0, -1.27)],
            [(1.27, -1.27), (-1.27, -1.27)],
            [(-0.635, -1.905), (0.635, -1.905)],
            [(-0.127, -2.54), (0.127, -2.54)]]
    o = ['\t(symbol "GND_EARTH"',
         '\t\t(power)',
         '\t\t(pin_numbers (hide yes))',
         '\t\t(pin_names (offset 0)(hide yes))',
         '\t\t(exclude_from_sim no)(in_bom yes)(on_board yes)',
         '\t\t(property "Reference" "#PWR" (at 0 -6.35 0)'
         '(effects (font (size 1.27 1.27))(hide yes)))',
         # Value IS the net name for a power symbol -- this is what keeps the
         # net called GND while the drawing uses the earth glyph.
         f'\t\t(property "Value" "GND" (at 0 -3.81 0){FONT})',
         '\t\t(property "Footprint" "" (at 0 0 0)(effects (font (size 1.27 1.27))(hide yes)))',
         '\t\t(property "Datasheet" "" (at 0 0 0)(effects (font (size 1.27 1.27))(hide yes)))',
         '\t\t(property "Description" "Power symbol, GND, drawn as the three-bar earth" '
         '(at 0 0 0)(effects (font (size 1.27 1.27))(hide yes)))',
         '\t\t(symbol "GND_EARTH_0_1"']
    for b in bars:
        pts = " ".join(f"(xy {x:g} {y:g})" for x, y in b)
        o += ['\t\t\t(polyline', f'\t\t\t\t(pts {pts})',
              '\t\t\t\t(stroke (width 0)(type default))(fill (type none)))']
    o += ['\t\t)',
          '\t\t(symbol "GND_EARTH_1_1"',
          '\t\t\t(pin power_in line (at 0 0 270)(length 0)',
          f'\t\t\t\t(name "~" {FONT})',
          f'\t\t\t\t(number "1" {FONT}))',
          '\t\t)', '\t)']
    return "\n".join(o)


os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w") as f:
    f.write('(kicad_symbol_lib (version 20241209) (generator "make_symbols") '
            '(generator_version "9.0")\n')
    f.write(socket("ESP32_DevKitV1_RowA", ROW_A,
                   "ESP32 DevKit V1 30-pin, row A (D23..3V3) -- 1x15 socket") + "\n")
    f.write(socket("ESP32_DevKitV1_RowB", ROW_B,
                   "ESP32 DevKit V1 30-pin, row B (EN..VIN) -- 1x15 socket") + "\n")
    f.write(gnd() + "\n)\n")
print(f"wrote {os.path.relpath(OUT, HERE)}: 2 socket symbols + GND_EARTH")
