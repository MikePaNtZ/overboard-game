#!/usr/bin/env python3
"""Check that the DRAWN OB_Embarcadero corridor matches the course height. Runs OUTSIDE Unreal.

    OB_SF_DATA=/tmp/ob-levels-sf/data OB_COURSE_DIR=<course> \
      ~/.venvs/ob-levels/bin/python tools/levels/embarcadero/verify_embarcadero.py

It reads the corridor OBM meshes this build imports, and compares their surface with
course_height.npy along the demo path (level.json). The mesh is read back from the exporter's
files, so an exporter bug shows as a gap. PASS when the worst gap is under 10 mm.
"""
import json
import os
import struct
import sys

import numpy as np
from scipy import ndimage
from scipy.spatial import cKDTree

DATA = os.environ.get("OB_SF_DATA", "/tmp/ob-levels-sf/data")
OUT = os.environ.get("OB_VERIFY_OUT", "/tmp/ob-levels-sf/verify.txt")
meta = json.load(open(os.path.join(DATA, "meta.json")))
g = meta["grid"]
cdir = os.environ.get("OB_COURSE_DIR", meta["course_dir"])
lvl = json.load(open(os.path.join(cdir, "level.json")))
h = np.load(os.path.join(cdir, "course_height.npy"))
x0, y0, sp = g["x0_m"], g["y0_m"], g["spacing_m"]
LIFT = meta["corridor_lift_m"]


def course_z(x, y):
    return ndimage.map_coordinates(h, [[(y - y0) / sp], [(x - x0) / sp]], order=1, mode="nearest")[0]


def read_obm(path):
    """Return (pos N,3 UE cm, tri T,3). flags=0 for these meshes (no colour/uv1)."""
    b = open(path, "rb").read()
    assert b[:4] == b"OBM1", path
    nv, nt = struct.unpack_from("<2I", b, 4)
    off = 20
    pos = np.frombuffer(b, dtype="<f4", count=nv * 3, offset=off).reshape(nv, 3).astype(np.float64)
    tri_off = off + nv * 12 + nv * 12 + nv * 8          # pos + nrm + uv0
    tri = np.frombuffer(b, dtype="<u4", count=nt * 3, offset=tri_off).reshape(nt, 3)
    return pos, tri


# gather all corridor triangles as MuJoCo-frame vertex triples
tris = []
i = 0
while os.path.exists(os.path.join(DATA, "corridor_%d.obm" % i)):
    pos, tri = read_obm(os.path.join(DATA, "corridor_%d.obm" % i))
    m = np.column_stack([pos[:, 0] / 100.0, -pos[:, 1] / 100.0, pos[:, 2] / 100.0])  # UE cm -> Mj m
    tris.append(m[tri])
    i += 1
T = np.vstack(tris)                                      # (ntri, 3, 3)
cent = T[:, :, :2].mean(1)
tree = cKDTree(cent)


def bary_z(x, y):
    """The drawn corridor z at (x, y): find the triangle that contains it and interpolate."""
    _, idxs = tree.query([x, y], k=16)
    for k in np.atleast_1d(idxs):
        a, b, c = T[k, 0], T[k, 1], T[k, 2]
        d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(d) < 1e-12:
            continue
        wa = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / d
        wb = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / d
        wc = 1 - wa - wb
        if wa >= -1e-6 and wb >= -1e-6 and wc >= -1e-6:
            return wa * a[2] + wb * b[2] + wc * c[2]
    return None

out = open(OUT, "w")
out.write("OB_Embarcadero corridor vs course_height along the demo path. lift %.1f mm.\n" % (LIFT * 1000))
worst, n, over, wloc = 0.0, 0, 0, None
for i, p in enumerate(lvl["demo_path"]):
    x, y = p[0], p[1]
    sz = bary_z(x, y)
    if sz is None:
        continue
    gap = (sz - (course_z(x, y) + LIFT)) * 1000.0
    if abs(gap) > 10.0:
        over += 1
    if abs(gap) > worst:
        worst, wloc = abs(gap), (i, x, y)
    n += 1
out.write("samples %d (of %d), worst |gap| %.2f mm at idx %s; %d points over 10 mm.\n"
          % (n, len(lvl["demo_path"]), worst, wloc, over))
gap_ok = bool(n and worst < 10.0)

# --- raised-block cover check. The course raises each OSM building footprint by exactly 3 m, which
# makes a sharp ~3 m STEP at the footprint edge. The smoothed DEM has no such step (even a steep
# hill moves < 0.1 m per 0.1 m post), so a step of >= 1 m between adjacent posts marks a building
# block edge. Every such edge (away from the water seawall) must sit inside a drawn building; a bare
# block means a building is missing. This targets the artificial blocks, not the real terrain.
import shapely  # noqa: E402
from scipy import ndimage as ndi  # noqa: E402
from shapely.geometry import Polygon as Poly  # noqa: E402
from shapely.ops import unary_union  # noqa: E402

step = np.zeros(h.shape, bool)
sx = np.abs(np.diff(h, axis=1)) >= 1.0
sy = np.abs(np.diff(h, axis=0)) >= 1.0
step[:, :-1] |= sx; step[:, 1:] |= sx
step[:-1, :] |= sy; step[1:, :] |= sy
water_dil = ndi.binary_dilation(h <= -1.0, iterations=3)   # exclude the seawall drop to the bay
step &= ~water_dil
bj = json.load(open(os.path.join(DATA, "buildings.json")))
bgeom = unary_union([Poly(f) for f in bj["footprints"] if len(f) >= 4]).buffer(1.6)
shapely.prepare(bgeom)
ys_, xs_ = np.where(step)
n_edge = len(xs_)
rng = np.random.default_rng(0)
pick = rng.choice(n_edge, size=min(8000, n_edge), replace=False) if n_edge else []
px = x0 + xs_[pick] * sp
py = y0 + ys_[pick] * sp
inside = shapely.contains_xy(bgeom, px, py)
uncov = int((~inside).sum())
out.write("block edges: %d posts, %d sampled, %.1f%% inside a drawn building; %d uncovered.\n"
          % (n_edge, len(pick), 100.0 * inside.mean() if len(pick) else 100.0, uncov))
blocks_ok = (len(pick) == 0) or (inside.mean() >= 0.995)
out.write("PASS\n" if gap_ok and blocks_ok else "FAIL\n")
out.close()
print(open(OUT).read())
