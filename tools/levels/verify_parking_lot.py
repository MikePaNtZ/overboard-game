#!/usr/bin/env python3
"""Check that the DRAWN OB_ParkingLot ground matches the MuJoCo course height. Runs OUTSIDE Unreal.

    OB_PL_DATA=/tmp/ob-levels-map/data OB_COURSE_DIR=<course> tools/levels/verify_parking_lot.py

It reads the ground OBM meshes this build imports (the asphalt lot tiles and the ramp), recovers
the top surface, and compares it with course_height.npy along the demo path (level.json). The mesh
is read back from the files the exporter wrote, so an exporter bug (a wrong tile offset, a dropped
lift, an axis swap) shows as a gap. PASS when the worst gap is under 10 mm. It also checks that the
pole_NE axis marker lands at UE (6000, -4000) cm. Writes /tmp/ob-levels-map/verify.txt.
"""
import json
import os
import struct
import sys

import numpy as np
from scipy import ndimage

DATA = os.environ.get("OB_PL_DATA", "/tmp/ob-levels-map/data")
OUT = os.environ.get("OB_VERIFY_OUT", "/tmp/ob-levels-map/verify.txt")

meta = json.load(open(os.path.join(DATA, "meta.json")))
g = meta["grid"]
cdir = os.environ.get("OB_COURSE_DIR", meta.get("course_dir", ""))
lvl = json.load(open(os.path.join(cdir, "level.json")))
h = np.load(os.path.join(cdir, "course_height.npy")).astype(np.float64)
x0, y0, sp = g["x0_m"], g["y0_m"], g["spacing_m"]
LIFT = meta["ground_lift_m"]
DX = meta["ground_dx_m"]


def course_z(x, y):
    return ndimage.map_coordinates(h, [[(y - y0) / sp], [(x - x0) / sp]], order=1, mode="nearest")[0]


def read_obm_pos(path):
    """Return the vertex positions (N, 3) in the Unreal frame (cm) from an OBM1 file."""
    b = open(path, "rb").read()
    assert b[:4] == b"OBM1", path
    nv, nt, ns, flags = struct.unpack_from("<4I", b, 4)
    pos = np.frombuffer(b, dtype="<f4", count=nv * 3, offset=20).reshape(nv, 3)
    return pos.astype(np.float64)


# recover the top surface on the uniform 0.1 m grid (UE cm -> MuJoCo m; x = X/100, y = -Y/100)
ground = {}
files = [f for f in os.listdir(DATA) if f.startswith("ground_lot_") or f == "ground_ramp.obm"]
for f in files:
    pos = read_obm_pos(os.path.join(DATA, f))
    xs = pos[:, 0] / 100.0
    ys = -pos[:, 1] / 100.0
    zs = pos[:, 2] / 100.0
    ix = np.round((xs - (-62.0)) / DX).astype(int)
    iy = np.round((ys - (-42.0)) / DX).astype(int)
    for a, b2, z in zip(ix, iy, zs):
        k = (int(a), int(b2))
        if k not in ground or z > ground[k]:      # the top surface where tiles overlap
            ground[k] = z


def mesh_z(x, y):
    """Bilinear lookup of the drawn ground surface at a MuJoCo point."""
    fx = (x - (-62.0)) / DX
    fy = (y - (-42.0)) / DX
    ix, iy = int(np.floor(fx)), int(np.floor(fy))
    tx, ty = fx - ix, fy - iy
    acc, wsum = 0.0, 0.0
    for dx in (0, 1):
        for dy in (0, 1):
            k = (ix + dx, iy + dy)
            if k in ground:
                w = (tx if dx else 1 - tx) * (ty if dy else 1 - ty)
                acc += w * ground[k]
                wsum += w
    return acc / wsum if wsum > 0 else None


out = open(OUT, "w")
out.write("OB_ParkingLot ground vs course_height along the demo path. ground lift %.1f mm.\n" % (LIFT * 1000))
out.write("%8s %8s %10s %10s %9s\n" % ("x_m", "y_m", "mesh_z_mm", "course_mm", "gap_mm"))
worst, n = 0.0, 0
for p in lvl["demo_path"]:
    x, y = p[0], p[1]
    mz = mesh_z(x, y)
    if mz is None:
        continue
    cz = course_z(x, y) + LIFT
    gap = (mz - cz) * 1000.0
    worst = max(worst, abs(gap))
    n += 1
    if n % 40 == 0:
        out.write("%8.2f %8.2f %10.2f %10.2f %9.2f\n" % (x, y, mz * 1000, cz * 1000, gap))
out.write("samples %d, worst |gap| %.2f mm (drawn ground vs course_height + lift).\n" % (n, worst))

# pole_NE axis marker: must stand at UE (6000, -4000) cm
props = json.load(open(os.path.join(DATA, "props.json")))
pole = [p for p in props if p["kind"] == "street_lamp"]
pole_ok = False
if pole:
    px, py = 100.0 * pole[0]["x"], -100.0 * pole[0]["y"]
    pole_ok = abs(px - 6000.0) < 1.0 and abs(py - (-4000.0)) < 1.0
    out.write("pole_NE at UE (%.1f, %.1f) cm; expected (6000, -4000): %s\n"
              % (px, py, "OK" if pole_ok else "FAIL"))
out.write("PASS\n" if n and worst < 10.0 and pole_ok else "FAIL\n")
out.close()
print(open(OUT).read())
