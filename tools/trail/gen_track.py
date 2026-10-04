#!/usr/bin/env python3
"""A PLACEHOLDER track along a course's path, for scene and camera checks only. Runs OUTSIDE Unreal.

usage: gen_track.py <course_dir> <out.npz> [--speed 3.0] [--x0 <m>] [--x1 <m>] [--hz 100]
       [--bin out.bin]

The board rolls straight down the path centre line at a constant speed. Each pose sits on the
course heightfield: the axle is one wheel radius from the surface, along the surface normal, and
the board pitches with the grade. This is geometry, not physics: no balance, no rider shift, no
lean. A MuJoCo run on the same course replaces it. The npz has the carve-lab track keys, so
tools/replay/npz_replay.py and tools/render/plan_cameras.py read it unchanged; --bin also writes
the wire-v3 replay file.
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np
from scipy import ndimage

WHEEL_R = 0.1454  # m, the MuJoCo tyre radius (BoardActor kWheelRadiusM)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("course_dir")
    ap.add_argument("out")
    ap.add_argument("--speed", type=float, default=3.0)
    ap.add_argument("--x0", type=float, default=None, help="start x (default: course spawn_x_m)")
    ap.add_argument("--x1", type=float, default=None, help="end x (default: the end of the lane)")
    ap.add_argument("--hz", type=float, default=100.0)
    ap.add_argument("--lead", type=float, default=1.0, help="seconds at rest before rolling")
    ap.add_argument("--bin", help="also write the wire-v3 replay file here")
    a = ap.parse_args()

    meta = json.load(open(os.path.join(a.course_dir, "metadata.json")))
    course = json.load(open(os.path.join(a.course_dir, "course.json")))
    lane = np.array(json.load(open(os.path.join(a.course_dir, "lane.json"))), float)
    h = np.load(os.path.join(a.course_dir, "course_height.npy")).astype(np.float64)
    sp, c0 = meta["spacing_m"], meta["center_post_index"]
    sign = -1.0 if course["path"]["axis"] == "-X" else 1.0
    x0 = a.x0 if a.x0 is not None else course.get("spawn_x_m", course["path"]["start_x_m"])
    x1 = a.x1 if a.x1 is not None else (lane[:, 0].min() if sign < 0 else lane[:, 0].max())
    o = np.argsort(lane[:, 0])
    ycen = lambda x: np.interp(x, lane[o, 0], 0.5 * (lane[o, 1] + lane[o, 2]))

    def H(x, y):
        return ndimage.map_coordinates(h, [c0 + np.asarray(y) / sp, c0 + np.asarray(x) / sp], order=1, mode="nearest")

    dist = abs(x1 - x0)
    t_roll = dist / a.speed
    t = np.arange(0.0, a.lead + t_roll, 1.0 / a.hz)
    s = np.clip(t - a.lead, 0.0, None) * a.speed           # distance travelled
    v = np.where(t >= a.lead, a.speed, 0.0)
    xc = x0 + sign * s                                      # contact point under the wheel
    yc = ycen(xc)
    zc = H(xc, yc)
    g = (H(xc + 0.05, yc) - H(xc - 0.05, yc)) / 0.10       # dz/dx along the path
    th = np.arctan(g)
    # surface normal in the x-z plane: (-sin th, 0, cos th); the axle is R along it
    px = xc - WHEEL_R * np.sin(th)
    py = yc
    pz = zc + WHEEL_R * np.cos(th)
    phi = -th                                               # rotation about +Y: +X axis -> (1, 0, g)
    # MuJoCo board forward is -X; for a run toward +X the board yaws 180 degrees.
    yaw = 0.0 if sign < 0 else np.pi
    qw = np.cos(phi / 2) * np.cos(yaw / 2)
    qx = -np.sin(phi / 2) * np.sin(yaw / 2)
    qy = np.sin(phi / 2) * np.cos(yaw / 2)
    qz = np.cos(phi / 2) * np.sin(yaw / 2)
    wheel_rate = v / WHEEL_R
    wheel_angle = np.concatenate([[0.0], np.cumsum(0.5 * (wheel_rate[1:] + wheel_rate[:-1]) * np.diff(t))])
    vx = sign * v * np.cos(th)
    vz = sign * v * np.sin(th)
    wy = np.gradient(phi, t)
    n = len(t)
    z = np.zeros(n)
    d = dict(flags=np.full(n, 3.0), seq=np.arange(n, dtype=float), t=t + 1.0 / a.hz, px=px, py=py, pz=pz,
             qw=qw, qx=qx, qy=qy, qz=qz, wheel_angle=wheel_angle, wheel_rate=wheel_rate, pitch=phi, yaw=np.full(n, yaw),
             current=z, rider_fa=z, rider_lat=z, vx=vx, vy=z, vz=vz, wx=z, wy=wy, wz=z)
    d = {k: np.asarray(val, np.float32) for k, val in d.items()}
    np.savez(a.out, **d)
    print("placeholder track: %d samples, %.1f s, x %.1f -> %.1f at %.1f m/s, z %.2f..%.2f -> %s"
          % (n, t[-1], x0, x1, a.speed, pz.min(), pz.max(), a.out))
    if a.bin:
        here = os.path.dirname(os.path.abspath(__file__))
        subprocess.check_call([sys.executable, os.path.join(here, "..", "replay", "npz_replay.py"), a.out, "--bin", a.bin])


if __name__ == "__main__":
    main()
