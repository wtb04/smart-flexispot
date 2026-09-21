#!/usr/bin/env python3
"""Clip Natural Earth vectors to a box and emit the radar's map_data.h.

The panel draws coastlines, national borders and water around wherever Home
Assistant says home is, so the data has to cover a region rather than a point:
nothing here knows the panel's own position, and nothing here should.

    tools/make_map.py --out components/radar/map_data.h \
        --box 49.5 2.0 54.5 9.0 --tolerance 150
"""
import argparse
import json
import math
import os
import sys
import urllib.request

# Each layer carries its own simplification, in metres. Every path becomes a
# line object on the scope and every one of those costs internal memory at draw
# time -- enough of them starved the Wi-Fi co-processor's SDIO driver of receive
# buffers and brought the panel down. Coast and water are what the eye reads, so
# they stay fine; the political lines are context and can be coarse.
# Ocean and Lake are areas and are filled; the rest are lines. An area has to
# stay a closed ring through the clip, so those go through a polygon clipper
# rather than being cut into runs.
FILLS = {"Ocean", "Lake"}

LAYERS = [
    ("Ocean", "10m/physical/ne_10m_ocean.json", None, 300),
    ("Lake", "10m/physical/ne_10m_lakes.json", None, 150),
    ("River", "10m/physical/ne_10m_rivers_lake_centerlines.json", "scalerank", 300),
    ("Border", "10m/cultural/ne_10m_admin_0_boundary_lines_land.json", None, 600),
    ("Province", "10m/cultural/ne_10m_admin_1_states_provinces_lines.json", None, 1500),
]
BASE = "https://raw.githubusercontent.com/martynafford/natural-earth-geojson/master/"


def load(path, cache):
    local = os.path.join(cache, os.path.basename(path))
    if not os.path.exists(local):
        os.makedirs(cache, exist_ok=True)
        urllib.request.urlretrieve(BASE + path, local)
    with open(local) as handle:
        return json.load(handle)


def rings(geometry):
    """Every geometry reduced to a list of shapes, each a list of rings. A
    polygon's later rings are its holes -- the islands in a sea -- and have to
    stay with the ring they are cut out of, because the fill decides inside from
    outside by counting crossings across the whole shape at once."""
    kind = geometry["type"]
    coords = geometry["coordinates"]
    if kind == "LineString":
        return [[coords]]
    if kind == "MultiLineString":
        return [[line] for line in coords]
    if kind == "Polygon":
        return [coords]
    if kind == "MultiPolygon":
        return list(coords)
    return []


def clip_polygon(points, box):
    """Sutherland-Hodgman against the box. A filled shape has to come out of the
    clip still closed, which cutting it into runs does not."""
    min_lat, min_lon, max_lat, max_lon = box
    edges = (
        (lambda p: p[0] >= min_lon, lambda a, b: (min_lon, a[1] + (b[1] - a[1]) *
                                                  (min_lon - a[0]) / (b[0] - a[0]))),
        (lambda p: p[0] <= max_lon, lambda a, b: (max_lon, a[1] + (b[1] - a[1]) *
                                                  (max_lon - a[0]) / (b[0] - a[0]))),
        (lambda p: p[1] >= min_lat, lambda a, b: (a[0] + (b[0] - a[0]) *
                                                  (min_lat - a[1]) / (b[1] - a[1]), min_lat)),
        (lambda p: p[1] <= max_lat, lambda a, b: (a[0] + (b[0] - a[0]) *
                                                  (max_lat - a[1]) / (b[1] - a[1]), max_lat)),
    )
    output = points
    for keep, cross in edges:
        if not output:
            return []
        current, output = output, []
        previous = current[-1]
        for point in current:
            if keep(point):
                if not keep(previous):
                    output.append(cross(previous, point))
                output.append(point)
            elif keep(previous):
                output.append(cross(previous, point))
            previous = point
    return output


def clip(points, box):
    """Runs of the line that stay inside the box, with one point of overlap so
    a line crossing the edge still reaches it."""
    min_lat, min_lon, max_lat, max_lon = box
    inside = [min_lon <= lon <= max_lon and min_lat <= lat <= max_lat for lon, lat in points]
    runs, current = [], []
    for i, point in enumerate(points):
        near = inside[i] or (i > 0 and inside[i - 1]) or (i + 1 < len(inside) and inside[i + 1])
        if near:
            current.append(point)
        elif current:
            runs.append(current)
            current = []
    if current:
        runs.append(current)
    return [run for run in runs if len(run) >= 2]


def simplify(points, tolerance_deg):
    """Douglas-Peucker, iterative so a long coastline cannot blow the stack."""
    if len(points) < 3:
        return points
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        first, last = stack.pop()
        ax, ay = points[first]
        bx, by = points[last]
        worst, index = 0.0, -1
        for i in range(first + 1, last):
            px, py = points[i]
            dx, dy = bx - ax, by - ay
            if dx == 0 and dy == 0:
                distance = math.hypot(px - ax, py - ay)
            else:
                t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
                distance = math.hypot(px - (ax + t * dx), py - (ay + t * dy))
            if distance > worst:
                worst, index = distance, i
        if worst > tolerance_deg and index > 0:
            keep[index] = True
            stack.append((first, index))
            stack.append((index, last))
    return [p for p, k in zip(points, keep) if k]


def stitch(paths):
    """Natural Earth ships a border as hundreds of separate fragments, and each
    one becomes a line object on the panel, which is what it cannot afford.
    Fragments that share an endpoint are the same line, so they are joined."""
    remaining = [list(p) for p in paths]
    joined = []
    while remaining:
        current = remaining.pop()
        changed = True
        while changed:
            changed = False
            for i, other in enumerate(remaining):
                if current[-1] == other[0]:
                    current += other[1:]
                elif current[-1] == other[-1]:
                    current += other[-2::-1]
                elif current[0] == other[-1]:
                    current = other[:-1] + current
                elif current[0] == other[0]:
                    current = other[:0:-1] + current
                else:
                    continue
                remaining.pop(i)
                changed = True
                break
        joined.append(current)
    return joined


def length_km(points):
    total = 0.0
    for (ax, ay), (bx, by) in zip(points, points[1:]):
        total += math.hypot((bx - ax) * 111.0, (ax - bx) * 0.0 + (bx - ax) * 0.0)
        total += math.hypot((bx - ax) * 111.0 * math.cos(math.radians(ay)),
                            (by - ay) * 111.0)
    return total


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--box", nargs=4, type=float, required=True,
                        metavar=("MIN_LAT", "MIN_LON", "MAX_LAT", "MAX_LON"))
    parser.add_argument("--tolerance", type=float, default=0.0,
                        help="metres, a floor under every layer's own")
    parser.add_argument("--min-km", type=float, default=8.0,
                        help="drop anything shorter than this once joined")
    parser.add_argument("--cache", default=".map-cache")
    args = parser.parse_args()

    box = tuple(args.box)
    paths = []
    for layer, source, rank_field, layer_tolerance in LAYERS:
        data = load(source, args.cache)
        runs = []
        for feature in data["features"]:
            if feature.get("geometry") is None:
                continue
            if rank_field is not None:
                rank = feature["properties"].get(rank_field)
                if rank is not None and rank > 8:
                    continue  # minor watercourses are noise at this scale
                # Natural Earth runs a river's centreline straight through the
                # lakes it passes into, which draws the IJsselmeer as a river.
                if feature["properties"].get("featurecla") != "River":
                    continue
            for shape in rings(feature["geometry"]):
                if layer in FILLS:
                    kept = []
                    for ring in shape:
                        flat = [(float(c[0]), float(c[1])) for c in ring if len(c) >= 2]
                        closed = clip_polygon(flat, box)
                        if len(closed) >= 3:
                            kept.append(closed + [closed[0]])
                    if kept:
                        runs.append(kept)
                else:
                    for ring in shape:
                        flat = [(float(c[0]), float(c[1])) for c in ring if len(c) >= 2]
                        runs.extend([r] for r in clip(flat, box))

        tolerance = max(layer_tolerance, args.tolerance) / 111000.0
        if layer in FILLS:
            shapes = runs
        else:
            shapes = [[run] for run in stitch([r[0] for r in runs])]

        for shape in shapes:
            thinned = [simplify(ring, tolerance) for ring in shape]
            thinned = [ring for ring in thinned if len(ring) >= 2]
            if not thinned:
                continue
            if layer not in FILLS and length_km(thinned[0]) < args.min_km:
                continue
            paths.append((layer, thinned))

    vertices = sum(len(ring) for _, shape in paths for ring in shape)
    with open(args.out, "w") as out:
        out.write("// Generated by tools/make_map.py -- do not edit.\n")
        out.write("// Natural Earth, public domain. Box %s, %.0f m tolerance.\n"
                  % (list(box), args.tolerance))
        out.write("// %d paths, %d vertices.\n\n#pragma once\n\n#include <cstdint>\n\n"
                  % (len(paths), vertices))
        out.write("namespace radar {\n\nenum class MapLayer : std::uint8_t "
                  "{ Ocean, Coast, Border, Province, Lake, River };\n\nstruct MapPath {\n"
                  "    const std::int32_t  *points;  // lat, lon pairs in micro-degrees\n"
                  "    const std::uint16_t *rings;   // vertices in each ring, holes after the first\n"
                  "    std::uint16_t        count;\n"
                  "    std::uint8_t         ring_count;\n"
                  "    MapLayer             layer;\n};\n\n")
        for i, (_, shape) in enumerate(paths):
            out.write("inline constexpr std::int32_t kMapPoints%d[] = {" % i)
            out.write(",".join("%d,%d" % (round(lat * 1e6), round(lon * 1e6))
                               for ring in shape for lon, lat in ring))
            out.write("};\n")
            out.write("inline constexpr std::uint16_t kMapRings%d[] = {%s};\n"
                      % (i, ",".join(str(len(ring)) for ring in shape)))
        out.write("\ninline constexpr MapPath kMapPaths[] = {\n")
        for i, (layer, shape) in enumerate(paths):
            total = sum(len(ring) for ring in shape)
            out.write("    {kMapPoints%d, kMapRings%d, %d, %d, MapLayer::%s},\n"
                      % (i, i, total, len(shape), layer))
        out.write("};\n\ninline constexpr int kMapPathCount = %d;\n\n}  // namespace radar\n"
                  % len(paths))

    print("%d shapes, %d vertices, %d bytes of coordinates"
          % (len(paths), vertices, vertices * 8), file=sys.stderr)


if __name__ == "__main__":
    main()
