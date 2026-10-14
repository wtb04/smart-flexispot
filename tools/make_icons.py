#!/usr/bin/env python3
"""Rasterises the panel's own marks into an LVGL A8 header.

The few the design kit's Material Symbols have no match for: the bar's Wi-Fi,
phone and laptop marks, and the radar's planes and missing photo. A few
rectangles, circles and polygons, sampled sixteen times a pixel so the curves
do not stair-step.
Alpha only: the panel recolours them, the same way it recolours the map.
"""
import pathlib

SUB = 4


def rounded(x0, y0, x1, y1, r):
    def inside(x, y):
        if not (x0 <= x <= x1 and y0 <= y <= y1):
            return False
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return inside


def disc(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def stroke(x0, y0, x1, y1, width):
    def inside(x, y):
        dx, dy = x1 - x0, y1 - y0
        t = max(0, min(1, ((x - x0) * dx + (y - y0) * dy) / (dx * dx + dy * dy)))
        return (x - x0 - t * dx) ** 2 + (y - y0 - t * dy) ** 2 <= (width / 2) ** 2
    return inside


def fan(cx, cy, inner, outer, half_deg):
    """A ring cut down to the wedge either side of straight up, as wifi draws."""
    import math
    limit = math.radians(half_deg)
    def inside(x, y):
        dx, dy = x - cx, cy - y
        return inner * inner <= dx * dx + dy * dy <= outer * outer and dy > 0 and \
            abs(math.atan2(dx, dy)) <= limit
    return inside


def polygon(*points):
    def inside(x, y):
        hit = False
        for (x0, y0), (x1, y1) in zip(points, points[1:] + points[:1]):
            if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
                hit = not hit
        return hit
    return inside


def struck(layers, x0, y0, x1, y1):
    """The one way anything here is shown off: a slash from top left to bottom
    right, with a cut either side so it stays a slash and not a smear."""
    return layers + [(stroke(x0, y0, x1, y1, 7), False), (stroke(x0, y0, x1, y1, 2.5), True)]


def paint(layers, side, height=None):
    """Each layer is (shape, ink), later ones over earlier: ink paints it in, True
    fully and a fraction faintly, and no ink cuts it out. `side` wide, `height`
    tall when not square."""
    out = bytearray()
    for py in range(height or side):
        for px in range(side):
            hit = 0.0
            for sy in range(SUB):
                for sx in range(SUB):
                    x = px + (sx + 0.5) / SUB
                    y = py + (sy + 0.5) / SUB
                    on = 0.0
                    for shape, ink in layers:
                        if shape(x, y):
                            on = float(ink)
                    hit += on
            out.append(round(255 * hit / (SUB * SUB)))
    return bytes(out)


NO_PHOTO_SIDE = 40
NO_PHOTO = paint(struck([(rounded(3, 11, 37, 34, 5), True),
                         (rounded(13, 6, 27, 13, 2), True),
                         (disc(20, 22.5, 8), False),
                         (disc(20, 22.5, 5), True)], 5, 4, 35, 38),
                 NO_PHOTO_SIDE)

STATUS_SIDE = 28
WIFI_LAYERS = [(disc(14, 23, 3), True),
               (fan(14, 23, 8, 11.5, 45), True),
               (fan(14, 23, 15, 18.5, 45), True)]
# An iPhone 15 Pro from the front: a hairline round a faintly lit screen, the
# Dynamic Island at its top and the home bar at its foot. Stouter than the real
# one, which at this size reads as a sliver.
PHONE_LAYERS = [(rounded(6.5, 2, 21.5, 26, 2.8), True),
                (rounded(7.6, 3.1, 20.4, 24.9, 1.9), 0.28),
                (rounded(11.8, 4.1, 16.2, 5.6, 0.75), True),
                (rounded(11.3, 22.9, 16.7, 23.8, 0.45), True)]
# A MacBook open, in the same hand: the lid's hairline round a faintly lit
# screen, on a base a little wider than the lid. No notch: the screen holds
# how many laptops share when more than one does.
LAPTOP_LAYERS = [(rounded(3, 4, 25, 21, 2.4), True),
                 (rounded(4.1, 5.1, 23.9, 21, 1.5), 0.28),
                 (rounded(1, 20.5, 27, 23.2, 1.1), True)]
WIFI = paint(WIFI_LAYERS, STATUS_SIDE)
WIFI_OFF = paint(struck(WIFI_LAYERS, 4, 3, 24, 26), STATUS_SIDE)
PHONE = paint(PHONE_LAYERS, STATUS_SIDE)
PHONE_OFF = paint(struck(PHONE_LAYERS, 4, 3, 24, 26), STATUS_SIDE)
LAPTOP = paint(LAPTOP_LAYERS, STATUS_SIDE)
LAPTOP_OFF = paint(struck(LAPTOP_LAYERS, 4, 3, 24, 26), STATUS_SIDE)

PLANE_SIDE = 28


def plane_layers(k):
    """The plane at 28 pixels, scaled by k."""
    def at(*points):
        return polygon(*[(x * k, y * k) for x, y in points])
    return [(stroke(14 * k, 3 * k, 14 * k, 24 * k, 4 * k), True),
            (at((12.5, 10), (15.5, 10), (27, 17), (27, 19.5), (15.5, 16), (12.5, 16),
                (1, 19.5), (1, 17)), True),
            (at((13, 21), (15, 21), (20.5, 25), (20.5, 26.5), (15, 25), (13, 25),
                (7.5, 26.5), (7.5, 25)), True)]


PLANE = paint(plane_layers(1), PLANE_SIDE)
# The same plane, faint behind the radar's spinner while a photo is on its way.
PLANE_LARGE_SIDE = 70
PLANE_LARGE = paint(plane_layers(PLANE_LARGE_SIDE / PLANE_SIDE), PLANE_LARGE_SIDE)


def emit(name, data, side, height=None):
    rows = []
    for at in range(0, len(data), 16):
        rows.append("    " + " ".join(f"0x{b:02x}," for b in data[at:at + 16]))
    body = "\n".join(rows)
    return f"""
const std::uint8_t {name.upper()}_PIXELS[] = {{
{body}
}};

const lv_image_dsc_t {name.lower()}_icon = {{
    .header = {{.magic = LV_IMAGE_HEADER_MAGIC,
               .cf = LV_COLOR_FORMAT_A8,
               .flags = 0,
               .w = {side},
               .h = {height or side},
               .stride = {side},
               .reserved_2 = 0}},
    .data_size = sizeof({name.upper()}_PIXELS),
    .data = {name.upper()}_PIXELS,
    .reserved = nullptr,
    .reserved_2 = nullptr,
}};
"""


out = pathlib.Path(__file__).resolve().parent.parent / "components/ui/fonts/icons.h"
out.write_text(f"""#pragma once

// Generated by tools/make_icons.py -- do not edit.

#include "lvgl.h"

#include <cstdint>

namespace ui {{
namespace icons {{
{emit("no_photo", NO_PHOTO, NO_PHOTO_SIDE)}{emit("wifi", WIFI, STATUS_SIDE)}{emit("wifi_off", WIFI_OFF, STATUS_SIDE)}{emit("phone", PHONE, STATUS_SIDE)}{emit("phone_off", PHONE_OFF, STATUS_SIDE)}{emit("laptop", LAPTOP, STATUS_SIDE)}{emit("laptop_off", LAPTOP_OFF, STATUS_SIDE)}{emit("plane", PLANE, PLANE_SIDE)}{emit("plane_large", PLANE_LARGE, PLANE_LARGE_SIDE)}
}}  // namespace icons
}}  // namespace ui
""")
print(f"wrote {out}")
