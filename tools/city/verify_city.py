#!/usr/bin/env python3
"""Check that a replayed track rides ON the OB_CityHill street. Runs OUTSIDE Unreal (numpy).

   OB_CITY_DATA=/tmp/ob-city/city_hill tools/city/verify_city.py /tmp/ob-city/track.bin

For samples along the track it maps the board pose MuJoCo -> UE exactly as ABoardActor does
((x, -y, z) * 100 cm, then the PlayerStart yaw and location), finds the tyre's lowest point, and
compares it with the street surface. OB_CityHill has no landscape; the street, kerbs and sidewalks
are exact meshes built from course_height.npy plus ribbon_lift_m (see gen_city.py). So the surface
reference IS the course heightfield plus the lift -- the same geometry the street mesh carries, with
no dependence on editor collision (a nanite mesh's complex collision does not trace in the editor
world). It writes /tmp/ob-city/verify.txt. PASS when the worst tyre gap is under 10 mm.

The analytical surface is the ground truth of the mesh; the editor-trace style of verify_trail.py is
not reused because that one leans on the trail landscape's collision, which this level does not have.
"""
import json
import math
import os
import struct
import sys

import numpy as np
from scipy import ndimage

DATA = os.environ.get("OB_CITY_DATA", "/tmp/ob-city/city_hill")
BIN = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("OB_TRACK_BIN", "/tmp/ob-city/track.bin")
OUT = os.environ.get("OB_VERIFY_OUT", "/tmp/ob-city/verify.txt")
STEP_M = float(os.environ.get("OB_VERIFY_STEP_M", "5.0"))
WHEEL_R = 0.1454
FMT = "<IHHQd3f4f5f2f3f3f"
SIZE = struct.calcsize(FMT)

meta = json.load(open(os.path.join(DATA, "meta.json")))
lift = meta["street"]["ribbon_lift_m"]
origin = meta["origin_cm"]
yaw = math.radians(meta["origin_yaw_deg"])
cmeta = json.load(open(os.path.join(meta["course_dir"], "metadata.json")))
h = np.load(os.path.join(meta["course_dir"], "course_height.npy")).astype(np.float64)
sp, c0 = cmeta["spacing_m"], cmeta["center_post_index"]


def surf_m(x, y):
    return float(ndimage.map_coordinates(h, [[c0 + y / sp], [c0 + x / sp]], order=1, mode="nearest")[0]) + lift


data = open(BIN, "rb").read()
rows = [struct.unpack_from(FMT, data, k) for k in range(0, len(data) - SIZE + 1, SIZE)]
c, s = math.cos(yaw), math.sin(yaw)
out = open(OUT, "w")
out.write("track %s: %d samples; origin %s yaw %.2f; surface = course_height + %.1f mm lift\n"
          % (BIN, len(rows), origin, math.degrees(yaw), lift * 1000))
out.write("%8s %8s %8s %9s %9s %8s\n" % ("t_s", "x_mj_m", "y_mj_m", "tyre_z_cm", "surf_z_cm", "gap_mm"))
last_x, worst, n = None, 0.0, 0
for r in rows:
    t, px, py, pz, qw, qx, qy, qz = r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11]
    if last_x is not None and abs(px - last_x) < STEP_M:
        continue
    last_x = px
    pitch = math.asin(max(-1.0, min(1.0, 2 * (qw * qy - qz * qx))))
    uz = pz * 100.0 + origin[2]
    tyre_z = uz - WHEEL_R * 100.0 / max(math.cos(pitch), 0.5)
    surf_z = surf_m(px, py) * 100.0 + origin[2]
    gap = (tyre_z - surf_z) * 10.0
    worst = max(worst, abs(gap))
    n += 1
    out.write("%8.2f %8.2f %8.2f %9.2f %9.2f %8.2f\n" % (t, px, py, tyre_z, surf_z, gap))
out.write("samples %d, worst |gap| %.2f mm (tyre bottom vs the street surface; + = tyre above). The street "
          "mesh is the course height plus %.1f mm.\n" % (n, worst, lift * 1000))
out.write("PASS\n" if n and worst < 10.0 else "FAIL\n")
out.close()
print(open(OUT).read())
