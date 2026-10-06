"""Level 2 (Embarcadero cruise loop): export the files that sim-host loads, plus level.json.

    ~/.venvs/ob-levels/bin/python tools/levels/embarcadero/export_embarcadero.py [OUT_DIR]

Default OUT_DIR: ~/projects/overboard-viz/out/carve-lab/data/courses/embarcadero
Outputs: course_hfield.bin, course_height.npy, metadata.json, obstacles.csv, level.json, and
tools/play/elements/embarcadero.json (mode "cruise": the demo path and a lap line, no score).

The world, by the rule "under 3 cm -> heightfield, steps and objects -> boxes" (c4):
  ground    smoothed DEM (NAVD88); carriageway 7.5 cm down, everything else 7.5 cm up (kerb 15 cm)
  buildings +3 m blocks in the heightfield (a hard edge to the city)
  water     DEM < 0 (the bay, -1.1 m): a 5 m drop at the seawall
  kerbs     boxes on the kerb line within KERB_BOX_RANGE_M of the route (a true 15 cm step)
  kerb cuts where the route crosses a kerb: no box, a heightfield ramp
  seawall   rail boxes along the water edge within RAIL_RANGE_M of the route
Map data (c) OpenStreetMap contributors, ODbL 1.0. Elevation: USGS 3DEP 1 m (public domain).
No board physics here. sim-host computes the ride.
"""
import json
import math
import struct
import sys
from pathlib import Path

import numpy as np
import shapely
from scipy.ndimage import gaussian_filter
from shapely.geometry import LineString, Point, Polygon
from shapely.ops import unary_union

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
from osm_stage import DEM, ODBL, OSM, VEHICLE, road_half_width  # noqa: E402
from route_stage import build, chain, fillet_closed  # noqa: E402
from export_level import full_stick_kappa, turn_speed_band  # noqa: E402

SPACING = 0.10
HALF_X, HALF_Y = 230.0, 430.0
KERB_HALF_STEP = 0.075
BUILDING_H = 3.0
KERB_BOX_RANGE_M = 25.0
RAIL_RANGE_M = 30.0
KERB_BOX_W = 0.20
CUT_HALF_W = 1.5           # kerb cut: 3 m wide
CUT_LEN = 1.5              # 15 cm over 1.5 m = 10 %
CUT_FLARE = 1.0            # side flares: 15 cm over 1 m
KERB_CORNER_R = 6.0
V_CRUISE = 5.0


def frame(osm):
    """Pick the grid origin (UTM) at the centre of the route box + margins; set osm.origin."""
    J, legs = build(osm)
    P, radii = fillet_closed(chain(legs))
    lo, hi = P.min(0) - 30.0, P.max(0) + np.array([45.0, 30.0])
    c = np.round((lo + hi) / 2, 1)
    osm.origin = (float(c[0]), float(c[1]))
    J, legs = build(osm)            # again, in the local frame
    P, radii = fillet_closed(chain(legs))
    return J, legs, P, radii


def rasterize(geom, X, Y, chunk=2_000_000):
    """Boolean mask of grid posts inside geom (shapely 2 vectorised, in chunks)."""
    shapely.prepare(geom)
    out = np.zeros(X.size, bool)
    xf, yf = X.ravel(), Y.ravel()
    for i in range(0, X.size, chunk):
        out[i:i + chunk] = shapely.contains_xy(geom, xf[i:i + chunk], yf[i:i + chunk])
    return out.reshape(X.shape)


def carriageway(osm, legs, route):
    """The road surface polygon: vehicle roads buffered by their half width, the street part of
    the route kept on the road, and kerb corners rounded."""
    roads = []
    for t, nd in osm.ways:
        if t.get("highway") in VEHICLE:
            roads.append(LineString(osm.line(nd)).buffer(road_half_width(t), cap_style="flat",
                                                           join_style="round"))
    # The OSM widths are estimates (lanes x 3 m): make sure the bike-lane line of the street legs
    # is on the road. Without this the line crosses the estimated kerb line at random places
    # (16 crossings in the first export, 2 are real). The fillets at the street corners too;
    # not the promenade legs (C, D, E).
    prom = unary_union([LineString(legs[k]).buffer(4.0) for k in ("C", "D", "E")])
    roads.append(route.difference(prom).buffer(2.0, cap_style="flat"))
    # Close with KERB_CORNER_R: real kerb corners are rounded. A sharp inner corner put the kerb
    # too near the corner of the two bike lines, and a right turn hit it.
    return unary_union(roads).buffer(KERB_CORNER_R).buffer(-KERB_CORNER_R)


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else
               Path.home() / "projects/overboard-viz/out/carve-lab/data/courses/embarcadero")
    out.mkdir(parents=True, exist_ok=True)
    osm = OSM()
    dem = DEM()
    J, legs, P, radii = frame(osm)
    ox, oy = osm.origin
    route = LineString(list(map(tuple, P)) + [tuple(P[0])])
    print(f"origin UTM ({ox}, {oy}); route {route.length:.0f} m, min fillet R {min(radii):.1f} m")

    ncol = round(2 * HALF_X / SPACING) + 1
    nrow = round(2 * HALF_Y / SPACING) + 1
    xs = -HALF_X + SPACING * np.arange(ncol)
    ys = -HALF_Y + SPACING * np.arange(nrow)
    X, Y = np.meshgrid(xs, ys)
    print(f"grid {nrow} x {ncol} = {nrow * ncol / 1e6:.1f} M posts")

    # Ground: the DEM at 1 m, smoothed (sigma 1 m), sampled at the posts.
    e0 = int(ox - HALF_X - dem.e0) - 3
    n0 = int(dem.n0 - (oy + HALF_Y)) - 3
    w, h = int(2 * HALF_X) + 8, int(2 * HALF_Y) + 8
    sub = dem.Z[n0:n0 + h, e0:e0 + w].astype(np.float64)
    water1m = sub < 0.0
    land = np.where(water1m, np.nan, sub)
    fill = np.nan_to_num(land, nan=float(np.nanmedian(land)))
    sm = gaussian_filter(fill, 1.0)
    # Bilinear at the posts (UTM of a post: ox + x, oy + y).
    cc = (ox + X) - (dem.e0 + e0) - 0.5
    rr = (dem.n0 - n0) - (oy + Y) - 0.5
    c0, r0 = np.floor(cc).astype(int), np.floor(rr).astype(int)
    fc, fr = cc - c0, rr - r0
    Z = (sm[r0, c0] * (1 - fc) * (1 - fr) + sm[r0, c0 + 1] * fc * (1 - fr) +
         sm[r0 + 1, c0] * (1 - fc) * fr + sm[r0 + 1, c0 + 1] * fc * fr)
    water = water1m[np.clip(np.round(rr).astype(int), 0, h - 1), np.clip(np.round(cc).astype(int), 0, w - 1)]
    del cc, rr, c0, r0, fc, fr

    carriage = carriageway(osm, legs, route)
    buildings = unary_union([Polygon(osm.line(nd)).buffer(0) for t, nd in osm.ways
                             if "building" in t and nd[0] == nd[-1] and len(nd) >= 4])
    road_m = rasterize(carriage, X, Y)
    bld_m = rasterize(buildings, X, Y) & ~road_m
    print(f"road {road_m.mean() * 100:.1f} %, buildings {bld_m.mean() * 100:.1f} %, water {water.mean() * 100:.1f} %")

    Z = Z + np.where(road_m, -KERB_HALF_STEP, KERB_HALF_STEP)
    Z = np.where(bld_m, Z + BUILDING_H, Z)

    # Kerb cuts: where the route crosses the kerb line, a ramp on the sidewalk side, no box.
    kerb_line = carriage.boundary
    cuts = []
    xing = route.intersection(kerb_line)
    for g in getattr(xing, "geoms", [xing]):
        if g.is_empty:
            continue
        p = g.coords[0]
        s = route.project(Point(p))
        a, b = route.interpolate(s - 0.5).coords[0], route.interpolate(s + 0.5).coords[0]
        hdg = math.atan2(b[1] - a[1], b[0] - a[0])
        cuts.append((p, hdg))
        # The ramp: posts within CUT_HALF_W of the route, up to CUT_LEN on the sidewalk side.
        u = (X - p[0]) * math.cos(hdg) + (Y - p[1]) * math.sin(hdg)
        v = -(X - p[0]) * math.sin(hdg) + (Y - p[1]) * math.cos(hdg)
        # Flared sides (1 m, 15 %), as on a real kerb ramp: a hard side edge felled a rider who
        # crossed the ramp sideways on the promenade.
        zone = (np.abs(v) <= CUT_HALF_W + CUT_FLARE) & (np.abs(u) <= CUT_LEN) & ~road_m
        t = np.clip(np.abs(u) / CUT_LEN, 0, 1)
        flare = np.clip((CUT_HALF_W + CUT_FLARE - np.abs(v)) / CUT_FLARE, 0, 1)
        Z = np.where(zone, Z - 2 * KERB_HALF_STEP * (1 - t) * flare, Z)
    print(f"kerb cuts: {len(cuts)}")

    Z = np.where(water, -1.1, Z).astype(np.float32)

    # Kerb boxes along the kerb line near the route, broken at the cuts.
    near = route.buffer(KERB_BOX_RANGE_M)
    cut_zone = unary_union([Point(p).buffer(CUT_HALF_W + CUT_FLARE + 0.3) for p, _ in cuts]) if cuts else None
    boxes = []

    def zat(x, y):
        c = int(round((x + HALF_X) / SPACING)); r = int(round((y + HALF_Y) / SPACING))
        return float(Z[min(max(r, 0), nrow - 1), min(max(c, 0), ncol - 1)])

    kl = kerb_line.intersection(near)
    if cut_zone is not None:
        kl = kl.difference(cut_zone)
    for g in getattr(kl, "geoms", [kl]):
        if g.is_empty or g.geom_type != "LineString":
            continue
        g = g.simplify(0.05)
        cs = list(g.coords)
        for a, b in zip(cs, cs[1:]):
            L = math.dist(a, b)
            k = max(1, math.ceil(L / 4.0))
            for j in range(k):
                pa = np.add(a, np.subtract(b, a) * j / k)
                pb = np.add(a, np.subtract(b, a) * (j + 1) / k)
                mid = (pa + pb) / 2
                yaw = math.atan2(pb[1] - pa[1], pb[0] - pa[0])
                nrm = np.array([-math.sin(yaw), math.cos(yaw)])
                # Put the box on the sidewalk side of the kerb line.
                side = 1.0 if not carriage.contains(Point(mid + nrm * 0.3)) else -1.0
                cen = mid + side * nrm * KERB_BOX_W / 2
                zr = zat(*(mid - side * nrm * 0.3))          # road surface
                zs = zat(*(mid + side * nrm * 0.4))          # sidewalk surface
                if zs - zr < 0.05 or zs - zr > 0.6:          # not a kerb here (cut, building)
                    continue
                boxes.append(("box", f"kerb{len(boxes):04d}", cen[0], cen[1],
                              float(np.linalg.norm(pb - pa)) + 0.02, KERB_BOX_W, zs - zr,
                              math.degrees(yaw), zr))
    n_kerb = len(boxes)

    # Seawall rails: along the water edge near the route.
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    near_r = route.buffer(RAIL_RANGE_M)
    cs = plt.contour(xs[::5], ys[::5], water[::5, ::5].astype(float), levels=[0.5])
    for seg in cs.allsegs[0]:
        if len(seg) < 2:
            continue
        g = LineString(seg).simplify(0.3).intersection(near_r)
        for gg in getattr(g, "geoms", [g]):
            if gg.is_empty or gg.geom_type != "LineString":
                continue
            pts = list(gg.coords)
            for a, b in zip(pts, pts[1:]):
                L = math.dist(a, b)
                if L < 0.3:
                    continue
                mid = np.add(a, b) / 2
                yaw = math.atan2(b[1] - a[1], b[0] - a[0])
                nrm = np.array([-math.sin(yaw), math.cos(yaw)])
                # 0.4 m on the land side of the edge.
                land_side = 1.0 if zat(*(mid + nrm * 1.0)) > 0 else -1.0
                cen = mid + land_side * nrm * 0.4
                boxes.append(("box", f"rail{len(boxes) - n_kerb:04d}", cen[0], cen[1], L + 0.05,
                              0.10, 1.05, math.degrees(yaw), None))
    plt.close("all")
    print(f"boxes: {n_kerb} kerb, {len(boxes) - n_kerb} rail")

    # Write the heightfield and metadata.
    with open(out / "course_hfield.bin", "wb") as f:
        f.write(struct.pack("<ii", nrow, ncol))
        f.write(Z.tobytes(order="C"))
    np.save(out / "course_height.npy", Z)

    # Demo path: 1 m points, speed band from the turn law, accel limit 0.8 m/s^2.
    n = int(route.length)
    D = np.array([route.interpolate(i * route.length / n).coords[0] for i in range(n)])
    d1 = np.roll(D, -1, 0) - np.roll(D, 1, 0)
    d2 = np.roll(D, -1, 0) - 2 * D + np.roll(D, 1, 0)
    kap = np.abs(d1[:, 0] * d2[:, 1] - d1[:, 1] * d2[:, 0]) / (np.hypot(*d1.T) ** 3 + 1e-9) * 4
    kap = np.maximum(kap, np.convolve(np.r_[kap[-5:], kap, kap[:5]], np.ones(11) / 11, "valid"))
    band = [turn_speed_band(k) for k in kap]
    v = np.array([min(V_CRUISE, hi, 3.3 if k > 1 / 12 else 99.0) for (lo, hi), k in zip(band, kap)])
    v = np.maximum(v, np.array([min(lo, V_CRUISE) for lo, hi in band]))
    sD = [route.project(Point(q)) for q in legs["D"]]   # the loop at 3.0 m/s: full steer allowed
    for i in range(n):
        if min(sD) - 8 <= i <= max(sD) + 8:
            v[i] = min(v[i], 3.0)
    for p, _ in cuts:                                  # kerb cuts at 2.5 m/s
        s0 = route.project(Point(p))
        for i in range(n):
            ds = min(abs(i - s0), n - abs(i - s0))
            if ds < 6:
                v[i] = min(v[i], 2.5)
    for _ in range(3):   # speed up at 0.8 m/s^2, slow down at 0.5 m/s^2 (the pilot lags a braking)
        for i in range(1, 2 * n):
            v[i % n] = min(v[i % n], math.sqrt(v[(i - 1) % n] ** 2 + 2 * 0.8))
        for i in range(2 * n, 0, -1):
            v[(i - 1) % n] = min(v[(i - 1) % n], math.sqrt(v[i % n] ** 2 + 2 * 0.5))
    path = [[round(float(a), 3), round(float(b), 3), round(float(c), 2)] for (a, b), c in zip(D, v)]

    # The two crossings of The Embarcadero: where the demo must give way to traffic (phase B).
    # stop_idx: the demo-path index of the stop line, 3 m before the crossing enters the road.
    def tail_pt(pts, m, end):
        L = LineString(pts)
        return L.interpolate(L.length - m if end else m).coords[0]
    crossings = []
    for a_pt, b_pt in ((tail_pt(legs["A"], 25.0, True), legs["C"][0]),
                       (legs["E"][-1], tail_pt(legs["G"], 25.0, False))):
        s_start = route.project(Point(a_pt))
        if a_pt == legs["E"][-1]:
            s_start -= 6.0          # stop on the promenade, before the kerb cut
        stop_idx = int((s_start - 3.0) % route.length)
        cx, cy = (a_pt[0] + b_pt[0]) / 2, (a_pt[1] + b_pt[1]) / 2
        crossings.append({"stop_idx": stop_idx, "x": round(cx, 2), "y": round(cy, 2),
                          "a": [round(a_pt[0], 2), round(a_pt[1], 2)],
                          "b": [round(b_pt[0], 2), round(b_pt[1], 2)], "clear_m": 10.0})

    # Spawn: 15 m along leg A (Brannan eastbound), on the line; the lap line 6 m ahead of it.
    s_sp = route.project(Point(legs["A"][0])) + 15.0
    sx, sy = route.interpolate(s_sp).coords[0]
    ax_, ay_ = route.interpolate(s_sp + 1).coords[0]
    hdg = math.atan2(ay_ - sy, ax_ - sx)
    yaw = (math.degrees(hdg) + 180.0) % 360.0
    yaw = yaw - 360.0 if yaw > 180 else yaw
    lx, ly = route.interpolate(s_sp + 6).coords[0]
    z_min, z_max = float(Z.min()), float(Z.max())
    m = 3.0
    meta = {
        "nrow": nrow, "ncol": ncol, "half_extent_m": HALF_X,
        "half_extent_x_m": HALF_X, "half_extent_y_m": HALF_Y, "spacing_m": SPACING,
        "z_min_m": z_min, "z_max_m": z_max,
        "row_col_convention": "height[row, col]; row increases with MuJoCo +Y, col with +X",
        "spawn": {"x": round(sx, 3), "y": round(sy, 3), "yaw_deg": round(yaw, 2)},
        "bounds": {"xmin": -HALF_X + m, "xmax": HALF_X - m, "ymin": -HALF_Y + m, "ymax": HALF_Y - m},
        "frame": {"crs": "EPSG:26910 (UTM 10N), shifted", "origin_easting": ox, "origin_northing": oy,
                  "z_datum": "NAVD88"},
        "source": "overboard-game tools/levels/embarcadero/export_embarcadero.py; " + ODBL +
                  "; USGS 3DEP 1 m DEM CA_SanFrancisco_B23",
    }
    (out / "metadata.json").write_text(json.dumps(meta, indent=1))
    with open(out / "obstacles.csv", "w") as f:
        f.write("# type,id,x_m,y_m,lx_m,ly_m,lz_m,yaw_deg,pitch_deg,roll_deg,z_m\n")
        f.write("# embarcadero: kerbs and seawall rails near the route. " + ODBL + "\n")
        for typ, bid, x, y, lx_, ly_, lz, yawd, zm in boxes:
            zs = "" if zm is None else f"{zm:.4f}"
            f.write(f"{typ},{bid},{x:.4f},{y:.4f},{lx_:.4f},{ly_:.4f},{lz:.4f},{yawd:.3f},0,0,{zs}\n")
    level = {
        "level": "embarcadero", "frame": "MuJoCo world = UTM 10N shifted to the grid centre; z NAVD88",
        "attribution": ODBL + "; elevation USGS 3DEP (public domain)",
        "grid": {"nrow": nrow, "ncol": ncol, "spacing_m": SPACING, "x0_m": float(xs[0]),
                 "y0_m": float(ys[0]), "half_extent_x_m": HALF_X, "half_extent_y_m": HALF_Y},
        "origin_utm": [ox, oy], "spawn": meta["spawn"], "lap_length_m": round(route.length, 1),
        "kerb_cuts": [[round(p[0], 3), round(p[1], 3), round(math.degrees(hh), 1)] for p, hh in cuts],
        "crossings": crossings,
        "junctions": {k: [round(a, 2), round(b, 2)] for k, (a, b) in J.items()},
        "demo_path": path,
    }
    (out / "level.json").write_text(json.dumps(level, indent=1))
    els = {
        "course": "embarcadero", "mode": "cruise",
        "frame": "xy: MuJoCo world x, y (m); heading_deg CCW from +X; z ground (m)",
        "attribution": ODBL,
        "lap_length_m": round(route.length, 1),
        "elements": [{"type": "start_finish", "id": "start_finish", "order": 0, "label": "EMBARCADERO LOOP",
                      "x": round(lx, 3), "y": round(ly, 3), "z": round(zat(lx, ly), 3),
                      "heading_deg": round(math.degrees(hdg), 2), "half_width": 3.0}],
        "crossings": crossings,
        "demo_path": path,
    }
    ep = HERE.parents[1] / "play" / "elements" / "embarcadero.json"
    ep.write_text(json.dumps(els, indent=1))
    t = sum(math.dist(path[i][:2], path[(i + 1) % n][:2]) / max(0.5 * (path[i][2] + path[(i + 1) % n][2]), 0.1)
            for i in range(n))
    print(f"z {z_min:.2f}..{z_max:.2f} m; spawn ({sx:.1f}, {sy:.1f}) yaw {yaw:.1f}; demo loop {t:.0f} s")
    print(f"--terrain {out / 'course_hfield.bin'} --obstacles {out / 'obstacles.csv'}")


if __name__ == "__main__":
    main()
