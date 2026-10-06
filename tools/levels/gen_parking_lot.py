#!/usr/bin/env python3
"""Generate the OB_ParkingLot level inputs from the parking_lot course. Runs OUTSIDE Unreal (numpy).

usage: gen_parking_lot.py <course_dir> <out_dir>

The course directory is the exported parking_lot course (course_height.npy, metadata.json,
level.json, obstacles.csv). The heightfield is the single source of truth for the ground shape.
The ground, every obstacle box and every marking are EXACT geometry sampled from the exported
data, so an exporter bug shows on screen (HARD RULE: Unreal draws only what MuJoCo owns).

Frames. MuJoCo is right-handed, Z up, metres; the lot is centred on the origin. Unreal is
left-handed, Z up, cm. ABoardActor maps MuJoCo (x, y, z) to Unreal (100x, -100y, 100z).
OB_ParkingLot puts the PlayerStart at the world origin with yaw 0, so the MuJoCo origin is the
world origin and UE (X, Y) = (100x, -100y).

Outputs in <out_dir> (all read by build_parking_lot.py):
  meta.json          layout numbers, the kerb-ring bounds, the ground lift, the verify anchor
  ground_lot_*.obm   the asphalt lot, exact course heights, split into ~25 m Nanite tiles
  ground_ramp.obm    the two ramps (raised ground), concrete, sampled from the heightfield
  ground_verge.obm   the grass band outside the kerb ring, to the grid edge
  boxes.obm          every obstacle drawn as a box (kerbs, island, plank, rails, cart, pallet),
                     one section per material
  cones.obm          the traffic cones, cone geometry inside the 0.30 x 0.30 x 0.45 m box
  marks.obm          the lane edge lines, stall lines, start/finish checker, S-carve line, arrows
  props.json         the props the editor places at an exact pose (pole_NE light, the bin)
"""
import csv
import json
import math
import os
import sys

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "trail"))
sys.path.insert(0, os.path.join(HERE, "..", "city"))
import obm  # noqa: E402
import gen_city as gc  # noqa: E402  (import-safe: its main is guarded by __name__)
sys.path.insert(0, HERE)
import parking_lot_layout as LAY  # noqa: E402

GROUND_DX = 0.10        # ground post spacing (m); the demo-path gap stays 0 at this spacing
VERGE_DX = 0.50         # the grass band is flat, so a coarse grid is enough
TILE_M = 25.0           # Nanite ground tiles, about 25 x 25 m
GROUND_LIFT = 0.004     # the drawn ground sits 4 mm above the course height, as OB_CityHill does
RAMP_LIFT = 0.006       # the ramp concrete a little higher, so it reads on top of the asphalt
MARK_LIFT = 0.003       # markings 3 mm above the ground (2-5 mm: not enough to matter)
LANE_HALF = 3.0         # lane edge lines this far off the centreline
LINE_W = 0.12           # a painted line width (m)


def log(*a):
    print(*a, flush=True)


def load(cdir):
    meta = json.load(open(os.path.join(cdir, "metadata.json")))
    lvl = json.load(open(os.path.join(cdir, "level.json")))
    h = np.load(os.path.join(cdir, "course_height.npy")).astype(np.float64)
    rows = []
    with open(os.path.join(cdir, "obstacles.csv")) as f:
        for r in csv.reader(f):
            if not r or r[0].startswith("#"):
                continue
            rows.append(r)
    g = lvl["grid"]
    return meta, lvl, h, rows, g


def make_sampler(h, g):
    """A bilinear height sampler in MuJoCo metres. Row = +Y, col = +X; x0/y0 from the grid spec."""
    x0, y0, sp = g["x0_m"], g["y0_m"], g["spacing_m"]

    def z(x, y):
        xa, ya = np.atleast_1d(np.asarray(x, float)), np.atleast_1d(np.asarray(y, float))
        r = ndimage.map_coordinates(h, [(ya - y0) / sp, (xa - x0) / sp], order=1, mode="nearest")
        return r.reshape(np.shape(x)) if np.ndim(x) else float(r[0])
    return z


def tile_grid(mesh, zfn, x_lo, x_hi, y_lo, y_hi, dx, lift, section):
    """Add one ground tile: a regular grid sampled from the heightfield, in MuJoCo metres."""
    xs = np.arange(x_lo, x_hi + dx * 0.5, dx)
    ys = np.arange(y_lo, y_hi + dx * 0.5, dx)
    X, Y = np.meshgrid(xs, ys, indexing="ij")
    Z = zfn(X, Y) + lift
    gc.add_grid(mesh, X, Y, Z, section, X, Y)   # UV in metres (world-ish), for the material tiling


def build_ground(zfn, out, lot_hx, lot_hy, grid_hx, grid_hy):
    # --- the asphalt lot, split into ~25 m tiles ------------------------------------------------
    n = 0
    for xi in np.arange(-lot_hx, lot_hx, TILE_M):
        for yi in np.arange(-lot_hy, lot_hy, TILE_M):
            x1 = min(xi + TILE_M, lot_hx)
            y1 = min(yi + TILE_M, lot_hy)
            m = obm.Mesh()
            tile_grid(m, zfn, xi, x1, yi, y1, GROUND_DX, GROUND_LIFT, "Asphalt")
            nv, nt = gc.ue_winding(m).write(out("ground_lot_%d.obm" % n))
            n += 1
    log("ground lot: %d tiles" % n)

    # --- the ramps (raised ground): concrete, sampled from the heightfield ----------------------
    ramp = obm.Mesh()
    for (rx0, rx1) in ((48.0, 62.0), (-62.0, -45.0)):   # ramp A (east), ramp B (west) footprints
        tile_grid(ramp, zfn, rx0, rx1, -12.0, 20.0, GROUND_DX, RAMP_LIFT, "Concrete")
    nv, nt = gc.ue_winding(ramp).write(out("ground_ramp.obm"))
    log("ground_ramp.obm %d verts %d tris" % (nv, nt))

    # --- the grass verge: outside the kerb ring, to the grid edge -------------------------------
    verge = obm.Mesh()
    bands = [(-grid_hx, grid_hx, lot_hy, grid_hy),        # north band
             (-grid_hx, grid_hx, -grid_hy, -lot_hy),      # south band
             (lot_hx, grid_hx, -lot_hy, lot_hy),          # east band
             (-grid_hx, -lot_hx, -lot_hy, lot_hy)]        # west band
    for (x0, x1, y0, y1) in bands:
        tile_grid(verge, zfn, x0, x1, y0, y1, VERGE_DX, 0.0, "Grass")
    nv, nt = gc.ue_winding(verge).write(out("ground_verge.obm"))
    log("ground_verge.obm %d verts %d tris" % (nv, nt))


# material section per obstacle id prefix
def box_section(bid, typ):
    if bid.startswith("kerb") or bid == "island":
        return "Concrete"
    if bid == "plank" or bid == "pallet" or bid == "cart":
        return "Wood"
    if "rail" in bid:
        return "Metal"
    return "Concrete"


def build_boxes(zfn, rows, out):
    boxes = obm.Mesh()
    cones = obm.Mesh()
    props = []
    nb = nc = 0
    for r in rows:
        typ, bid = r[0], r[1]
        x, y, lx, ly, lz = (float(r[2]), float(r[3]), float(r[4]), float(r[5]), float(r[6]))
        yaw = float(r[7])
        zm = r[10].strip()
        cz = float(zm) if zm else (float(zfn(x, y)) + lz / 2.0)
        if bid == "pole_NE":
            props.append(dict(kind="street_lamp", x=x, y=y, z=float(zfn(x, y)), yaw=yaw, light=True))
            continue
        if bid == "bin":
            props.append(dict(kind="trash_can", x=x, y=y, z=float(zfn(x, y)), yaw=yaw))
            continue
        if typ == "cone":
            cone_mesh(cones, x, y, float(zfn(x, y)), lx, lz)
            nc += 1
            continue
        gc.box_mesh(boxes, (x, y, cz), (lx, ly, lz), box_section(bid, typ), yaw_deg=yaw)
        nb += 1
    nv, nt = gc.ue_winding(boxes).write(out("boxes.obm"))
    log("boxes.obm %d boxes %d verts %d tris" % (nb, nv, nt))
    nv, nt = gc.ue_winding(cones).write(out("cones.obm"))
    log("cones.obm %d cones %d verts %d tris" % (nc, nv, nt))
    json.dump(props, open(out("props.json"), "w"), indent=1)
    log("props.json %d props" % len(props))


def cone_mesh(mesh, x, y, z_ground, base_d, height, seg=16):
    """A traffic cone: a cone body on a square base, inside the base_d x base_d x height box, in
    the Unreal frame (cm). Built here so the drawn cone stands exactly where the box does."""
    cu = gc.to_ue((x, y, z_ground))
    r = base_d * 100.0 * 0.40          # body radius a little inside the box
    H = height * 100.0
    apex = np.array([cu[0], cu[1], cu[2] + H])
    ang = np.linspace(0, 2 * math.pi, seg, endpoint=False)
    ring = np.stack([cu[0] + r * np.cos(ang), cu[1] + r * np.sin(ang),
                     np.full(seg, cu[2] + H * 0.08)], 1)
    pos = np.vstack([apex, ring])
    nrm = pos - np.array([cu[0], cu[1], cu[2]])
    nrm = nrm / np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
    uv = np.zeros((len(pos), 2), np.float32)
    tri = [(0, 1 + i, 1 + (i + 1) % seg) for i in range(seg)]
    mesh.add(pos, nrm, uv, np.array(tri), "Cone")


# --- markings -----------------------------------------------------------------------------------
def ribbon(mesh, zfn, pts, lat_off, width, section):
    """A thin painted ribbon along a polyline of (x, y, heading) points, in MuJoCo metres.
    lat_off is the lateral offset of the ribbon centre (left +); width is the paint width."""
    X = np.zeros((len(pts), 2))
    Y = np.zeros((len(pts), 2))
    for i, (x, y, h) in enumerate(pts):
        nx, ny = -math.sin(h), math.cos(h)
        for j, w in enumerate((lat_off - width / 2, lat_off + width / 2)):
            X[i, j] = x + nx * w
            Y[i, j] = y + ny * w
    Z = zfn(X, Y) + MARK_LIFT
    gc.add_grid(mesh, X, Y, Z, section, X, Y)


def dashed(pts, dash=2.0, gap=2.0, step=0.25):
    """Split a point list into dash runs, so a lane line reads as dashes, not one long strip."""
    runs, cur, d = [], [], 0.0
    period = dash + gap
    for i, p in enumerate(pts):
        if i:
            d += math.dist(pts[i - 1][:2], p[:2])
        if (d % period) < dash:
            cur.append(p)
        elif cur:
            if len(cur) >= 2:
                runs.append(cur)
            cur = []
    if len(cur) >= 2:
        runs.append(cur)
    return runs


def build_marks(zfn, lvl, out):
    marks = obm.Mesh()
    cl = LAY.centreline(step=0.25)
    pts = [(p[0], p[1], p[2]) for p in cl]

    # lane edge lines: dashed white, both sides of the circuit centreline
    for sgn in (+1.0, -1.0):
        for run in dashed(pts, dash=3.0, gap=3.0):
            ribbon(marks, zfn, run, sgn * LANE_HALF, LINE_W, "White")

    # start / finish: a checker band across the lane at the start_finish line
    sf = [c for c in lvl["checkpoints"] if c["type"] == "start_finish"][0]
    checker_band(marks, zfn, sf)

    # the S-carve line: the painted sinusoid the rider follows (magenta)
    for e in lvl["elements"]:
        if e["kind"] == "s_carve":
            s0 = LAY.seg_starts()[e["seg"]] + e["at"]
            A, lam, Ln = e["amplitude"], e["wavelength"], e["length"]
            sc = []
            s = 0.0
            while s <= Ln:
                x, y, h = LAY.pose_at(s0 + s, cl)
                off = A * math.sin(2 * math.pi * s / lam)
                sc.append((x, y, h, off))
                s += 0.25
            spts = [(x - math.sin(h) * o, y + math.cos(h) * o, h) for (x, y, h, o) in sc]
            ribbon(marks, zfn, spts, 0.0, 0.15, "Magenta")

    # arrows at the arc turns: a chevron on the lane centreline
    for s_arc in turn_markers():
        x, y, h = LAY.pose_at(s_arc, cl)
        chevron(marks, zfn, x, y, h)

    # ramp edge paint: a solid line along each ramp deck edge
    for e in lvl["elements"]:
        if e["kind"] == "ramp":
            s0 = LAY.seg_starts()[e["seg"]] + e["at"]
            hw = e["half_width"]
            L = e["up_len"] + e["deck_len"] + e["up_grade"] * e["up_len"] / e["down_grade"]
            rp = [(LAY.pose_at(s0 + s, cl)) for s in np.arange(0, L, 0.25)]
            for sgn in (+1.0, -1.0):
                ribbon(marks, zfn, rp, sgn * hw, 0.10, "Yellow")

    # parking stall lines in the infield (clear of the circuit bounds)
    build_stalls(marks, zfn)

    nv, nt = gc.ue_winding(marks).write(out("marks.obm"))
    log("marks.obm %d verts %d tris" % (nv, nt))


def checker_band(mesh, zfn, sf, squares=10, size=0.7):
    x0, y0, hd = sf["x"], sf["y"], math.radians(sf["heading_deg"])
    nx, ny = -math.sin(hd), math.cos(hd)
    fx, fy = math.cos(hd), math.sin(hd)
    for i in range(squares):
        for j in range(2):
            if (i + j) % 2:
                continue
            cx = x0 + nx * (i - squares / 2 + 0.5) * size
            cy = y0 + ny * (i - squares / 2 + 0.5) * size
            cx += fx * (j - 0.5) * size
            cy += fy * (j - 0.5) * size
            # build the square explicitly from its 4 corners
            corners = []
            for du in (-size / 2, size / 2):
                for dv in (-size / 2, size / 2):
                    corners.append((cx + fx * du + nx * dv, cy + fy * du + ny * dv))
            cxv = np.array([[corners[0][0], corners[1][0]], [corners[2][0], corners[3][0]]])
            cyv = np.array([[corners[0][1], corners[1][1]], [corners[2][1], corners[3][1]]])
            Z = zfn(cxv, cyv) + MARK_LIFT
            gc.add_grid(mesh, cxv, cyv, Z, "White", cxv, cyv)


def chevron(mesh, zfn, x, y, h, size=1.2):
    fx, fy = math.cos(h), math.sin(h)
    nx, ny = -math.sin(h), math.cos(h)
    for sgn in (+1.0, -1.0):
        pts = [(x - fx * size * 0.4 + nx * sgn * size * 0.5, y - fy * size * 0.4 + ny * sgn * size * 0.5, h),
               (x + fx * size * 0.4, y + fy * size * 0.4, h)]
        ribbon(mesh, zfn, pts, 0.0, 0.18, "Yellow")


def turn_markers():
    """Lap distance a short way into each arc turn, for an arrow."""
    s, out = 0.0, []
    for cmd in LAY.PROGRAM:
        if cmd[0] == "A":
            out.append(s + 1.0)
        s += cmd[1] if cmd[0] == "S" else abs(math.radians(cmd[2])) * cmd[1]
    return out


def build_stalls(mesh, zfn):
    """Parking stall lines in the central infield, clear of the ridden circuit."""
    for row_y in (-5.0, 5.0):
        for i in range(-3, 4):
            cx = i * 3.0
            pts = [(cx, row_y, 0.0), (cx, row_y + 4.0 * (1 if row_y > 0 else -1), 0.0)]
            ribbon(mesh, zfn, pts, 0.0, 0.10, "White")


def build_meta(out, g, lot_hx, lot_hy):
    m = dict(
        origin_cm=[0.0, 0.0, 0.0], origin_yaw_deg=0.0,
        course_dir=os.environ.get("OB_COURSE_DIR", ""),
        ground_dx_m=GROUND_DX, ground_lift_m=GROUND_LIFT, ramp_lift_m=RAMP_LIFT,
        lot_half_x_m=lot_hx, lot_half_y_m=lot_hy,
        grid=g,
        pole_ne_ue_cm=[6000.0, -4000.0],
    )
    json.dump(m, open(out("meta.json"), "w"), indent=1)


def main():
    cdir, odir = sys.argv[1], sys.argv[2]
    os.makedirs(odir, exist_ok=True)
    os.environ["OB_COURSE_DIR"] = os.path.abspath(cdir)
    out = lambda n: os.path.join(odir, n)
    meta, lvl, h, rows, g = load(cdir)
    zfn = make_sampler(h, g)
    lot_hx, lot_hy = LAY.LOT_HALF_X, LAY.LOT_HALF_Y
    grid_hx, grid_hy = LAY.GRID_HALF_X, LAY.GRID_HALF_Y
    build_ground(zfn, out, lot_hx, lot_hy, grid_hx, grid_hy)
    build_boxes(zfn, rows, out)
    build_marks(zfn, lvl, out)
    build_meta(out, g, lot_hx, lot_hy)
    log("done ->", odir)


if __name__ == "__main__":
    main()
