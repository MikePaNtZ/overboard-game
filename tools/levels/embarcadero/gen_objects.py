"""Level 2 phase B: write objects.json (scripted cars, pedestrians, cyclists) for sim-host.

    ~/.venvs/ob-levels/bin/python tools/levels/embarcadero/gen_objects.py [COURSE_DIR]

Format (agreed with c4, 2026-10-06): {"objects": [{"id", "kind", "size", "path": [[x, y], ...],
"closed", "speed_mps", "t0_s", "pause": [[s_m, seconds], ...]}]}. sim-host makes each object a
MuJoCo mocap body and moves it as a pure function of sim time; the game draws it from the OBJS
packet. Nothing here computes a board quantity.

v1 rules: cars in travel lanes, in closed loops; Embarcadero cars stop 8 s at the two route
crosswalks; pedestrians on sidewalks and the promenade edges; cyclists only in the
opposite-direction bike lanes. Away from the crosswalks, every path keeps CLEAR_M from the
demo line (checked; a path that fails is dropped and reported).
Map data (c) OpenStreetMap contributors, ODbL 1.0.
"""
import json
import math
import sys
from pathlib import Path

import numpy as np
from shapely.geometry import LineString, Point
from shapely.ops import unary_union

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
from osm_stage import DEM, ODBL, OSM, VEHICLE, graph, shortest  # noqa: E402
from export_embarcadero import HALF_X, HALF_Y, carriageway, frame  # noqa: E402

KIND = {"car": 0, "pedestrian": 1, "cyclist": 2}
SIZE = {"car": [4.6, 1.9, 1.5], "pedestrian": [0.5, 0.5, 1.75], "cyclist": [1.8, 0.6, 1.7]}
CLEAR_M = {"car": 2.6, "pedestrian": 2.5, "cyclist": 2.5}   # centre to demo line, off the crossings
XING_FREE_M = 15.0
MAX_OBJECTS = 64
CAR_LANE_OFFSET = 1.6
EDGE_M = 3.0                  # stay this far inside the grid


def inside(p):
    return abs(p[0]) < HALF_X - EDGE_M and abs(p[1]) < HALF_Y - EDGE_M


def street_line(osm, name, directed, want_heading=None):
    """The longest drivable line of a street inside the grid (one direction if directed)."""
    G = graph(osm, name, directed=directed)
    nodes = [n for n in G if inside(osm.xy(n))]
    if len(nodes) < 2:
        return None
    P = np.array([osm.xy(n) for n in nodes])
    c = P.mean(0)
    u, s, vt = np.linalg.svd(P - c)
    axis = vt[0]
    proj = (P - c) @ axis
    order = np.argsort(proj)
    best = None
    for i in order[:6]:
        for j in order[::-1][:6]:
            for a, b in ((nodes[i], nodes[j]), (nodes[j], nodes[i])):
                try:
                    p = shortest(G, a, b)
                except ValueError:
                    continue
                pts = [osm.xy(n) for n in p if inside(osm.xy(n))]
                if len(pts) < 2:
                    continue
                L = LineString(pts).length
                h = math.atan2(pts[-1][1] - pts[0][1], pts[-1][0] - pts[0][0])
                if want_heading is not None and math.cos(h - want_heading) < 0:
                    continue
                if best is None or L > best[0]:
                    best = (L, pts)
    return best[1] if best else None


def offset(pts, d):
    """d > 0: to the right of travel."""
    o = LineString(pts).offset_curve(-d, join_style=2, mitre_limit=3.0)
    return list(o.coords)


def main():
    course = Path(sys.argv[1] if len(sys.argv) > 1 else
                  Path.home() / "projects/overboard-viz/out/carve-lab/data/courses/embarcadero")
    meta = json.loads((course / "metadata.json").read_text())
    level = json.loads((course / "level.json").read_text())
    osm = OSM()
    J, legs, P, radii = frame(osm)
    if abs(osm.origin[0] - meta["frame"]["origin_easting"]) > 0.01:
        sys.exit("frame mismatch: re-export the course first")
    demo = LineString([p[:2] for p in level["demo_path"]] + [level["demo_path"][0][:2]])
    xings = [Point(c[0], c[1]) for c in level["kerb_cuts"]]
    # The two crossings of The Embarcadero: the whole crossing segment (street to promenade), not
    # only the kerb cut. Objects may cross the demo line only inside this zone (the demo rider
    # waits there until the OBJS stream is clear).
    def tail(pts, m, end):
        L = LineString(pts)
        s0, s1 = (L.length - m, L.length) if end else (0.0, m)
        return [L.interpolate(s0 + (s1 - s0) * k / 10).coords[0] for k in range(11)]
    # The end of Brannan (A) and the start of King (G) are already in the Embarcadero junction.
    xseg = [LineString(tail(legs["A"], 25.0, True) + [legs["C"][0]]),
            LineString([legs["E"][-1]] + tail(legs["G"], 25.0, False))]
    xing_zone = unary_union([x.buffer(XING_FREE_M) for x in xings] + [s.buffer(8.0) for s in xseg])
    carriage = carriageway(osm, legs, demo)
    dem = DEM()
    objects, dropped = [], []

    def add(kind, path, closed, speed, n, pause=None, size=None):
        L = LineString(path + ([path[0]] if closed else []))
        away = L.difference(xing_zone)
        dmin = away.distance(demo) if not away.is_empty else 99.0
        # distance() is the minimum over the geometry: check the whole path, not only one point.
        if dmin < CLEAR_M[kind]:
            dropped.append((kind, round(dmin, 2), round(L.length)))
            return
        period = L.length / speed
        for k in range(n):
            if len(objects) >= MAX_OBJECTS:
                return
            objects.append({"id": len(objects) + 1, "kind": kind, "kind_code": KIND[kind],
                            "size": size or SIZE[kind],
                            "path": [[round(x, 2), round(y, 2)] for x, y in path],
                            "closed": closed, "speed_mps": speed,
                            "t0_s": round(-k * period / n, 2),
                            "pause": pause or []})

    # --- Cars --------------------------------------------------------------------------
    for name, speed, n in (("Brannan Street", 8.0, 3), ("2nd Street", 8.0, 3)):
        c = street_line(osm, name, False)
        if c:
            fwd = offset(c, CAR_LANE_OFFSET)
            back = offset(c[::-1], CAR_LANE_OFFSET)
            add("car", fwd + back, True, speed, n)
    for name, speed, n in (("King Street", 8.0, 3), ("The Embarcadero", 10.0, 5)):
        hs = []
        for h in (None,):
            a = street_line(osm, name, True)
        if not a:
            continue
        ha = math.atan2(a[-1][1] - a[0][1], a[-1][0] - a[0][0])
        b = street_line(osm, name, True, want_heading=ha + math.pi)
        if not b:
            continue
        loop = offset(a, -0.8) + offset(b, -0.8)          # the left (inner) lane: far from the bike lane
        pause = []
        if name == "The Embarcadero":
            Lp = LineString(loop + [loop[0]])
            for x in xings:
                s = Lp.project(x)
                for ss in (s, ):
                    pause.append([round(max(ss - 6.0, 0.0), 2), 8.0])
            # A loop meets each crossing twice (both directions): add the second meeting too.
            for x in xings:
                pts = np.array(loop)
                d = np.hypot(pts[:, 0] - x.x, pts[:, 1] - x.y)
                idx = np.argsort(d)
                far = [i for i in idx if abs(i - idx[0]) > len(pts) // 4]
                if far:
                    s2 = Lp.project(Point(pts[far[0]]))
                    pause.append([round(max(s2 - 6.0, 0.0), 2), 8.0])
            pause.sort()
        add("car", loop, True, speed, n, pause=pause)

    # --- Pedestrians: OSM sidewalks near the route, and the promenade edges --------------
    near = demo.buffer(40.0)
    walks = []
    for t, nd in osm.ways:
        if t.get("footway") == "sidewalk" or t.get("highway") == "pedestrian":
            line = LineString(osm.line(nd))
            if line.length > 25 and line.intersects(near):
                seg = line.intersection(near)
                for g in getattr(seg, "geoms", [seg]):
                    if g.geom_type == "LineString" and g.length > 25:
                        walks.append(list(g.coords))
    walks.sort(key=lambda w: -LineString(w).length)
    G = graph(osm, "Herb Caen Way")
    prom = None
    try:
        hc = sorted(G, key=lambda n: osm.xy(n)[1])
        prom = [osm.xy(n) for n in shortest(G, hc[0], hc[-1])]
        prom = [p for p in prom if inside(p)]
    except ValueError:
        pass
    keep_out = demo.buffer(CLEAR_M["pedestrian"] + 0.3)

    def pieces(line, min_len=20.0):
        """The parts of a walk line clear of the demo line, each longer than min_len."""
        g = LineString(line).difference(keep_out)
        return [list(q.coords) for q in getattr(g, "geoms", [g])
                if q.geom_type == "LineString" and q.length >= min_len]

    if prom and len(prom) > 1:
        for d in (4.5, -4.5):
            edge = offset(prom, d)
            ok = [p for p in edge if not carriage.contains(Point(p))
                  and dem.sample([osm.origin[0] + p[0]], [osm.origin[1] + p[1]])[0] > 0.5]
            if len(ok) > 3:
                for w in pieces(ok)[:3]:
                    add("pedestrian", w + w[::-1][1:-1], True, 1.3, 2)
    for line in walks:
        for w in pieces(line):
            if len(objects) >= MAX_OBJECTS - 2:
                break
            add("pedestrian", w + w[::-1][1:-1], True, 1.3, 1)

    # --- Cyclists: the opposite-direction bike lanes only --------------------------------
    for name in ("Brannan Street", "2nd Street"):
        c = street_line(osm, name, False)
        if not c:
            continue
        hw = float(np.median([__import__("osm_stage").road_half_width(t) for t, nd in osm.ways
                              if t.get("name") == name and t.get("highway") in VEHICLE]))
        # The demo rides one side; take the lane on the other side (farther from the demo line).
        cand = [offset(c, hw - 2.0), offset(c[::-1], hw - 2.0)]
        lane = max(cand, key=lambda q: LineString(q).distance(demo) if True else 0)
        lane_d = LineString(lane).hausdorff_distance(demo)
        add("cyclist", lane + lane[::-1][1:-1], True, 4.0, 1)

    out = {
        "level": "embarcadero", "version": 1, "attribution": ODBL,
        "frame": "MuJoCo world (see metadata.json frame); sizes are full sizes (m)",
        "objects": [{k: v for k, v in o.items() if k != "kind_code"} for o in objects],
    }
    (course / "objects.json").write_text(json.dumps(out, indent=1))
    kinds = {k: sum(1 for o in objects if o["kind"] == k) for k in KIND}
    print(f"objects: {len(objects)} {kinds}; dropped paths (kind, min dist to demo, length): {dropped}")


if __name__ == "__main__":
    main()
