"""Level 2, stage 2: route B as a closed demo line, in the local grid frame.

Legs (counter-clockwise, keep right, bike lane 1 m from the kerb):
  A Brannan Street eastbound, 2nd -> The Embarcadero (bike lane)
  B cross The Embarcadero at Brannan to the promenade (Herb Caen Way)
  C promenade north to Bryant Street, D a turnaround loop, E promenade south to King Street
  F cross to King Street, G King Street westbound to 2nd (bike lane)
  H 2nd Street northbound to Brannan (cycle track)
"""
import math
import sys
from pathlib import Path

import numpy as np
from shapely.geometry import LineString, Point as Point_

sys.path.insert(0, str(Path(__file__).resolve().parent))
from osm_stage import (OSM, VEHICLE, graph, junction, nearest_node, offset_right,  # noqa: E402
                       road_half_width, shortest)

BIKE_FROM_KERB_M = 2.0   # bike lane beside the parking lane; the demo needs the margin (a 1 m line hit a kerb)
TURN_R_MIN = 7.0          # the same rule as Level 1: R >= 7 m at <= 3.6 m/s
LOOP_R = 6.0              # the promenade turnaround (at <= 3 m/s, full law kappa 0.17)
PROMENADE_KEEP_RIGHT_M = 1.5
LOOP_SHIFT_M = 4.0
SHIFT_RAMP_M = 25.0


def shift_end(pts, d, at_end, ramp=SHIFT_RAMP_M):
    """Shift the end (or start) of a line d metres to the right of travel, smoothly over `ramp`."""
    L = LineString(pts)
    P = resample_line(pts, 0.5)
    out = []
    for i, (x, y) in enumerate(P):
        s = L.project(Point_((x, y)))
        dist = (L.length - s) if at_end else s
        w = min(max((ramp - dist) / (ramp * 0.8), 0.0), 1.0)
        w = w * w * (3 - 2 * w)
        a, b = P[max(i - 1, 0)], P[min(i + 1, len(P) - 1)]
        hh = math.atan2(b[1] - a[1], b[0] - a[0])
        out.append((x + d * w * math.sin(hh), y - d * w * math.cos(hh)))
    return out


def resample_line(pts, step):
    L = LineString(pts)
    n = max(2, int(L.length / step))
    return [L.interpolate(i * L.length / n).coords[0] for i in range(n + 1)]


def street_half_width(osm, name):
    v = [road_half_width(t) for t, nd in osm.ways if t.get("name") == name and t.get("highway") in VEHICLE]
    return float(np.median(v))


def street_leg(osm, name, a_xy, b_xy, directed):
    G = graph(osm, name, directed=directed)
    near = lambda xy: sorted(G, key=lambda n: math.dist(osm.xy(n), xy))[:8]
    best = None
    for s in near(a_xy):            # a dual carriageway: the nearest node can be the other side
        for t in near(b_xy):
            try:
                p = shortest(G, s, t)
            except ValueError:
                continue
            miss = math.dist(osm.xy(s), a_xy) + math.dist(osm.xy(t), b_xy)
            if best is None or miss < best[0]:     # the ends nearest the junctions win
                best = (miss, p)
    pts = [osm.xy(n) for n in best[1]]
    hw = street_half_width(osm, name)
    # Dual carriageways (oneway ways) already are one side: offset from that way's centre.
    oneway = any(t.get("oneway") == "yes" for t, nd in osm.ways if t.get("name") == name)
    off = (hw - BIKE_FROM_KERB_M) if not oneway else (hw - BIKE_FROM_KERB_M)
    return offset_right(pts, off), hw


def promenade_leg(osm, a_xy, b_xy):
    G = graph(osm, "Herb Caen Way")
    s, t = nearest_node(osm, G, a_xy), nearest_node(osm, G, b_xy)
    return [osm.xy(n) for n in shortest(G, s, t)]


def arc(cx, cy, r, a0, a1, step=0.5):
    n = max(2, int(abs(a1 - a0) * r / step))
    return [(cx + r * math.cos(a0 + (a1 - a0) * k / n), cy + r * math.sin(a0 + (a1 - a0) * k / n))
            for k in range(n + 1)]


def racket(p_in, h, d, r_side=TURN_R_MIN, r_loop=LOOP_R):
    """A racket turnaround: right arc r_side, left loop r_loop, right arc r_side.

    Enters at p_in with heading h; leaves heading h + pi on the line d metres to the LEFT of the
    entry line (keep right both ways). Returns (points, exit point)."""
    cb = (r_side + d / 2) / (r_side + r_loop)
    b = math.acos(max(-1.0, min(1.0, cb)))
    loc = []
    # Local frame: entry at (0, 0) heading +y; left = -x.
    loc += arc(r_side, 0.0, r_side, math.pi, math.pi - b)                    # right arc
    ex, ey = loc[-1]
    cx, cy = ex - r_loop * math.cos(b), ey + r_loop * math.sin(b)            # loop centre
    a0 = math.atan2(ey - cy, ex - cx)
    loc += arc(cx, cy, r_loop, a0, a0 + math.pi + 2 * b)[1:]                 # left loop
    sx, sy = loc[-1]
    c2x = sx + r_side * math.cos(b) if False else -d - r_side                # mirror of the entry arc
    loc += arc(-d - r_side, 0.0, r_side, b, 0.0)[1:]                         # right arc to the exit line
    rot = h - math.pi / 2
    c, s = math.cos(rot), math.sin(rot)
    pts = [(p_in[0] + x * c - y * s, p_in[1] + x * s + y * c) for x, y in loc]
    return pts, pts[-1]


def fillet_closed(pts, r=TURN_R_MIN, simplify=0.4, fixed=()):
    """Closed polyline -> straights joined by circular fillets of radius r (smaller where the
    straights are short). Points inside `fixed` index ranges of the simplified line are kept."""
    L = LineString(list(pts) + [pts[0]]).simplify(simplify)
    V = list(L.coords)[:-1]
    n = len(V)
    out, radii = [], []
    for i in range(n):
        a, b, c = np.array(V[i - 1]), np.array(V[i]), np.array(V[(i + 1) % n])
        u, w = b - a, c - b
        la, lb = np.linalg.norm(u), np.linalg.norm(w)
        u, w = u / la, w / lb
        phi = math.atan2(u[0] * w[1] - u[1] * w[0], u @ w)        # signed turn, left +
        if abs(phi) < 1e-3:
            out.append(tuple(b))
            continue
        T = r * math.tan(abs(phi) / 2)
        rr = r if T <= 0.5 * min(la, lb) else 0.5 * min(la, lb) / math.tan(abs(phi) / 2)
        T = rr * math.tan(abs(phi) / 2)
        p0 = b - u * T
        nrm = np.array([-u[1], u[0]]) * (1 if phi > 0 else -1)
        cen = p0 + nrm * rr
        a0 = math.atan2(p0[1] - cen[1], p0[0] - cen[0])
        out += arc(cen[0], cen[1], rr, a0, a0 + phi)
        radii.append(rr)
    return np.array(out), radii


def build(osm):
    J = {
        "2nd_brannan": junction(osm, "2nd Street", "Brannan Street"),
        "brannan_emb": junction(osm, "Brannan Street", "The Embarcadero"),
        "bryant_emb": junction(osm, "Bryant Street", "The Embarcadero"),
        "king_emb": junction(osm, "King Street", "The Embarcadero"),
        "king_2nd": junction(osm, "King Street", "2nd Street"),
    }
    legs = {}
    legs["A"], _ = street_leg(osm, "Brannan Street", J["2nd_brannan"], J["brannan_emb"], False)
    C = promenade_leg(osm, J["brannan_emb"], J["bryant_emb"])
    E = promenade_leg(osm, J["bryant_emb"], J["king_emb"])
    legs["C"] = offset_right(C, PROMENADE_KEEP_RIGHT_M)
    legs["E"] = offset_right(E, PROMENADE_KEEP_RIGHT_M)
    # Move the loop LOOP_SHIFT_M east (to the right of northbound): a left loop bulges west, and
    # at the promenade's width it reached the roadway kerb.
    legs["C"] = shift_end(legs["C"], LOOP_SHIFT_M, at_end=True)
    legs["E"] = shift_end(legs["E"], -LOOP_SHIFT_M, at_end=False)
    h = math.atan2(C[-1][1] - C[-2][1], C[-1][0] - C[-2][0])
    pin = legs["C"][-1]
    q = legs["E"][0]
    d = -(q[0] - pin[0]) * math.sin(h) + (q[1] - pin[1]) * math.cos(h)   # left of travel +
    legs["D"], exit_pt = racket(pin, h, d)
    legs["E"] = [exit_pt] + legs["E"][1:]
    legs["G"], _ = street_leg(osm, "King Street", J["king_emb"], J["king_2nd"], True)
    legs["H"], _ = street_leg(osm, "2nd Street", J["king_2nd"], J["2nd_brannan"], False)
    return J, legs


def chain(legs):
    """Join the legs in order. Where two consecutive legs cross (a street corner), cut both at
    the crossing; else join them with a straight connector (a crosswalk)."""
    order = ["A", "C", "D", "E", "G", "H"]
    seq = [list(legs[k]) for k in order]
    m = len(seq)
    corners = {order.index("G"), order.index("H")}     # G->H and H->A are street corners
    for i in range(m):
        if i not in corners:
            continue
        X, Y = seq[i], seq[(i + 1) % m]
        if len(X) < 2 or len(Y) < 2:
            continue
        # Offset lines stop short of the junction: extend both ends 25 m before the test.
        ux = np.subtract(X[-1], X[-2]); ux = ux / np.linalg.norm(ux)
        uy = np.subtract(Y[1], Y[0]); uy = uy / np.linalg.norm(uy)
        X = X + [tuple(np.add(X[-1], 25 * ux))]
        Y = [tuple(np.subtract(Y[0], 25 * uy))] + Y
        x = LineString(X).intersection(LineString(Y))
        if x.is_empty:
            continue
        pt = x if x.geom_type == "Point" else min(getattr(x, "geoms", [x]),
                                                   key=lambda g: g.distance(LineString(X[-2:])))
        pt = pt.coords[0]
        LX, LY = LineString(X), LineString(Y)
        sx, sy = LX.project(Point_(pt)), LY.project(Point_(pt))
        seq[i] = [c for c in X if LX.project(Point_(c)) < sx] + [pt]
        seq[(i + 1) % m] = [pt] + [c for c in Y if LY.project(Point_(c)) > sy]
    pts = []
    for seg in seq:
        if pts and math.dist(pts[-1], seg[0]) < 0.01:
            seg = seg[1:]
        pts += seg
    return pts


def smooth_closed(pts, step=0.5, r_min=TURN_R_MIN, iters=4000):
    """Resample a closed line and relax corners until every radius >= r_min (Laplacian)."""
    L = LineString(list(pts) + [pts[0]])
    n = int(L.length / step)
    P = np.array([L.interpolate(i * L.length / n).coords[0] for i in range(n)])
    for it in range(iters):
        prv, nxt = np.roll(P, 1, 0), np.roll(P, -1, 0)
        d1, d2 = nxt - prv, nxt - 2 * P + prv
        k = np.abs(d1[:, 0] * d2[:, 1] - d1[:, 1] * d2[:, 0]) / (np.hypot(*d1.T) ** 3 + 1e-12) * 4
        bad = k > 1.0 / r_min
        if not bad.any():
            break
        w = np.convolve(bad.astype(float), np.ones(15), "same") > 0
        P[w] += 0.25 * d2[w]
    return P, it
