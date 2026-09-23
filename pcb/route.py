#!/usr/bin/env python3
"""Route tab5-desk-ctrl.kicad_pcb.

Two-layer maze router on a 0.1 mm grid. GND is never routed: through-hole GND
pads reach the B.Cu pour directly, SMD GND pads get a single via each.
Verify the result with:
    kicad-cli pcb drc --severity-error tab5-desk-ctrl.kicad_pcb
"""
import heapq, math, os, sys, uuid
import numpy as np
from kiutils.board import Board
from kiutils.items.common import Position
from kiutils.items.brditems import Segment, Via

PCB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tab5-desk-ctrl.kicad_pcb")
W, H, GRID = 60.0, 35.2, 0.1
NX, NY = int(W / GRID) + 1, int(H / GRID) + 1
EDGE = 0.5                      # board rule: copper-to-edge clearance
CLEAR = 0.25                     # router clearance (DRC rule is 0.20)
KEEPOUT = (19.5, 7.1, 33.0, 28.4)
# An M3 pan head with a washer is about 6 mm across and clamps down on whatever
# is under it, with only soldermask between steel and copper. DRC only knows
# about the drill, so it happily routes to 0.37 mm of a mounting hole -- keep
# copper a washer radius clear instead.
SCREW_KEEP = 3.0
POWER = {"+5V", "DESK_5V", "D5V_F"}
VIA_D, VIA_DRILL = 0.6, 0.3
HOLE2HOLE = 0.3                 # board rule is 0.2495; keep a margin
VIA_COST = 700                   # ~7 mm of track: keeps the via count down
LAYERS = ["F.Cu", "B.Cu"]
# ESP_P20 goes early: it is the longest awkward run (row A pin 1 sits at the far
# end, past the antenna keepout) and it loses if the top channel fills up first
ORDER = ["ESP_P20", "+5V", "DESK_5V", "D5V_F", "+3V3", "DESK_TX",
         "LED_BT", "LED_BT_A", "LED_DESK", "LED_DESK_A",
         "RX_DIV", "DESK_RX", "ESP_TX", "PIN20", "GND"]


def rot_offset(px, py, deg):
    a = math.radians(deg)
    return (px * math.cos(a) + py * math.sin(a), -px * math.sin(a) + py * math.cos(a))


board = Board.from_file(PCB)
netname = {n.number: n.name for n in board.nets}
netnum = {n.name: n.number for n in board.nets}

pads = []   # dict(ref, num, net, x, y, w, h, layers, tht)
for fp in board.footprints:
    fx, fy = fp.position.X, fp.position.Y
    rot = fp.position.angle or 0
    for p in fp.pads:
        ox, oy = rot_offset(p.position.X, p.position.Y, rot)
        sx, sy = (p.size.Y, p.size.X) if rot in (90, 270) else (p.size.X, p.size.Y)
        tht = p.type in ("thru_hole", "np_thru_hole")
        pads.append(dict(ref=fp.properties.get("Reference", "?"), num=str(p.number or ""),
                         net=(p.net.name if p.net else ""), x=fx + ox, y=fy + oy,
                         w=sx, h=sy, tht=tht,
                         mount=(p.type == "np_thru_hole"
                                and fp.properties.get("Reference", "").startswith("H")),
                         drill=(p.drill.diameter if p.drill else None),
                         lay=[0, 1] if tht else ([0] if "F.Cu" in p.layers else [1])))

routed = []   # (layer, x0, y0, x1, y1, width, net)
vias = []     # (x, y, net)


def rect_mask(mask, x0, y0, x1, y1):
    i0 = max(0, int(math.floor(y0 / GRID))); i1 = min(NY - 1, int(math.ceil(y1 / GRID)))
    j0 = max(0, int(math.floor(x0 / GRID))); j1 = min(NX - 1, int(math.ceil(x1 / GRID)))
    if i1 >= i0 and j1 >= j0:
        mask[i0:i1 + 1, j0:j1 + 1] = True


def seg_mask(mask, x0, y0, x1, y1, infl):
    n = max(2, int(math.hypot(x1 - x0, y1 - y0) / GRID) + 1)
    for k in range(n + 1):
        t = k / n
        rect_mask(mask, x0 + (x1 - x0) * t - infl, y0 + (y1 - y0) * t - infl,
                  x0 + (x1 - x0) * t + infl, y0 + (y1 - y0) * t + infl)


def build_obstacles(net, hw):
    """True = blocked, for a track of half-width hw belonging to `net`."""
    m = np.zeros((2, NY, NX), dtype=bool)
    infl = CLEAR + hw
    for L in (0, 1):
        mm = m[L]
        # the rule is edge-to-COPPER, so the centreline has to stay back by the
        # clearance plus half the track width, not just the clearance
        e = EDGE + hw
        mm[:, :int(e / GRID) + 1] = True
        mm[:, int((W - e) / GRID):] = True
        mm[:int(e / GRID) + 1, :] = True
        mm[int((H - e) / GRID):, :] = True
        rect_mask(mm, KEEPOUT[0] - hw, KEEPOUT[1] - hw, KEEPOUT[2] + hw, KEEPOUT[3] + hw)
    for p in pads:
        if p["net"] == net:
            continue
        # a mounting hole is not just its drill: nothing may sit under the screw
        pw = max(p["w"] / 2, SCREW_KEEP) if p.get("mount") else p["w"] / 2
        ph = max(p["h"] / 2, SCREW_KEEP) if p.get("mount") else p["h"] / 2
        for L in p["lay"]:
            rect_mask(m[L], p["x"] - pw - infl, p["y"] - ph - infl,
                      p["x"] + pw + infl, p["y"] + ph + infl)
    for (L, x0, y0, x1, y1, w, n) in routed:
        if n == net:
            continue
        seg_mask(m[L], x0, y0, x1, y1, infl + w / 2)
    for (x, y, n) in vias:
        if n == net:
            continue
        for L in (0, 1):
            rect_mask(m[L], x - VIA_D / 2 - infl, y - VIA_D / 2 - infl,
                      x + VIA_D / 2 + infl, y + VIA_D / 2 + infl)
    # Hole-to-hole spacing is a drill rule: it applies even between pads on the
    # SAME net, which the clearance masks above deliberately ignore. Without
    # this a GND via parks next to a GND shield pad and breaks the drill rule.
    if abs(hw - VIA_D / 2) < 1e-9:
        for p in pads:
            if not p.get("drill"):
                continue
            keep = p["drill"] / 2 + VIA_DRILL / 2 + HOLE2HOLE
            for L in (0, 1):
                rect_mask(m[L], p["x"] - keep, p["y"] - keep, p["x"] + keep, p["y"] + keep)
    return m


def pad_cells(p):
    """Cells safely inside the pad. Shrunk so a path that ends here really sits
    on copper -- the bounding-box corners of a round pad do not."""
    out = []
    sh = 0.2
    hw = max(GRID, p["w"] / 2 - sh); hh = max(GRID, p["h"] / 2 - sh)
    i0 = max(0, int(round((p["y"] - hh) / GRID))); i1 = min(NY - 1, int(round((p["y"] + hh) / GRID)))
    j0 = max(0, int(round((p["x"] - hw) / GRID))); j1 = min(NX - 1, int(round((p["x"] + hw) / GRID)))
    ci, cj = int(round(p["y"] / GRID)), int(round(p["x"] / GRID))
    for L in p["lay"]:
        out.append((L, ci, cj))
        for i in range(i0, i1 + 1):
            for j in range(j0, j1 + 1):
                out.append((L, i, j))
    return out


def owning_pad(cell, cands):
    """Which of `cands` does this path endpoint sit in?"""
    L, i, j = cell
    x, y = j * GRID, i * GRID
    best, bd = None, 1e9
    for p in cands:
        if L not in p["lay"]:
            continue
        d = math.hypot(x - p["x"], y - p["y"])
        if d < bd:
            best, bd = p, d
    return best


NB = [(-1, 0, 10), (1, 0, 10), (0, -1, 10), (0, 1, 10),
      (-1, -1, 14), (-1, 1, 14), (1, -1, 14), (1, 1, 14)]


def astar(blocked, viamask, sources, targets, tpts):
    tset = set(targets)
    dist = {}
    pq = []
    for s in sources:
        if not blocked[s[0], s[1], s[2]]:
            dist[s] = 0
            heapq.heappush(pq, (0, 0, s))
    def h(n):
        _, i, j = n
        return int(min(math.hypot((j - tj), (i - ti)) for ti, tj in tpts) * 10)
    while pq:
        f, g, cur = heapq.heappop(pq)
        if g > dist.get(cur, 1 << 60):
            continue
        if cur in tset:
            return cur, dist
        L, i, j = cur
        for di, dj, c in NB:
            ni, nj = i + di, j + dj
            if not (0 <= ni < NY and 0 <= nj < NX) or blocked[L, ni, nj]:
                continue
            nn = (L, ni, nj)
            ng = g + c
            if ng < dist.get(nn, 1 << 60):
                dist[nn] = ng
                heapq.heappush(pq, (ng + h(nn), ng, nn))
        nn = (1 - L, i, j)
        # a via is 0.6 mm across, far wider than a 0.25 mm track, so it needs its
        # own clearance check -- sizing it like the track under-reserves copper
        if not viamask[0, i, j] and not viamask[1, i, j]:
            ng = g + VIA_COST
            if ng < dist.get(nn, 1 << 60):
                dist[nn] = ng
                heapq.heappush(pq, (ng + h(nn), ng, nn))
    return None, dist


def backtrack(dist, end, blocked):
    path = [end]
    cur = end
    while dist[cur] != 0:
        L, i, j = cur
        best, bg = None, dist[cur]
        for di, dj, c in NB:
            pv = (L, i + di, j + dj)
            if pv in dist and dist[pv] + c == dist[cur]:
                best, bg = pv, dist[pv]; break
        if best is None:
            pv = (1 - L, i, j)
            if pv in dist and dist[pv] + VIA_COST == dist[cur]:
                best = pv
        if best is None:
            for di, dj, c in NB:
                pv = (L, i + di, j + dj)
                if pv in dist and dist[pv] < dist[cur]:
                    best = pv; break
        if best is None:
            break
        path.append(best); cur = best
    return path[::-1]


def emit(path, net, width):
    """Turn a cell path into merged segments + vias."""
    run = [path[0]]
    for nxt in path[1:]:
        if nxt[0] != run[-1][0]:                      # layer change -> via
            flush(run, net, width)
            vias.append((run[-1][2] * GRID, run[-1][1] * GRID, net))
            run = [nxt]
        else:
            run.append(nxt)
    flush(run, net, width)


def flush(run, net, width):
    if len(run) < 2:
        return
    L = run[0][0]
    pts = [run[0]]
    for k in range(1, len(run) - 1):
        a, b, c = run[k - 1], run[k], run[k + 1]
        if (b[1] - a[1], b[2] - a[2]) != (c[1] - b[1], c[2] - b[2]):
            pts.append(b)
    pts.append(run[-1])
    for a, b in zip(pts, pts[1:]):
        routed.append((L, a[2] * GRID, a[1] * GRID, b[2] * GRID, b[1] * GRID, width, net))


# A net with one pad routes trivially and passes DRC as connected -- which is how
# a missing series resistor hides. Refuse to build if any net is orphaned.
declared = {p["net"] for p in pads if p["net"]}
orphans = sorted(n for n in declared if sum(1 for p in pads if p["net"] == n) < 2)
if orphans:
    sys.exit(f"ERROR: net(s) with fewer than two pads: {orphans}")

fails = []
for net in ORDER:
    group = [p for p in pads if p["net"] == net]
    if len(group) < 2:
        continue
    width = 0.3 if net in POWER else 0.25
    hw = width / 2
    conn = [group[0]]
    rest = group[1:]
    while rest:
        blocked = build_obstacles(net, hw)
        viamask = build_obstacles(net, VIA_D / 2)
        src = [c for p in conn for c in pad_cells(p)]
        # nearest remaining pad first
        rest.sort(key=lambda p: min(math.hypot(p["x"] - q["x"], p["y"] - q["y"]) for q in conn))
        tgt_pad = rest[0]
        tgt = [c for c in pad_cells(tgt_pad)]
        tpts = [(int(tgt_pad["y"] / GRID), int(tgt_pad["x"] / GRID))]
        end, dist = astar(blocked, viamask, src, tgt, tpts)
        if end is None:
            fails.append((net, tgt_pad["ref"] + "." + tgt_pad["num"]))
            rest.pop(0); continue
        path = backtrack(dist, end, blocked)
        emit(path, net, width)
        # tie both ends to the exact pad centres: the grid endpoint can sit a
        # cell short of the copper, which reads as an unconnected pad in DRC
        for cell, cands in ((path[0], conn), (end, [tgt_pad])):
            op = owning_pad(cell, cands)
            if op and (abs(op["x"] - cell[2] * GRID) > 1e-9 or abs(op["y"] - cell[1] * GRID) > 1e-9):
                routed.append((cell[0], op["x"], op["y"], cell[2] * GRID, cell[1] * GRID, width, net))
        conn.append(rest.pop(0))
    print(f"  routed {net}: {len(group)} pads")

# GND is routed above like any other net, so connectivity is guaranteed by copper
# rather than by whichever pour islands survive the fill. The B.Cu pour still
# carries the bulk of the return current; these tracks just make it deterministic.
# The MST can arrive at the same spot on two branches and stack vias, or land
# them closer than the hole-to-hole minimum. Merge any cluster (same net, centres
# under 0.6 mm apart) into one hole and stitch the dropped positions to the kept
# one on both layers, so nothing loses its connection.
merged, kept = [], []
for (x, y, n) in vias:
    hit = next((k for k in kept if k[2] == n and math.hypot(k[0] - x, k[1] - y) < 0.6), None)
    if hit is None:
        kept.append((x, y, n))
    else:
        merged.append(((x, y), hit))
for (x, y), (kx, ky, n) in merged:
    if math.hypot(kx - x, ky - y) > 1e-9:
        for L in (0, 1):
            routed.append((L, x, y, kx, ky, 0.3, n))
if merged:
    print(f"  merged {len(merged)} stacked/too-close via(s)")
vias = kept

board.traceItems = []
for (L, x0, y0, x1, y1, w, n) in routed:
    board.traceItems.append(Segment(start=Position(round(x0, 4), round(y0, 4)),
                                    end=Position(round(x1, 4), round(y1, 4)),
                                    width=w, layer=LAYERS[L], net=netnum[n],
                                    tstamp=str(uuid.uuid4())))
for (x, y, n) in vias:
    board.traceItems.append(Via(position=Position(round(x, 4), round(y, 4)), size=VIA_D,
                                drill=VIA_DRILL, layers=["F.Cu", "B.Cu"], net=netnum[n],
                                tstamp=str(uuid.uuid4())))
board.to_file(PCB)
print(f"\n{len(routed)} segments, {len(vias)} vias")
if fails:
    print("UNROUTED:", fails); sys.exit(1)
print("all nets routed")
