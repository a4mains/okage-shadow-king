"""Static waypoint planning over a field's XCL collision polygons.

Input is the JSONL from `okage-assets xcl-polygons MAP/a.xcl` (owned data).
Floor polygons (normal Y below -threshold; Y grows downward) are sampled on an
XZ grid; each cell keeps the floor heights found there. Neighbouring cells are
connected when a floor surface continues within `step` height. Cells whose
body column (from `body_low` to `body_high` units above the floor) meets a
wall polygon's footprint within `clearance` are blocked. A* over the grid gives
a path that is simplified to waypoints for ordinary walking
(home.walk_points / Ctl.walk_to).

This is a planning aid, not evidence: it ignores dynamic actors, scripted
collision groups (event 0x3f) and exact capsule contact, so the live walk
remains the proof. Automatic floor-event polygons (material kind 0x81) are
walkable but cost `trigger_cost` times more unless they carry the goal event;
most authored automatic events test variables and do nothing. Every event a
plan crosses is reported (`crossed`) so it can be checked against the scripts.
Confirm-only triggers (0x80) cost nothing extra.
"""
import heapq
import json
import math
import sys
from collections import defaultdict

def load(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]

def contains_xz(vs, x, z):
    inside = False
    j = len(vs) - 1
    for i in range(len(vs)):
        xi, zi = vs[i][0], vs[i][2]
        xj, zj = vs[j][0], vs[j][2]
        if (zi > z) != (zj > z) and x < (xj - xi) * (z - zi) / (zj - zi) + xi:
            inside = not inside
        j = i
    return inside

def plane_y(vs, x, z):
    (ax, ay, az), (bx, by, bz), (cx, cy, cz) = vs[0], vs[1], vs[2]
    ux, uy, uz = bx - ax, by - ay, bz - az
    vx, vy, vz = cx - ax, cy - ay, cz - az
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    if abs(ny) < 1e-9:
        return None
    return ay - (nx * (x - ax) + nz * (z - az)) / ny

def seg_dist(px, pz, ax, az, bx, bz):
    vx, vz = bx - ax, bz - az
    L = vx * vx + vz * vz
    t = 0.0 if L == 0 else max(0.0, min(1.0, ((px - ax) * vx + (pz - az) * vz) / L))
    return math.hypot(px - (ax + t * vx), pz - (az + t * vz))

class Grid:
    def __init__(self, polygons, goal_event=None, cell=1.0, threshold=8190, step=1.5,
                 body_low=0.4, body_high=3.0, clearance=0.3, trigger_cost=6.0):
        self.cell = cell
        self.step = step
        self.floors = defaultdict(list)      # (i, k) -> [y, ...] (Y grows downward)
        self.events = {}
        self.trigger_cost = trigger_cost
        self.blocked = set()                 # (i, k, rounded y)
        walls = []
        for p in polygons:
            vs = p['vertices']
            ny = p['normal'][1]
            if ny < -threshold:
                kind, event = p['material'] & 0x83, (p['material'] >> 8) & 0x3f
                auto = kind == 0x81 and event != goal_event
                for i, k in self._cells(vs, 0):
                    x, z = i * cell, k * cell
                    if contains_xz(vs, x, z):
                        y = plane_y(vs, x, z)
                        if y is not None:
                            self.floors[(i, k)].append(y)
                            if auto:
                                self.events[(i, k, round(y, 1))] = event
            elif abs(ny) < threshold:
                walls.append(vs)
        for vs in walls:
            ys = [v[1] for v in vs]
            top, bottom = min(ys), max(ys)
            edges = [(vs[a], vs[(a + 1) % len(vs)]) for a in range(len(vs))]
            for i, k in self._cells(vs, clearance):
                ys_here = self.floors.get((i, k))
                if not ys_here:
                    continue
                x, z = i * cell, k * cell
                if min(seg_dist(x, z, a[0], a[2], b[0], b[2]) for a, b in edges) > clearance:
                    continue
                for y in ys_here:
                    if bottom >= y - body_high and top <= y - body_low:
                        self.blocked.add((i, k, round(y, 1)))

    def _cells(self, vs, pad):
        c = self.cell
        xs = [v[0] for v in vs]
        zs = [v[2] for v in vs]
        for i in range(math.floor((min(xs) - pad) / c), math.ceil((max(xs) + pad) / c) + 1):
            for k in range(math.floor((min(zs) - pad) / c), math.ceil((max(zs) + pad) / c) + 1):
                yield i, k

    def node(self, x, z, y=None):
        i, k = round(x / self.cell), round(z / self.cell)
        ys = [y_ for y_ in self.floors.get((i, k), []) if (i, k, round(y_, 1)) not in self.blocked]
        if not ys:
            return None
        return (i, k, min(ys, key=lambda y_: abs(y_ - y)) if y is not None else max(ys))

    def neighbours(self, n):
        i, k, y = n
        for di in (-1, 0, 1):
            for dk in (-1, 0, 1):
                if not (di or dk):
                    continue
                for y2 in self.floors.get((i + di, k + dk), ()):
                    m = (i + di, k + dk, round(y2, 1))
                    if abs(y2 - y) <= self.step and m not in self.blocked:
                        cost = math.hypot(di, dk) * self.cell
                        yield (i + di, k + dk, y2), cost * (self.trigger_cost if m in self.events else 1.0)

    def path(self, start, goal, start_y=None, goal_y=None):
        s, g = self.node(*start, start_y), self.node(*goal, goal_y)
        if s is None or g is None:
            raise ValueError(f'start/goal not on an open floor cell: {start}->{s}, {goal}->{g}')
        gx, gz = g[0], g[1]
        best = {s: 0.0}
        prev = {}
        heap = [(0.0, 0.0, s)]
        while heap:
            _, cost, n = heapq.heappop(heap)
            if n[:2] == (gx, gz):
                out = [n]
                while out[-1] in prev:
                    out.append(prev[out[-1]])
                return [(n[0] * self.cell, n[1] * self.cell, n[2]) for n in out[::-1]]
            if cost > best.get(n, math.inf):
                continue
            for m, d in self.neighbours(n):
                c = cost + d
                if c < best.get(m, math.inf):
                    best[m] = c
                    prev[m] = n
                    heapq.heappush(heap, (c + math.hypot(m[0] - gx, m[1] - gz) * self.cell, c, m))
        return None

def simplify(points, spacing=8.0, turn_degrees=20.0):
    """Keep the endpoints, turns larger than turn_degrees and at most
    `spacing` units between kept points."""
    if len(points) <= 2:
        return list(points)
    out = [points[0]]
    for i in range(1, len(points) - 1):
        a, b, c = out[-1], points[i], points[i + 1]
        d1 = math.atan2(b[1] - a[1], b[0] - a[0])
        d2 = math.atan2(c[1] - b[1], c[0] - b[0])
        turn = abs((math.degrees(d2 - d1) + 180) % 360 - 180)
        if turn > turn_degrees or math.hypot(c[0] - a[0], c[1] - a[1]) > spacing:
            out.append(b)
    out.append(points[-1])
    return out

def plan(polygons, start, goal, goal_event=None, spacing=8.0, start_y=None, goal_y=None, **grid):
    """Returns {'waypoints': [...], 'crossed': [automatic events in order]} or None."""
    g = Grid(polygons, goal_event, **grid)
    route = g.path(start, goal, start_y, goal_y)
    if route is None:
        return None
    crossed = []
    for x, z, y in route:
        e = g.events.get((round(x / g.cell), round(z / g.cell), round(y, 1)))
        if e is not None and (not crossed or crossed[-1] != e):
            crossed.append(e)
    pts = [tuple(start)] + [(round(x, 1), round(z, 1)) for x, z, _ in route[1:-1]] + [tuple(goal)]
    return {'waypoints': simplify(pts, spacing), 'crossed': crossed}

def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('polygons')
    ap.add_argument('--start', type=float, nargs=2, required=True)
    ap.add_argument('--start-y', type=float)
    ap.add_argument('--goal', type=float, nargs=2, required=True)
    ap.add_argument('--goal-y', type=float)
    ap.add_argument('--goal-event', type=int)
    ap.add_argument('--spacing', type=float, default=8.0)
    a = ap.parse_args(argv)
    result = plan(load(a.polygons), tuple(a.start), tuple(a.goal), a.goal_event, a.spacing, a.start_y, a.goal_y)
    if result is None:
        print('no floor-connected path', file=sys.stderr)
        return 1
    print(json.dumps(result))
    return 0

if __name__ == '__main__':
    sys.exit(main())
