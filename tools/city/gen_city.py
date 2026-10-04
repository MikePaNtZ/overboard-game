#!/usr/bin/env python3
"""Generate the OB_CityHill level inputs from a MuJoCo course directory. Runs OUTSIDE Unreal (numpy).

usage: gen_city.py <course_dir> <out_dir> [--bounds building_bounds.json] [--seed N]

The course directory is an overboard-viz carve-lab course (course_height.npy, metadata.json,
course.json, lane.json). The course heightfield is the single source of truth for shape: the
street, the kerbs and the sidewalks are exact meshes sampled from it, as the OB_Trail asphalt is.
The street runs along MuJoCo -X: down a 15 % grade, across a flat block (an intersection with a
cross street), and up a 12 % grade. Buildings line both sides, set back at the sidewalk's outer
edge; a concrete plinth under each one fills the slope (the San Francisco stepped look).

Frames. MuJoCo is right-handed, Z up, metres; board forward is -X. Unreal is left-handed, Z up,
cm. ABoardActor maps MuJoCo (x, y, z) to Unreal (100x, -100y, 100z). OB_CityHill puts the PlayerStart
at the world origin with yaw 0, so the MuJoCo origin is the world origin and UE (X, Y) = (100x, -100y).

Outputs in <out_dir> (all read by build_city_level.py):
  meta.json            layout, the street/sidewalk markings, the crosswalk and intersection ranges
  street.obm           the crowned asphalt street, exact course heights (UV: u across m, v along m)
  cross.obm            the cross street at the intersection
  kerbs.obm            the 0.15 m kerbs (both main and cross street)
  sidewalks.obm        the concrete sidewalks (UV in m for the joint pattern)
  buildings.json       each building (content path, transform) and its concrete plinth box
  massing.obm          the cheap massing layer: the second row and the tall far-end blocks
  scatter.json         street dressing transforms per kind (same format as the trail)
"""
import argparse
import json
import math
import os
import sys
import time

import numpy as np
from scipy import ndimage

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "trail"))
import obm  # noqa: E402

# --- street cross-section (MuJoCo metres), from course.json path ---------------------------------
SW = 6.0            # street half width (width_m 12 / 2)
CURB_H = 0.15       # kerb height (course.json path.curb_m)
CURB_TOP = 0.15     # kerb top width
SIDE_W = 3.5        # sidewalk width (course.json path.verge_m)
SETBACK = SW + CURB_TOP + SIDE_W    # 9.65 m: the building frontage line (sidewalk outer edge)
CROSS_Y = 20.0      # the cross street reaches this far off each side
DX = 0.2            # m along the street per quad (as the trail landscape)
RIBBON_LIFT = 0.004     # the street mesh sits this far above the course height
CROSS_LIFT = 0.006      # the cross street a little higher, so it reads on top in the intersection box
SIDE_LIFT = 0.004       # the sidewalk top above its course shelf
WHEEL_R = 0.1454


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def load_course(cdir):
    meta = json.load(open(os.path.join(cdir, "metadata.json")))
    course = json.load(open(os.path.join(cdir, "course.json")))
    lane = np.array(json.load(open(os.path.join(cdir, "lane.json"))), float)
    h = np.load(os.path.join(cdir, "course_height.npy")).astype(np.float64)
    assert h.shape == (meta["nrow"], meta["ncol"]), "course_height.npy does not match metadata.json"
    if course["path"]["axis"] != "-X":
        raise SystemExit("gen_city expects a -X street (course.json path.axis)")
    return meta, course, lane, h


def course_sampler(meta, h):
    sp, c0 = meta["spacing_m"], meta["center_post_index"]

    def sample(x, y, order=1):
        return ndimage.map_coordinates(h, [c0 + np.asarray(y, float) / sp, c0 + np.asarray(x, float) / sp],
                                       order=order, mode="nearest")
    return sample


def lane_centre(lane):
    o = np.argsort(lane[:, 0])
    xs, mid = lane[o, 0], 0.5 * (lane[o, 1] + lane[o, 2])
    return lambda x: np.interp(x, xs, mid)


def flat_block(course):
    """x range (lo, hi) of the valley floor: the longest 0 % segment, and its z."""
    s0, best = 0.0, None
    x_start = course["path"]["start_x_m"]
    prof_s = np.array(course["profile"]["s_m"])
    prof_z = np.array(course["profile"]["z_m"])
    for seg in course["segments"]:
        s1 = s0 + seg["length_m"]
        if abs(seg["grade_pct"]) < 1e-6 and seg["length_m"] >= 12.0:
            z = float(np.interp(0.5 * (s0 + s1), prof_s, prof_z))
            if best is None or z < best[2]:
                best = (s0, s1, z)
        s0 = s1
    xa, xb = x_start - best[0], x_start - best[1]
    return min(xa, xb), max(xa, xb), best[2]


def orient_up(P, tri):
    """Wind each triangle so its geometric normal faces +Z in Unreal (as gen_course does)."""
    a, b, c = P[tri[:, 0]], P[tri[:, 1]], P[tri[:, 2]]
    flip = np.cross(b - a, c - a)[:, 2] > 0
    t = tri.copy()
    t[flip] = t[flip][:, ::-1]
    return t


def to_ue(P):
    P = np.asarray(P, float)
    return np.stack([P[..., 0] * 100.0, -P[..., 1] * 100.0, P[..., 2] * 100.0], -1)


def add_grid(mesh, X, Y, Z, section, uvU, uvV):
    """Add a quad grid (X, Y, Z are [nx, ny], MuJoCo m) to mesh. Normals come from the surface, so a
    vertical kerb face keeps a horizontal normal; triangles are wound to match. uvU/uvV are metres."""
    P = to_ue(np.stack([X, Y, Z], -1))
    nrm = obm.grid_normals(P)
    if nrm[..., 2].mean() < 0:        # keep the flat parts facing +Z
        nrm = -nrm
    flat = P.reshape(-1, 3)
    tri = obm.grid_tris(X.shape[0], X.shape[1])
    a, b, c = flat[tri[:, 0]], flat[tri[:, 1]], flat[tri[:, 2]]
    gn = np.cross(b - a, c - a)
    vn = nrm.reshape(-1, 3)[tri[:, 0]]
    flip = np.sum(gn * vn, 1) < 0
    tri = tri.copy()
    tri[flip] = tri[flip][:, ::-1]
    mesh.add(flat, nrm.reshape(-1, 3), np.stack([uvU, uvV], -1).reshape(-1, 2), tri, section)


def box_mesh(mesh, center, size, section, uvscale=1.0 / 100.0, yaw_deg=0.0):
    """A box (MuJoCo-frame center/size in metres) added to mesh in the Unreal frame."""
    cu = to_ue(center)
    su = (size[0] * 100.0, size[1] * 100.0, size[2] * 100.0)
    p, n, uv, t = obm.box((cu[0], cu[1], cu[2]), su, yaw_deg=yaw_deg, uv_scale=uvscale)
    mesh.add(p, n, uv, t, section)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("course_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    out = lambda n: os.path.join(a.out_dir, n)
    meta, course, lane, hc = load_course(a.course_dir)
    sample = course_sampler(meta, hc)
    yc = lane_centre(lane)        # street centre line y(x); this course is straight (y = 0)
    rng = np.random.default_rng(a.seed)

    x_hi = float(min(lane[:, 0].max(), course["path"]["start_x_m"]))   # near the spawn (~88)
    x_lo = float(lane[:, 0].min())                                     # far end (~-90)
    xa, xb, floor_z = flat_block(course)                              # valley floor x range, z
    xc_cross = 0.5 * (xa + xb)                                        # intersection centre
    log("street x %.1f..%.1f, flat block x %.1f..%.1f z %.3f, cross at x %.1f" % (x_lo, x_hi, xa, xb, floor_z, xc_cross))

    xs = np.arange(x_lo, x_hi + DX, DX)
    sw_edge_z = sample(xs, yc(xs) + (SW - 0.02))                       # street edge height, both sides equal
    sw_z = sw_edge_z + CURB_H                                         # sidewalk top height along x

    def z_side(x):
        return float(np.interp(x, xs, sw_z))

    # ---- street ---------------------------------------------------------------------------------
    across = np.linspace(-SW, SW, 49)
    XX, AA = np.meshgrid(xs, across, indexing="ij")
    YY = yc(XX) + AA
    ZZ = sample(XX, YY) + RIBBON_LIFT
    street = obm.Mesh()
    add_grid(street, XX, YY, ZZ, "Street", AA + SW, -XX)              # u across (0..12 m), v along (-x)
    nv, nt = street.write(out("street.obm"))
    log("street.obm %d verts %d tris" % (nv, nt))

    # ---- cross street (flat, at the intersection) -----------------------------------------------
    cxs = np.arange(xc_cross - SW, xc_cross + SW + DX, DX)
    cys = np.linspace(-CROSS_Y, CROSS_Y, 161)
    CX, CY = np.meshgrid(cxs, cys, indexing="ij")
    CZ = np.full_like(CX, floor_z + CROSS_LIFT)
    cross = obm.Mesh()
    add_grid(cross, CX, CY, CZ, "Street", CX - (xc_cross - SW), CY)
    nv, nt = cross.write(out("cross.obm"))
    log("cross.obm %d verts %d tris" % (nv, nt))

    # ---- kerbs and sidewalks --------------------------------------------------------------------
    # One cross-section strip per side: street edge -> kerb face -> kerb top -> sidewalk outer edge.
    kerbs = obm.Mesh()
    sides = obm.Mesh()
    gap = (xc_cross - SW, xc_cross + SW)       # the cross street mouth: main kerb and sidewalk skip it

    def main_runs():
        return [(x_lo, gap[0]), (gap[1], x_hi)]

    for s in (+1.0, -1.0):
        for (ra, rb) in main_runs():
            sx = np.arange(ra, rb + DX, DX)
            if len(sx) < 2:
                continue
            ze = np.interp(sx, xs, sw_edge_z)
            zt = ze + CURB_H
            yk = yc(sx) + s * SW
            # kerb face (vertical) + kerb top as a 3-column strip
            Xk = np.stack([sx, sx, sx], 1)
            Yk = np.stack([yk, yk, yk + s * CURB_TOP], 1)
            Zk = np.stack([ze, zt, zt], 1)
            add_grid(kerbs, Xk, Yk, Zk, "Kerb", np.stack([-sx, -sx, -sx], 1),
                     np.stack([np.zeros_like(sx), np.full_like(sx, CURB_H), np.full_like(sx, CURB_H)], 1))
            # sidewalk slab
            yw = np.linspace(0, SIDE_W, 9)
            Xs = np.repeat(sx[:, None], len(yw), 1)
            Ys = yc(sx)[:, None] + s * (SW + CURB_TOP + yw[None, :])
            Zs = np.repeat(zt[:, None], len(yw), 1)
            add_grid(sides, Xs, Ys, Zs, "Sidewalk", np.repeat(-sx[:, None], len(yw), 1),
                     np.repeat((SW + CURB_TOP + yw)[None, :], len(sx), 0))

    # cross-street kerbs and sidewalks: along the two long edges (x = xc +- SW), outside the main street
    for sx_edge in (xc_cross - SW, xc_cross + SW):
        sgn = -1.0 if sx_edge < xc_cross else 1.0
        for ys0, ys1 in ((SW, CROSS_Y), (-CROSS_Y, -SW)):
            cy = np.arange(ys0, ys1 + DX, DX)
            zt = floor_z + CURB_H
            # kerb face + top (face toward the cross street centre)
            Xk = np.stack([np.full_like(cy, sx_edge), np.full_like(cy, sx_edge), np.full_like(cy, sx_edge + sgn * CURB_TOP)], 1)
            Yk = np.stack([cy, cy, cy], 1)
            Zk = np.stack([np.full_like(cy, floor_z), np.full_like(cy, zt), np.full_like(cy, zt)], 1)
            add_grid(kerbs, Xk, Yk, Zk, "Kerb", np.stack([cy, cy, cy], 1),
                     np.stack([np.zeros_like(cy), np.full_like(cy, CURB_H), np.full_like(cy, CURB_H)], 1))
            xw = np.linspace(0, SIDE_W, 9)
            Xs = np.full((len(cy), len(xw)), sx_edge) + sgn * (CURB_TOP + xw)[None, :]
            Ys = np.repeat(cy[:, None], len(xw), 1)
            Zs = np.full_like(Xs, zt)
            add_grid(sides, Xs, Ys, Zs, "Sidewalk", Xs - sx_edge, Ys)
    nv, nt = kerbs.write(out("kerbs.obm"))
    log("kerbs.obm %d verts %d tris" % (nv, nt))
    nv, nt = sides.write(out("sidewalks.obm"))
    log("sidewalks.obm %d verts %d tris" % (nv, nt))

    # ---- massing: the cheap depth layer behind the front row ------------------------------------
    massing = obm.Mesh()
    nmass = 0
    for s in (+1.0, -1.0):
        xpos = x_lo + 12.0
        while xpos < x_hi - 12.0:
            w = rng.uniform(16.0, 28.0)
            if gap[0] - 6.0 < xpos < gap[1] + 6.0:     # leave the cross-street corridor open
                xpos = gap[1] + 6.0
                continue
            far_end = min(abs(xpos - x_lo), abs(xpos - x_hi)) < 40.0
            depth = rng.uniform(16.0, 26.0)
            h = rng.uniform(55.0, 90.0) if far_end else rng.uniform(22.0, 42.0)
            cxb = xpos + w / 2
            cyb = s * (SETBACK + 26.0 + depth / 2)
            zt = z_side(cxb)
            box_mesh(massing, (cxb, cyb, zt + h / 2 - 2.0), (w - 2.0, depth, h + 20.0), "Massing", yaw_deg=0.0)
            nmass += 1
            xpos += w + rng.uniform(1.0, 4.0)
    nv, nt = massing.write(out("massing.obm"))
    log("massing.obm %d boxes %d verts %d tris" % (nmass, nv, nt))

    # ---- dressing scatter -----------------------------------------------------------------------
    scat = make_scatter(rng, xs, sw_z, yc, x_lo, x_hi, gap, floor_z, xc_cross, z_side)
    json.dump(scat, open(out("scatter.json"), "w"), separators=(",", ":"))
    log("scatter", {k: len(v) for k, v in scat["kinds"].items()})

    # ---- meta -----------------------------------------------------------------------------------
    m = dict(
        course_dir=os.path.abspath(a.course_dir), seed=a.seed,
        origin_cm=[0.0, 0.0, 0.0], origin_yaw_deg=0.0,
        street=dict(half_width_m=SW, curb_h_m=CURB_H, curb_top_m=CURB_TOP, sidewalk_m=SIDE_W, setback_m=SETBACK,
                    ribbon_lift_m=RIBBON_LIFT, x_lo=x_lo, x_hi=x_hi),
        intersection=dict(x_lo=gap[0], x_hi=gap[1], cross_x=xc_cross, cross_reach_m=CROSS_Y, floor_z=floor_z),
        crosswalk_v=[-xa, -xb],                      # the street UV v (= -x) of the flat block's two ends
        markings=dict(edge_u=0.17, centre_u=SW, double_gap=0.16),
        frontage=dict(setback_m=SETBACK, side_signs=[1, -1]),
        sidewalk_z=dict(x=[float(v) for v in xs[::10]], z=[float(v) for v in sw_z[::10]]),
        verify=dict(ribbon_lift_mm=RIBBON_LIFT * 1000.0),
    )
    json.dump(m, open(out("meta.json"), "w"), indent=1)
    log("done")


# --- dressing (street lamps, trees, bins, signs); same scatter.json format as the trail ----------
# Footprint radius (m) and height (cm) per kind, for plan_cameras clearance (report these to add).
FOOT = dict(street_lamp=0.35, street_tree=2.4, tree_grate=1.2, trash_can=0.4, stop_sign=0.25, street_sign=0.25,
            bus_sign=0.35)
HEIGHT = dict(street_lamp=950, street_tree=850, tree_grate=10, trash_can=110, stop_sign=260, street_sign=320,
              bus_sign=260)


def make_scatter(rng, xs, sw_z, yc, x_lo, x_hi, gap, floor_z, xc_cross, z_side):
    kinds = {}

    def emit(kind, x, y, z, yaw, scale=1.0, pitch=0.0, roll=0.0):
        kinds.setdefault(kind, []).append([round(x * 100, 2), round(-y * 100, 2), round(z * 100, 2),
                                           round(yaw, 2), round(pitch, 2), round(roll, 2), round(scale, 3)])

    y_lamp = 6.0 + 0.15 + 0.6               # just inside the sidewalk, clear of the kerb
    y_tree = 6.0 + 0.15 + 1.6               # tree line mid-sidewalk
    in_gap = lambda x: gap[0] - 2.0 < x < gap[1] + 2.0

    # street lamps every ~25 m both sides; facing the street
    for s in (+1.0, -1.0):
        x = x_lo + 10.0
        k = 0
        while x < x_hi - 6.0:
            if not in_gap(x):
                yaw = 90.0 if s > 0 else 270.0        # face toward the street centre
                emit("street_lamp", x, s * y_lamp, z_side(x), yaw + rng.uniform(-3, 3))
                # a tree in a grate between lamps
                xt = x + 12.5
                if xt < x_hi - 6.0 and not in_gap(xt):
                    emit("tree_grate", xt, s * y_tree, z_side(xt), rng.uniform(0, 360))
                    emit("street_tree", xt, s * y_tree, z_side(xt), rng.uniform(0, 360), rng.uniform(0.9, 1.25))
                # the occasional bin by a lamp
                if k % 3 == 1:
                    emit("trash_can", x + 1.2, s * (6.0 + 0.15 + 0.7), z_side(x), rng.uniform(0, 360))
            x += 25.0
            k += 1

    # signs at the intersection corners: a stop sign and a street sign on each near corner
    for s in (+1.0, -1.0):
        for xe in (gap[0] - 0.8, gap[1] + 0.8):
            yaw = 90.0 if s > 0 else 270.0
            emit("stop_sign", xe, s * (6.0 + 0.15 + 0.5), z_side(xe), yaw)
            emit("street_sign", xe + 0.6, s * (6.0 + 0.15 + 0.9), z_side(xe), yaw)
    # a bus sign up the hill on the +y side
    xb0 = x_hi - 28.0
    emit("bus_sign", xb0, (6.0 + 0.15 + 0.6), z_side(xb0), 90.0)
    return dict(kinds=kinds, foot=FOOT, height=HEIGHT)


if __name__ == "__main__":
    main()
