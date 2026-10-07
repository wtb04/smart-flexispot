#!/usr/bin/env python3
"""Builds the design kit's fonts: Instrument Sans for text, Material Symbols Rounded for icons,
and Montserrat for the few letters Instrument Sans lacks.

The first two come as variable fonts, which lv_font_conv cannot pick an instance from,
so fontTools cuts the static ones first. Needs fontTools (pip install fonttools),
node's npx on PATH for lv_font_conv, and a build done once, for the Montserrat in
LVGL's managed component. Run from the repository's root:

    tools/make_fonts.py
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys
import urllib.parse
import urllib.request

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ROOT = pathlib.Path(__file__).resolve().parent.parent
FONTS = ROOT / "components" / "ui" / "fonts"
LV_FONT_CONV = "lv_font_conv@1.5.3"

TEXT_URL = "https://raw.githubusercontent.com/google/fonts/main/ofl/instrumentsans/InstrumentSans[wdth,wght].ttf"
ICON_URL = ("https://raw.githubusercontent.com/google/material-design-icons/master/variablefont/"
            "MaterialSymbolsRounded[FILL,GRAD,opsz,wght]")
MONTSERRAT = ROOT / "managed_components/lvgl__lvgl/scripts/generators/built_in_font/Montserrat-Medium.ttf"

TEXT = ([*range(0x20, 0x7F), *range(0xA0, 0x100), *range(0x100, 0x180)]
        + [ord(c) for c in "–—‘’“”…•→←↑↓−€µ₂"])
# Units beside a readout are set in a smaller font, so the big sizes carry only these.
READOUT = [ord(c) for c in " 0123456789+,-.:%°−"]

# The text roles fall back on these, at the nearest size Montserrat comes in;
# see kit_fonts.cpp. Each carries only what the text fonts do not.
UNITS = [*range(0xA0, 0x180), *range(0x2010, 0x2028), *range(0x2030, 0x203B), 0x20AC, 0x3BC]
UNITS_SIZES = (16, 20, 22, 28, 32)

# role: (weight, size, range)
ROLES = {
    "label": (600, 15, TEXT),
    "small": (500, 19, TEXT),
    "body": (500, 23, TEXT),
    "h3": (600, 25, TEXT),
    "h2": (600, 30, TEXT),
    "h1": (600, 36, TEXT),
    "read_m": (600, 40, READOUT),
    "read_l": (600, 52, READOUT),
    "read_xl": (600, 64, READOUT),
    "read_xxl": (600, 140, READOUT),
}

ICON_SIZES = (24, 28, 34)
ICONS = """
home calendar_month flight timer lightbulb wifi smartphone battery_full battery_5_bar
battery_2_bar bolt play_arrow pause stop skip_previous skip_next replay_10 forward_10
volume_up volume_down subtitles open_in_full close_fullscreen close add remove check
chevron_right chevron_left keyboard_arrow_up keyboard_arrow_down my_location desk wall_lamp
floor_lamp light bedtime settings palette tune monitor_heart restart_alt power_settings_new
air directions_bike train directions_bus directions_walk schedule event music_note info
aspect_ratio local_laundry_service notifications warning error check_circle sync
arrow_upward arrow_downward visibility_off
""".split()


def fetch(url, cache):
    local = cache / urllib.parse.unquote(url.rsplit("/", 1)[1])
    if not local.exists():
        cache.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(urllib.parse.quote(url, safe=":/"), local)
    return local


def freeze(font, feature):
    """Points the cmap at what a single-substitution feature would swap in, so it is always on."""
    gsub = font["GSUB"].table
    swap = {}
    for record in gsub.FeatureList.FeatureRecord:
        if record.FeatureTag != feature:
            continue
        for index in record.Feature.LookupListIndex:
            for table in gsub.LookupList.Lookup[index].SubTable:
                if table.LookupType == 7:
                    table = table.ExtSubTable
                if table.LookupType != 1:
                    sys.exit(f"{feature} uses lookup type {table.LookupType}, which this cannot freeze")
                swap.update(table.mapping)
    for table in font["cmap"].tables:
        for code, glyph in table.cmap.items():
            table.cmap[code] = swap.get(glyph, glyph)


def text_instance(source, weight, cache):
    out = cache / f"InstrumentSans-{weight}-tnum.ttf"
    font = instancer.instantiateVariableFont(TTFont(source), {"wdth": 100, "wght": weight})
    freeze(font, "tnum")
    font.save(out)
    return out


def icon_instance(source, codepoints, filled, cache):
    out = cache / f"MaterialSymbolsRounded-{'filled' if filled else 'outlined'}.ttf"
    font = TTFont(source)
    # Cut down to the icons first: instancing all of its thousands takes minutes.
    options = subset.Options()
    options.layout_features = []
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=codepoints)
    subsetter.subset(font)
    font = instancer.instantiateVariableFont(font, {"FILL": int(filled), "GRAD": 0, "opsz": 24, "wght": 400})
    font.save(out)
    return out


def ranges(codes):
    runs = []
    for code in sorted(set(codes)):
        if runs and runs[-1][1] == code - 1:
            runs[-1][1] = code
        else:
            runs.append([code, code])
    return ",".join(f"0x{a:X}" if a == b else f"0x{a:X}-0x{b:X}" for a, b in runs)


def convert(font, size, codes, name):
    # Relative paths, so the command lv_font_conv writes into the file reads the same on any machine.
    subprocess.run(["npx", "--yes", LV_FONT_CONV, "--font", os.path.relpath(font, FONTS),
                    "--size", str(size), "--bpp", "4", "--no-compress", "--format", "lvgl",
                    "--range", ranges(codes), "--lv-include", "lvgl.h", "-o", f"{name}.c"],
                   cwd=FONTS, check=True, stdout=subprocess.DEVNULL)
    return FONTS / f"{name}.c"


SIZES = {"uint8_t": 1, "int8_t": 1, "uint16_t": 2, "lv_font_fmt_txt_glyph_dsc_t": 8,
         "lv_font_fmt_txt_cmap_t": 20, "lv_font_fmt_txt_kern_pair_t": 0,
         "lv_font_fmt_txt_kern_classes_t": 0}


def flash_bytes(path):
    """Roughly what a font's tables take in flash, from the arrays in its source."""
    total = 0
    for kind, body in re.findall(r"const (\w+) \w+\[\] =\s*\{(.*?)\n\};", path.read_text(), re.S):
        if kind.endswith("_t") and kind.startswith("lv_"):
            total += SIZES.get(kind, 0) * len(re.findall(r"\{", body))
        else:
            body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
            total += SIZES.get(kind, 1) * len(re.findall(r"-?(?:0x[0-9a-fA-F]+|\d+)", body))
    return total


def icon_header(names, codepoints):
    lines = ["#pragma once", "",
             "// Generated by tools/make_fonts.py: Material Symbols Rounded's codepoints, in UTF-8,",
             "// for a label set in ui::kit::fonts::icon().", "namespace ui::kit::glyph {"]
    for name in names:
        utf8 = "".join(f"\\x{b:02X}" for b in chr(codepoints[name]).encode())
        lines.append(f'inline constexpr const char *{name} = "{utf8}";')
    lines += ["}  // namespace ui::kit::glyph", ""]
    (FONTS / "kit_icons.h").write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cache", default=str(ROOT / ".font-cache"))
    cache = pathlib.Path(parser.parse_args().cache).resolve()

    text_source = fetch(TEXT_URL, cache)
    weights = {weight: text_instance(text_source, weight, cache) for weight, _, _ in ROLES.values()}
    have = set(TTFont(weights[600]).getBestCmap())
    missing = sorted({code for _, _, codes in ROLES.values() for code in codes} - have)

    icon_source = fetch(ICON_URL + ".ttf", cache)
    codepoints = {}
    for line in fetch(ICON_URL + ".codepoints", cache).read_text().splitlines():
        name, code = line.split()
        codepoints.setdefault(name, int(code, 16))
    unknown = [name for name in ICONS if name not in codepoints]
    if unknown:
        sys.exit(f"not in Material Symbols: {' '.join(unknown)}")
    icon_codes = [codepoints[name] for name in ICONS]
    styles = {style: icon_instance(icon_source, icon_codes, style == "f", cache) for style in "of"}

    made = []
    for role, (weight, size, codes) in ROLES.items():
        made.append(convert(weights[weight], size, [c for c in codes if c in have], f"lv_font_kit_{role}"))
    text = {c for c in TEXT if c in have}
    montserrat = TTFont(MONTSERRAT).getBestCmap()
    fallback = [c for c in UNITS if c in montserrat and c not in text]
    for size in UNITS_SIZES:
        made.append(convert(MONTSERRAT, size, fallback, f"lv_font_units_{size}"))
    for style, font in styles.items():
        for size in ICON_SIZES:
            made.append(convert(font, size, icon_codes, f"lv_font_kit_icon_{style}_{size}"))
    icon_header(ICONS, codepoints)

    total = 0
    for path in made:
        size = flash_bytes(path)
        total += size
        print(f"{path.name:32} {size / 1024:7.1f} KiB")
    print(f"{'total':32} {total / 1024:7.1f} KiB in flash, about")
    if missing:
        print("not in Instrument Sans, so left out:",
              " ".join(f"U+{code:04X} {chr(code)}" for code in missing))


if __name__ == "__main__":
    main()
