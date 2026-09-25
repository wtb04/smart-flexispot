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


def paint(layers, side=SIDE, height=None):
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
# An iPhone 15 Pro from the front: a hairline round a faintly lit screen, the
# Dynamic Island at its top and the home bar at its foot. Stouter than the real
# one, which at this size reads as a sliver.
PHONE_LAYERS = [(rounded(6.5, 2, 21.5, 26, 2.8), True),
                (rounded(7.6, 3.1, 20.4, 24.9, 1.9), 0.28),
                (rounded(11.8, 4.1, 16.2, 5.6, 0.75), True),
                (rounded(11.3, 22.9, 16.7, 23.8, 0.45), True)]
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


# An update on its way in: an arrow down into what it is for, the screen or
# the companion's little box with its cable.
def arrow_down(cx, top, bottom):
    return [(stroke(cx, top, cx, bottom - 3.5, 2.6), True),
            (polygon((cx - 5, bottom - 5), (cx + 5, bottom - 5), (cx, bottom)), True)]


UPDATE_PANEL = paint([(rounded(2, 4, 26, 21, 3), True),
                      (rounded(4.5, 6.5, 23.5, 18.5, 1.5), False),
                      (rounded(10, 23, 18, 25.5, 1.2), True)]
                     + arrow_down(14, 8, 17.5), STATUS_SIDE)
UPDATE_COMPANION = paint([(rounded(6, 3, 22, 20, 3), True),
                          (rounded(8.5, 5.5, 19.5, 17.5, 1.5), False),
                          (stroke(14, 20, 14, 26, 2.6), True)]
                         + arrow_down(14, 7, 16.5), STATUS_SIDE)

# The image going on to the companion, over Bluetooth: its box filled, with the
# rune cut out, so it reads apart from the outlined boxes of an image arriving.
def rune(cx, top, bottom, width):
    mid, reach = (top + bottom) / 2, (bottom - top) / 4
    return [stroke(cx, top, cx, bottom, width),
            stroke(cx, top, cx + reach * 1.3, top + reach, width),
            stroke(cx + reach * 1.3, top + reach, cx - reach * 1.3, mid + reach, width),
            stroke(cx - reach * 1.3, mid - reach, cx + reach * 1.3, bottom - reach, width),
            stroke(cx + reach * 1.3, bottom - reach, cx, bottom, width)]


INSTALL_COMPANION = paint([(rounded(6, 3, 22, 20, 3), True),
                           (stroke(14, 20, 14, 26, 2.6), True)]
                          + [(part, False) for part in rune(14, 5, 18, 1.6)], STATUS_SIDE)


# A bulb in two layers over the same ground, the glass and the base, so each can
# take its own colour: a globe narrowing into its neck over a threaded base.
BULB_W, BULB_H = 34, 48
BULB_GLASS = paint([(disc(17, 15, 14.5), True),
                    (polygon((5.2, 21), (28.8, 21), (23.6, 33), (10.4, 33)), True)],
                   BULB_W, BULB_H)
BULB_BASE = paint([(rounded(10, 34.5, 24, 45, 2), True),
                   (stroke(10, 39.8, 24, 39.8, 1.4), False)], BULB_W, BULB_H)


# A standing desk: its top, the long feet, and legs of three telescoping tubes,
# each slimmer out of the one below. Sitting they are shorter, never all the
# way in.
DESK_W, DESK_H = 58, 56
LEG_X = (13, 45)
FOOT_TOP = 50
TUBES = ((2.1, 0.0), (2.8, 0.3), (3.6, 0.6))  # half-width, and where it starts down the leg
RAISED_TOP, LOWERED_TOP = 5, 20


def desk_layers(top):
    layers = [(rounded(1, top, 57, top + 6.5, 2.5), True),
              (rounded(3, 49.5, 23, 54, 2.2), True),
              (rounded(35, 49.5, 55, 54, 2.2), True)]
    leg_top = top + 6
    span = FOOT_TOP - leg_top
    for x in LEG_X:
        for half, start in TUBES:
            layers.append((rounded(x - half, leg_top + span * start, x + half, FOOT_TOP, 0.5), True))
    return layers


DESK_UP = paint(desk_layers(RAISED_TOP), DESK_W, DESK_H)
DESK_DOWN = paint(desk_layers(LOWERED_TOP), DESK_W, DESK_H)


# Ten seconds back or on: a circle turning the one way or the other, its arrow
# at the top, and the 10 inside it.
SEEK_SIDE = 56


def arc(cx, cy, outer, inner, gap_from, gap_to):
    """A ring less the part between two angles, clockwise from straight up."""
    import math
    def inside(x, y):
        dx, dy = x - cx, y - cy
        if not inner * inner <= dx * dx + dy * dy <= outer * outer:
            return False
        angle = math.degrees(math.atan2(dx, -dy)) % 360
        return not (gap_from <= angle <= gap_to)
    return inside


def seek_layers(forward):
    import math
    cx, cy, r, w = 28, 29, 21, 3.4
    # The gap sits just past the top, where the arrow turns into it.
    gap = (20, 58) if forward else (302, 340)
    tip = gap[0] if forward else gap[1]
    ax = cx + r * math.sin(math.radians(tip - (6 if forward else -6)))
    ay = cy - r * math.cos(math.radians(tip - (6 if forward else -6)))
    head = 7.5
    along = 1 if forward else -1  # the way the arrow points, along the circle
    tx, ty = ax + along * head, ay
    layers = [(arc(cx, cy, r + w / 2, r - w / 2, *gap), True),
              (polygon((tx, ty), (ax - along * 1.5, ay - head * 0.85), (ax - along * 1.5, ay + head * 0.85)), True)]
    # The 10, drawn rather than set, so it sits in the circle's middle.
    layers += [(stroke(21.5, 23, 21.5, 36, 3), True), (stroke(21.5, 23, 18.5, 25.8, 2.6), True)]
    layers += [(disc(31.5, 29.5, 6.8), True), (rounded(29.2, 25.6, 33.8, 33.4, 2.3), False)]
    return layers


SEEK_BACK = paint(seek_layers(False), SEEK_SIDE)
SEEK_ON = paint(seek_layers(True), SEEK_SIDE)


# Four corners pulled out, as a picture going fullscreen.
EXPAND = paint([(stroke(x0, y0, x1, y1, 2.6), True) for x0, y0, x1, y1 in (
    (4, 4, 11, 4), (4, 4, 4, 11), (24, 4, 17, 4), (24, 4, 24, 11),
    (4, 24, 11, 24), (4, 24, 4, 17), (24, 24, 17, 24), (24, 24, 24, 17))], STATUS_SIDE)


def emit(name, data, side=SIDE, height=None):
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


out = pathlib.Path(__file__).resolve().parent.parent / "components/ui/icons.h"
out.write_text(f"""#pragma once

// Generated by tools/make_icons.py -- do not edit.

#include "lvgl.h"

#include <cstdint>

namespace ui {{
namespace icons {{
{emit("train", TRAIN)}{emit("bus", BUS)}{emit("walk", WALK)}{emit("bike", BIKE)}{emit("no_photo", NO_PHOTO, NO_PHOTO_SIDE)}{emit("wifi", WIFI, STATUS_SIDE)}{emit("wifi_off", WIFI_OFF, STATUS_SIDE)}{emit("phone", PHONE, STATUS_SIDE)}{emit("phone_off", PHONE_OFF, STATUS_SIDE)}{emit("plane", PLANE, PLANE_SIDE)}{emit("plane_large", PLANE_LARGE, PLANE_LARGE_SIDE)}{emit("calendar", CALENDAR, 28)}{emit("desk", DESK, STATUS_SIDE)}{emit("timer", TIMER, 28)}{emit("update_panel", UPDATE_PANEL, STATUS_SIDE)}{emit("update_companion", UPDATE_COMPANION, STATUS_SIDE)}{emit("install_companion", INSTALL_COMPANION, STATUS_SIDE)}{emit("bulb_glass", BULB_GLASS, BULB_W, BULB_H)}{emit("bulb_base", BULB_BASE, BULB_W, BULB_H)}{emit("desk_up", DESK_UP, DESK_W, DESK_H)}{emit("desk_down", DESK_DOWN, DESK_W, DESK_H)}{emit("expand", EXPAND, STATUS_SIDE)}{emit("seek_back", SEEK_BACK, SEEK_SIDE)}{emit("seek_on", SEEK_ON, SEEK_SIDE)}{emit("plus", PLUS, MARK_SIDE)}{emit("minus", MINUS, MARK_SIDE)}{emit("times", TIMES, MARK_SIDE)}
}}  // namespace icons
}}  // namespace ui
""")
print(f"wrote {out}")
