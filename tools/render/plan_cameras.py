#!/usr/bin/env python3
"""Plan the carve-render camera moves from a recorded track. Runs OUTSIDE Unreal (numpy).

Reads the npz track, maps it into UE world space with the same rule ABoardActor uses
(metres -> cm, mirror Y, rotate by the origin yaw, then translate), and bakes per-frame keys
for each shot's CineCamera: location, rotation and manual focus distance. The editor script
build_carve_level.py turns this JSON into a Level Sequence.

The cameras only LOOK at the board. Nothing here changes where the board is drawn -- that comes
from the replay file, through ABoardActor, at render time.
"""
import argparse
import json
import math

import numpy as np

ORIGIN_CM = np.array([-47725.0, -30575.0, -6.2003])
ORIGIN_YAW_DEG = -37.6
SIM_T0 = 3.0          # sim time at sequence time 0
SIM_T1 = 21.5         # sim time at the end of the sequence
FPS = 24
BOARD_GROUND_CM = 15.04  # board origin height above the road at rest (pz at t=0)


def to_ue(px, py, pz):
    x, y, z = px * 100.0, -py * 100.0, pz * 100.0
    c, s = math.cos(math.radians(ORIGIN_YAW_DEG)), math.sin(math.radians(ORIGIN_YAW_DEG))
    return np.stack([x * c - y * s + ORIGIN_CM[0], x * s + y * c + ORIGIN_CM[1], z + ORIGIN_CM[2]], axis=-1)


def gauss_smooth(a, sigma_samples):
    r = int(4 * sigma_samples)
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma_samples) ** 2)
    k /= k.sum()
    pad = np.pad(a, ((r, r), (0, 0)), mode="edge")
    return np.stack([np.convolve(pad[:, i], k, mode="valid") for i in range(a.shape[1])], axis=1)


def look_rot(cam, tgt):
    d = tgt - cam
    yaw = math.degrees(math.atan2(d[1], d[0]))
    pitch = math.degrees(math.atan2(d[2], math.hypot(d[0], d[1])))
    return pitch, yaw


def shake(t, amp, seed):
    rng = np.random.default_rng(seed)
    f = rng.uniform(0.15, 1.1, 4)
    p = rng.uniform(0, 2 * math.pi, 4)
    w = np.array([1.0, 0.6, 0.35, 0.2])
    return amp * sum(w[i] * math.sin(2 * math.pi * f[i] * t + p[i]) for i in range(4)) / w.sum()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("npz")
    ap.add_argument("out")
    a = ap.parse_args()
    d = np.load(a.npz)
    t = d["t"]
    board = to_ue(d["px"], d["py"], d["pz"])

    # Frame-rate samples of the board and of a heavily smoothed "path" that ignores the carve weave.
    n_frames = int(round((SIM_T1 - SIM_T0) * FPS))
    ft = SIM_T0 + np.arange(n_frames + 1) / FPS
    bf = np.stack([np.interp(ft, t, board[:, i]) for i in range(3)], axis=1)
    # Smooth over the whole run (500 Hz), then sample.
    path_full = gauss_smooth(board, sigma_samples=500 * 1.6)
    path = np.stack([np.interp(ft, t, path_full[:, i]) for i in range(3)], axis=1)
    aim_full = gauss_smooth(board, sigma_samples=500 * 0.25)
    aim = np.stack([np.interp(ft, t, aim_full[:, i]) for i in range(3)], axis=1)

    # Arc length along the smoothed path, over the whole run, for "N metres ahead on the road".
    seg = np.linalg.norm(np.diff(path_full[:, :2], axis=0), axis=1)
    s_full = np.concatenate([[0.0], np.cumsum(seg)])
    s_board = np.interp(ft, t, s_full)

    def path_at_s(s):
        s = np.clip(s, s_full[0], s_full[-1])
        return np.array([np.interp(s, s_full, path_full[:, i]) for i in range(3)])

    def heading_at_s(s, ds=150.0):
        p0, p1 = path_at_s(s - ds), path_at_s(s + ds)
        v = p1 - p0
        h = math.atan2(v[1], v[0])
        return np.array([math.cos(h), math.sin(h), 0.0]), np.array([-math.sin(h), math.cos(h), 0.0])

    shots = []

    def add_shot(name, f0, f1, focal, fstop, place, aim_z=70.0, shake_deg=0.0, seed=0):
        keys = []
        prev_yaw = None
        for f in range(f0, f1 + 1):
            cam = place(f)
            tgt = 0.7 * aim[f] + 0.3 * bf[f]
            tgt = tgt + np.array([0, 0, aim_z])
            pitch, yaw = look_rot(cam, tgt)
            roll = 0.0
            if shake_deg:
                tt = f / FPS
                pitch += shake(tt, shake_deg, seed)
                yaw += shake(tt, shake_deg, seed + 1)
                roll += shake(tt, shake_deg * 0.5, seed + 2)
            if prev_yaw is not None:
                while yaw - prev_yaw > 180: yaw -= 360
                while yaw - prev_yaw < -180: yaw += 360
            prev_yaw = yaw
            focus = float(np.linalg.norm((bf[f] + [0, 0, 80]) - cam))
            keys.append([f, *map(float, cam), roll, pitch, yaw, focus])
        shots.append(dict(name=name, start=f0, end=f1, focal=focal, fstop=fstop, keys=keys))

    # Shot A -- wide establishing, high above the road ahead, slow drift back and down.
    A0, A1 = 0, int(5.0 * FPS)
    sA = s_board[A1] + 1700.0
    def place_a(f):
        u = (f - A0) / max(1, A1 - A0)
        s = sA - 300.0 * u
        p = path_at_s(s)
        fwd, left = heading_at_s(s)
        return p + left * 300.0 + np.array([0, 0, 650.0 - 150.0 * u - BOARD_GROUND_CM])
    add_shot("A_wide", A0, A1, focal=70.0, fstop=4.0, place=place_a, aim_z=60.0, shake_deg=0.04, seed=10)

    # Shot B -- low tracking shot near road level, ahead of the board, looking back at the carves.
    B0, B1 = A1, int(10.5 * FPS)
    def place_b(f):
        s = s_board[f] + 650.0
        p = path_at_s(s)
        fwd, left = heading_at_s(s)
        return p + left * (-120.0) + np.array([0, 0, 45.0 - BOARD_GROUND_CM])
    add_shot("B_low_track", B0, B1, focal=50.0, fstop=1.8, place=place_b, aim_z=60.0, shake_deg=0.2, seed=20)

    # Shot D -- close side tracking at wheel height, on the sun side, slightly ahead.
    D0, D1 = B1, int(14.0 * FPS)
    def place_d(f):
        s = s_board[f] + 120.0
        p = path_at_s(s)
        fwd, left = heading_at_s(s)
        return p + left * 300.0 + np.array([0, 0, 30.0 - BOARD_GROUND_CM])
    add_shot("D_side", D0, D1, focal=28.0, fstop=2.0, place=place_d, aim_z=55.0, shake_deg=0.15, seed=40)

    # Shot C -- chase from behind and to the side, easing to a stop and rising as the board halts.
    C0, C1 = D1, n_frames
    def place_c(f):
        u = (f - C0) / max(1, C1 - C0)
        s = s_board[f] - 520.0
        p = path_at_s(s)
        fwd, left = heading_at_s(s)
        return p + left * 160.0 + np.array([0, 0, 120.0 + 120.0 * u * u - BOARD_GROUND_CM])
    add_shot("C_chase", C0, C1, focal=35.0, fstop=2.2, place=place_c, aim_z=75.0, shake_deg=0.2, seed=30)

    out = dict(fps=FPS, frames=n_frames, sim_t0=SIM_T0, origin_cm=ORIGIN_CM.tolist(), origin_yaw_deg=ORIGIN_YAW_DEG,
               board=[list(map(float, p)) for p in bf], shots=shots)
    json.dump(out, open(a.out, "w"))
    print("frames", n_frames, "shots", [(s["name"], s["start"], s["end"]) for s in shots])
    for f in (0, A1, B1, D1, n_frames):
        print("frame", f, "board", bf[f].round(1), "path", path[f].round(1))


if __name__ == "__main__":
    main()
