#!/usr/bin/env python3
"""Rasterises the panel's icons into an LVGL A8 header.

There is no icon font here with a train or a bus in it, and building one needs a
node toolchain that is not on this machine. These are a few rectangles and
circles instead, sampled sixteen times a pixel so the curves do not stair-step.
Alpha only: the panel recolours them, the same way it recolours the map.
"""
import pathlib

SIDE = 22
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


def paint(layers, side=SIDE):
    """Each layer is (shape, ink): ink paints it in, no ink cuts it out, in order."""
    out = bytearray()
    for py in range(side):
        for px in range(side):
            hit = 0
            for sy in range(SUB):
                for sx in range(SUB):
                    x = px + (sx + 0.5) / SUB
                    y = py + (sy + 0.5) / SUB
                    on = False
                    for shape, ink in layers:
                        if shape(x, y):
                            on = ink
                    hit += on
            out.append(round(255 * hit / (SUB * SUB)))
    return bytes(out)


def render(solid, holes):
    return paint([(s, True) for s in solid] + [(h, False) for h in holes])


# A carriage seen from the side: a rounded roof, a deep windscreen, a skirt over
# the bogies, and a gap under it so the wheels read as wheels.
TRAIN = render(
    solid=[rounded(5, 1, 17, 15, 4),       # body, roof rounded
           rounded(4, 14, 18, 16.5, 1),    # skirt
           rounded(6.5, 18, 9.5, 20.5, 1), # near bogie
           rounded(12.5, 18, 15.5, 20.5, 1)],
    holes=[rounded(6.5, 3.5, 15.5, 9, 2),  # windscreen
           rounded(10.6, 3, 11.4, 9.5, 0), # its centre pillar
           rounded(7, 10.5, 9, 13, 0.5),   # door
           rounded(13, 10.5, 15, 13, 0.5)],
)

# A bus is longer, flatter and sits lower, with a row of windows rather than one
# screen: that difference is what has to read at this size.
BUS = render(
    solid=[rounded(1, 4, 21, 15, 2),
           rounded(1, 14, 21, 16.5, 1),
           rounded(4, 18, 7, 20.5, 1),
           rounded(15, 18, 18, 20.5, 1)],
    holes=[rounded(2.5, 5.5, 8, 9.5, 1),
           rounded(9.5, 5.5, 15, 9.5, 1),
           rounded(16.5, 5.5, 19.5, 9.5, 1),
           rounded(2.5, 11, 19.5, 11.8, 0)],
)

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
PHONE_LAYERS = [(rounded(6.5, 2, 21.5, 26, 3.5), True),
                (rounded(8.8, 4.5, 19.2, 20.5, 1), False)]
CALENDAR = paint([(rounded(3, 5, 25, 25.5, 3.5), True),
                  (rounded(5.5, 11.5, 22.5, 23, 1.5), False),
                  (stroke(9.5, 2.5, 9.5, 7.5, 3.2), False),
                  (stroke(18.5, 2.5, 18.5, 7.5, 3.2), False),
                  (stroke(9.5, 2.5, 9.5, 7.5, 2), True),
                  (stroke(18.5, 2.5, 18.5, 7.5, 2), True)]
                 + [(rounded(x, y, x + 3.2, y + 3.2, 0.8), True)
                    for x in (7.6, 12.4, 17.2) for y in (13.6, 18.2)],
                 28)

PLANE_SIDE = 28
PLANE = paint([(stroke(14, 3, 14, 24, 4), True),
               (polygon((12.5, 10), (15.5, 10), (27, 17), (27, 19.5), (15.5, 16), (12.5, 16),
                        (1, 19.5), (1, 17)), True),
               (polygon((13, 21), (15, 21), (20.5, 25), (20.5, 26.5), (15, 25), (13, 25),
                        (7.5, 26.5), (7.5, 25)), True)],
              PLANE_SIDE)
DESK = paint([(rounded(2, 5, 26, 9, 2), True),
              (rounded(7, 8, 10, 23, 0), True),
              (rounded(18, 8, 21, 23, 0), True),
              (rounded(3.5, 22, 13.5, 25, 1.5), True),
              (rounded(14.5, 22, 24.5, 25, 1.5), True)],
             STATUS_SIDE)
# One geometry for the chips' and screws' marks, so every plus is the same plus.
MARK_SIDE = 16
MARK_ARM = 6.5
MARK_STROKE = 2.6
PLUS = paint([(stroke(8 - MARK_ARM, 8, 8 + MARK_ARM, 8, MARK_STROKE), True),
              (stroke(8, 8 - MARK_ARM, 8, 8 + MARK_ARM, MARK_STROKE), True)], MARK_SIDE)
MINUS = paint([(stroke(8 - MARK_ARM, 8, 8 + MARK_ARM, 8, MARK_STROKE), True)], MARK_SIDE)
TIMES = paint([(stroke(4.2, 4.2, 11.8, 11.8, 2.0), True),
               (stroke(4.2, 11.8, 11.8, 4.2, 2.0), True)], MARK_SIDE)
WIFI = paint(WIFI_LAYERS, STATUS_SIDE)
WIFI_OFF = paint(struck(WIFI_LAYERS, 4, 3, 24, 26), STATUS_SIDE)
PHONE = paint(PHONE_LAYERS, STATUS_SIDE)
PHONE_OFF = paint(struck(PHONE_LAYERS, 4, 3, 24, 26), STATUS_SIDE)


WALK = paint([(disc(12.5, 3.2, 2.4), True),
              (stroke(11.5, 7.5, 10, 13.5, 3.2), True),
              (stroke(10.8, 9, 15, 11.5, 2.2), True),
              (stroke(11, 9, 7.2, 12, 2.2), True),
              (stroke(10, 13.5, 13.5, 17, 2.8), True),
              (stroke(13.5, 17, 14.5, 21, 2.8), True),
              (stroke(10, 13.5, 7.5, 21, 2.8), True)])


def ring(cx, cy, outer, inner):
    return [(disc(cx, cy, outer), True), (disc(cx, cy, inner), False)]


BIKE = paint(ring(5.2, 14.5, 4.4, 2.8) + ring(16.8, 14.5, 4.4, 2.8)
             + [(stroke(5.2, 14.5, 9.5, 8, 1.8), True),
                (stroke(9.5, 8, 15.5, 8, 1.8), True),
                (stroke(15.5, 8, 16.8, 14.5, 1.8), True),
                (stroke(9.5, 8, 11, 14.5, 1.8), True),
                (stroke(5.2, 14.5, 11, 14.5, 1.8), True),
                (stroke(11, 14.5, 15.5, 8, 1.8), True),
                (stroke(8, 5.5, 11, 5.5, 2), True),
                (stroke(9.5, 5.5, 9.5, 8, 1.8), True),
                (stroke(14.5, 4.5, 15.5, 8, 1.8), True),
                (stroke(13.5, 4.5, 16.5, 4.5, 2), True)])


# A stopwatch: the dial, the crown it is started by, and one hand.
TIMER = paint(ring(14, 16, 10.5, 8) +
              [(rounded(11, 1.5, 17, 4.5, 1), True),
               (rounded(12.8, 4, 15.2, 6.5, 0), True),
               (stroke(21.2, 6.2, 23.4, 8.4, 2.4), True),
               (stroke(14, 16, 14, 10.5, 2.4), True),
               (disc(14, 16, 1.8), True)],
              28)


def emit(name, data, side=SIDE):
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
               .h = {side},
               .stride = {side},
               .reserved_2 = 0}},
    .data_size = sizeof({name.upper()}_PIXELS),
    .data = {name.upper()}_PIXELS,
    .reserved = nullptr,
    .reserved_2 = nullptr,
}};
"""


out = pathlib.Path(__file__).resolve().parent.parent / "components/ui/icons.h"
out.write_text(f"""#pragma once

// Generated by tools/make_icons.py -- do not edit.

#include "lvgl.h"

#include <cstdint>

namespace ui {{
namespace icons {{
{emit("train", TRAIN)}{emit("bus", BUS)}{emit("walk", WALK)}{emit("bike", BIKE)}{emit("no_photo", NO_PHOTO, NO_PHOTO_SIDE)}{emit("wifi", WIFI, STATUS_SIDE)}{emit("wifi_off", WIFI_OFF, STATUS_SIDE)}{emit("phone", PHONE, STATUS_SIDE)}{emit("phone_off", PHONE_OFF, STATUS_SIDE)}{emit("plane", PLANE, PLANE_SIDE)}{emit("calendar", CALENDAR, 28)}{emit("desk", DESK, STATUS_SIDE)}{emit("timer", TIMER, 28)}{emit("plus", PLUS, MARK_SIDE)}{emit("minus", MINUS, MARK_SIDE)}{emit("times", TIMES, MARK_SIDE)}
}}  // namespace icons
}}  // namespace ui
""")
print(f"wrote {out}")
