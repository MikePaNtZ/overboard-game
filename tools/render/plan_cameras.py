#!/usr/bin/env python3
"""Plan the carve-render camera moves from a recorded track. Runs OUTSIDE Unreal (numpy).

Every camera is derived from the track, so any track on this road renders without hand keys:

  * an ANCHOR follows the board through a critically damped spring (it lags in each carve, so
    the board swings across the frame), and a HEADING follows the smoothed direction of travel
    (the camera swings a little with the line);
  * each shot places the camera behind the anchor (distance, height, side offset), looks
    forward past the rider at the road ahead, and keys manual focus on the rider;
  * a shot may run in slow motion: its replay rate is written out per shot, and the render
    passes it to ABoardActor (-ObReplayRate / -ObReplayOffset) so the board and the camera
    keep the same clock.

Mapping into UE space is the one ABoardActor uses (metres -> cm, mirror Y, rotate by the origin
yaw, then translate). The cameras only LOOK at the board; nothing here moves the board.
"""
import argparse
import json
import math
import os

import numpy as np

ORIGIN_CM = np.array([-47725.0, -30575.0, -6.2003])
ORIGIN_YAW_DEG = -37.6
FPS = 24
AXLE_ABOVE_GROUND_CM = 15.04  # board origin height above the road at rest

# Shot list: name, sim start, sim end, replay rate, framing. Times are clipped to the track.
# Framing: dist/height/side in cm relative to the damped anchor and heading; look_ahead in cm.
SHOTS = [
    dict(name="S1_crane_in", t0=3.0, t1=7.0, rate=1.0, focal=28.0, fstop=2.8,
         dist=(950.0, 380.0), height=(820.0, 160.0), side=(0.0, 60.0), look_ahead=1400.0, shake=0.03),
    dict(name="S2_chase", t0=7.0, t1=12.5, rate=1.0, focal=35.0, fstop=2.0,
         dist=(380.0, 380.0), height=(160.0, 160.0), side=(60.0, 60.0), look_ahead=1200.0, shake=0.12),
    dict(name="S3_low_ots", t0=12.5, t1=16.5, rate=1.0, focal=24.0, fstop=1.8,
         dist=(230.0, 230.0), height=(95.0, 95.0), side=(-55.0, -55.0), look_ahead=900.0, shake=0.18),
    dict(name="S4_slowmo", t0=16.5, t1=19.0, rate=0.4, focal=40.0, fstop=1.8,
         dist=(330.0, 330.0), height=(120.0, 120.0), side=(45.0, 45.0), look_ahead=1000.0, shake=0.08),
    dict(name="S5_chase_stop", t0=19.0, t1=24.0, rate=1.0, focal=35.0, fstop=2.2,
         dist=(380.0, 620.0), height=(160.0, 300.0), side=(60.0, 90.0), look_ahead=1200.0, shake=0.1),
]

# Optional look-dev close-up of the rider's head (OB_HEAD_CHECK=1): three-quarter front, aimed at
# the head (board + HEAD_ABOVE_BOARD_CM), from the sim time of the chase still (frame 162 = 9.75 s).
# 2 s long: a still frame within a few frames of the sequence end rendered with no board state.
HEAD_ABOVE_BOARD_CM = 165.0
if os.environ.get("OB_HEAD_CHECK") == "1":
    SHOTS.append(dict(name="S6_head_check", t0=9.75, t1=11.75, rate=1.0, focal=50.0, fstop=4.0,
                      dist=(-150.0, -150.0), height=(170.0, 170.0), side=(110.0, 110.0), look_ahead=0.0,
                      shake=0.0, look_at_head=True))


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


def damped_follow(t, x, omega):
    """Critically damped spring: the anchor chases x with natural frequency omega (rad/s)."""
    y = np.empty_like(x)
    y[0] = x[0]
    v = np.zeros(x.shape[1])
    for i in range(1, len(t)):
        dt = t[i] - t[i - 1]
        acc = omega * omega * (x[i] - y[i - 1]) - 2.0 * omega * v
        v = v + acc * dt
        y[i] = y[i - 1] + v * dt
    return y


def smoothstep(u):
    u = min(max(u, 0.0), 1.0)
    return u * u * (3 - 2 * u)


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
    hz = 1.0 / np.median(np.diff(t))

    anchor = damped_follow(t, board, omega=2.2)
    # Heading of travel: smoothed velocity direction, held when the board is (nearly) stopped.
    vel = np.gradient(gauss_smooth(board, hz * 0.9), t, axis=0)
    speed = np.hypot(vel[:, 0], vel[:, 1])
    heading = np.arctan2(vel[:, 1], vel[:, 0])
    for i in range(1, len(t)):
        if speed[i] < 40.0:
            heading[i] = heading[i - 1]
    first_moving = int(np.argmax(speed > 40.0))
    heading[:first_moving] = heading[first_moving]
    heading = np.unwrap(heading)
    heading = gauss_smooth(heading[:, None], hz * 0.6)[:, 0]
    ground = board[:, 2] - AXLE_ABOVE_GROUND_CM

    def at(arr, ts):
        ts = min(max(ts, t[0]), t[-1])
        if arr.ndim == 1:
            return float(np.interp(ts, t, arr))
        return np.array([np.interp(ts, t, arr[:, i]) for i in range(arr.shape[1])])

    def ground_under(xy):
        i = int(np.argmin(np.hypot(board[:, 0] - xy[0], board[:, 1] - xy[1])))
        return ground[i]

    shots, frame = [], 0
    for k, sh in enumerate(SHOTS):
        t0, t1 = max(sh["t0"], t[0]), min(sh["t1"], t[-1])
        if t1 <= t0:
            continue
        n = int(round((t1 - t0) / sh["rate"] * FPS))
        keys, prev_yaw = [], None
        for j in range(n + 1):
            u = j / max(1, n)
            ts = t0 + (j / FPS) * sh["rate"]
            h = at(heading, ts)
            fwd = np.array([math.cos(h), math.sin(h), 0.0])
            side = np.array([-math.sin(h), math.cos(h), 0.0])
            e = smoothstep(u)
            dist = sh["dist"][0] + (sh["dist"][1] - sh["dist"][0]) * e
            hgt = sh["height"][0] + (sh["height"][1] - sh["height"][0]) * e
            sid = sh["side"][0] + (sh["side"][1] - sh["side"][0]) * e
            anc = at(anchor, ts)
            cam = anc - fwd * dist + side * sid
            cam[2] = ground_under(cam) + hgt
            b = at(board, ts)
            ahead = anc + fwd * sh["look_ahead"]
            ahead[2] = ground_under(ahead) + 40.0
            tgt = 0.45 * (b + np.array([0, 0, 95.0])) + 0.55 * ahead
            if sh.get("look_at_head"):
                cam = b - fwd * dist + side * sid
                cam[2] = ground_under(cam) + hgt
                tgt = b + np.array([0, 0, HEAD_ABOVE_BOARD_CM])
            dv = tgt - cam
            yaw = math.degrees(math.atan2(dv[1], dv[0]))
            pitch = math.degrees(math.atan2(dv[2], math.hypot(dv[0], dv[1])))
            roll = 0.0
            if sh["shake"]:
                tt = (frame + j) / FPS
                pitch += shake(tt, sh["shake"], 10 * k)
                yaw += shake(tt, sh["shake"], 10 * k + 1)
                roll += shake(tt, sh["shake"] * 0.5, 10 * k + 2)
            if prev_yaw is not None:
                while yaw - prev_yaw > 180: yaw -= 360
                while yaw - prev_yaw < -180: yaw += 360
            prev_yaw = yaw
            focus = float(np.linalg.norm((tgt if sh.get("look_at_head") else b + np.array([0, 0, 100.0])) - cam))
            keys.append([frame + j, *map(float, cam), roll, pitch, yaw, focus])
        shots.append(dict(name=sh["name"], start=frame, end=frame + n, focal=sh["focal"], fstop=sh["fstop"],
                          sim_t0=t0, sim_t1=t1, replay_rate=sh["rate"],
                          replay_offset=t0 - (frame / FPS) * sh["rate"], keys=keys))
        frame += n

    out = dict(fps=FPS, frames=frame, origin_cm=ORIGIN_CM.tolist(), origin_yaw_deg=ORIGIN_YAW_DEG, shots=shots)
    json.dump(out, open(a.out, "w"))
    for s in shots:
        print("%-14s frames %4d..%4d  sim %.2f..%.2f  rate %.2f  offset %.4f" % (
            s["name"], s["start"], s["end"], s["sim_t0"], s["sim_t1"], s["replay_rate"], s["replay_offset"]))
    print("total frames", frame, "=", frame / FPS, "s")


if __name__ == "__main__":
    main()
