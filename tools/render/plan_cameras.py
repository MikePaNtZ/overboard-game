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
AXLE_ABOVE_GROUND_CM = 15.04
HANDLE = 12  # trail pre-roll frames per shot (see main)  # board origin height above the road at rest

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


# OB_Trail (tools/trail/): the MuJoCo origin is the landscape centre, yaw 0. Times are sim seconds
# on the trail track. A shot with "fixed" holds the camera at a MuJoCo point (x, y, z in m) and
# turns to keep the rider in frame. "still" is the sim time of the frame MRQ_Still_<shot> renders.
SHOTS_TRAIL = [
    # Aerial push-in: starts 13 m over the board and ends 4.2 m up and 6 m behind it, on the path
    # corridor. The move is short and the lens long, so the rider reads from the first frame and the
    # foliage at the frame edge does not smear. Motion blur is lower on this shot for the same reason.
    dict(name="T1_valley", t0=4.0, t1=9.0, rate=1.0, focal=32.0, fstop=8.0,
         dist=(1400.0, 600.0), height=(1300.0, 420.0), side=(0.0, 0.0), look_ahead=0.0,
         shake=0.0, still=6.0, motion_blur=0.2),
    dict(name="T2_chase", t0=10.5, t1=16.5, rate=1.0, focal=35.0, fstop=2.8,
         dist=(380.0, 380.0), height=(160.0, 160.0), side=(60.0, 60.0), look_ahead=1200.0, shake=0.1, still=13.5),
    # The board crosses the bridge (x 8.9 to 2.1 m) at sim 16.3 to 17.8 s. The camera is low, on the
    # verge beside the span, and follows the board. It aims 1 m over the deck with a 24 mm lens, so the
    # head stays in frame at the closest approach.
    dict(name="T3_bridge", t0=15.3, t1=19.3, rate=1.0, focal=24.0, fstop=5.6, ev100=10.6,
         cam_fixed_ground=(10.5, -3.6, 70.0), cam_aim_cm=100.0, dist=(0.0, 0.0), height=(0.0, 0.0),
         side=(0.0, 0.0), look_ahead=0.0, shake=0.0, still=17.0),
    dict(name="T4_front", t0=27.0, t1=33.5, rate=1.0, focal=40.0, fstop=2.8,
         dist=(-560.0, -420.0), height=(120.0, 120.0), side=(-170.0, -170.0), look_ahead=0.0, shake=0.06,
         look_at_head=True, head_cm=80.0, still=30.0),
]

# One follow shot for the whole run (--shots follow, any course). The camera rides a RAIL: the
# board path smoothed with a zero-lag Gaussian (rail_sigma s), so the straight line through the
# S-turns is kept and the rider swings across the frame in each carve. It sits rail_dist cm behind
# the board's point on the rail, f_height cm over the ground under it, f_side cm to the right of
# travel, and looks along the rail: aim_mix of the rider's chest, the rest a point look_ahead cm
# ahead on the rail. No damped spring: a spring lags 2 v / omega, 8 m at 9 m/s. t1 < 0 counts from
# the track end.
SHOTS_FOLLOW = [
    dict(name="F1_follow", t0=0.6, t1=-0.6, rate=1.0, focal=30.0, fstop=4.0, follow=True,
         rail_sigma=1.4, rail_dist=520.0, f_height=230.0, f_side=70.0, look_ahead=1500.0, aim_mix=0.65,
         chest_cm=105.0, shake=0.04, still=12.0, dist=(0.0, 0.0), height=(0.0, 0.0), side=(0.0, 0.0)),
]

# A side follow (--shots side): the same rail, no back distance, the camera beside the board at knee
# height, aimed at the board and the rider's hips. For runs where the board's PITCH is the story
# (a nose strike, a tail-down stop): from behind, pitch does not read.
SHOTS_SIDE = [
    dict(name="F1_follow", t0=0.6, t1=-0.6, rate=1.0, focal=28.0, fstop=5.6, follow=True,
         rail_sigma=1.4, rail_dist=0.0, f_height=75.0, f_side=380.0, look_ahead=300.0, aim_mix=0.9,
         chest_cm=70.0, shake=0.02, still=12.0, dist=(0.0, 0.0), height=(0.0, 0.0), side=(0.0, 0.0)),
]

# Footprint radius (m, scale 1) and height (cm, scale 1) of each scatter kind, as in gen_course.FOOT.
# Used to keep the trail cameras out of the dressing. Aspens also get a crown (radius 3 m from 4 m up).
FOOT = dict(aspen_01=0.4, aspen_02=0.3, aspen_03=1.0, aspen_04=0.55, hazel_01=2.4, hazel_02=0.9, hazel_03=0.7,
            hazel_04=0.35, snag_birch_a=0.4, snag_birch_c=0.4, snag_alder_b=0.4, rock=0.5,
            grass_meadow_a=0.3, grass_meadow_b=0.3, grass_meadow_c=0.3, grass_forest=0.3, grass_reed=0.45,
            flower_daisy=0.16, flower_buttercup=0.16, flower_knapweed=0.16, fern=0.8)
HEIGHT = dict(grass_meadow_a=120, grass_meadow_b=120, grass_meadow_c=120, grass_forest=120, grass_reed=200,
              flower_daisy=100, flower_buttercup=100, flower_knapweed=100, fern=80, rock=100,
              hazel_01=300, hazel_02=300, hazel_03=300, hazel_04=300)
CLEAR_CM = 150.0


def load_scatter(path):
    """Instances as one array (x, y, z, footprint_cm, top_cm, crown_cm), UE cm. None if no file."""
    if not os.path.exists(path):
        return None
    rows = []
    for kind, inst in json.load(open(path))["kinds"].items():
        for x, y, z, _yaw, _p, _r, sc in inst:
            crown = 300.0 * sc if kind.startswith("aspen") else 0.0
            rows.append((x, y, z, FOOT.get(kind, 0.5) * 100.0 * sc, z + HEIGHT.get(kind, 3000) * sc, crown))
    return np.array(rows)


def blocked(sc, p, margin):
    """True if the point p (UE cm) is inside an instance footprint plus margin."""
    d = np.hypot(sc[:, 0] - p[0], sc[:, 1] - p[1])
    trunk = (d < sc[:, 3] + margin) & (p[2] < sc[:, 4] + margin)
    crown = (sc[:, 5] > 0) & (d < sc[:, 5] + margin) & (p[2] > sc[:, 2] + 400.0) & (p[2] < sc[:, 2] + 2000.0)
    return bool((trunk | crown).any())


def cam_clear(sc, cam, target):
    """The camera point keeps CLEAR_CM from the dressing, and so does the line to the target."""
    if blocked(sc, cam, CLEAR_CM):
        return False
    for f in np.linspace(0.1, 1.0, 14):
        if blocked(sc, cam + (target - cam) * f, 40.0):
            return False
    return True


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
    ap.add_argument("--shots", default="carve", help="carve (OB_Carve, default), trail (OB_Trail), follow or side (one shot over the whole track)")
    ap.add_argument("--origin-cm", default=None, help="x,y,z of the MuJoCo origin in UE (default: OB_Carve's)")
    ap.add_argument("--origin-yaw", type=float, default=None)
    a = ap.parse_args()
    global ORIGIN_CM, ORIGIN_YAW_DEG, SHOTS
    if a.origin_cm:
        ORIGIN_CM = np.array([float(v) for v in a.origin_cm.split(",")])
    if a.origin_yaw is not None:
        ORIGIN_YAW_DEG = a.origin_yaw
    if a.shots == "side":
        a.shots = "follow"
        SHOTS_FOLLOW[:] = SHOTS_SIDE
    if a.shots == "follow":
        SHOTS = SHOTS_FOLLOW
        # OB_FOLLOW_TAIL: seconds trimmed from the track end (default 0.6); 0 keeps a clip that ends
        # on its event (a nose strike, a step-off).
        SHOTS[0]["t1"] = -float(os.environ.get("OB_FOLLOW_TAIL", "0.6"))
    if a.shots == "trail":
        SHOTS = SHOTS_TRAIL
        if os.environ.get("OB_CONTACT_CHECK") == "1":
            # Wheel-contact close-ups: a camera held 1.5 m beside the path, 0.2 m over the surface,
            # at the board's position at the still time. Short lens-free check of tyre vs ground.
            for nm, ts in (("C1_descent", 13.0), ("C2_bridge", 28.5), ("C3_climb", 36.0)):
                SHOTS.append(dict(name=nm, t0=ts - 1.0, t1=ts + 1.0, rate=1.0, focal=50.0, fstop=8.0,
                                  dist=(0.0, 0.0), height=(0.0, 0.0), side=(0.0, 0.0), look_ahead=0.0, shake=0.0,
                                  fixed_rel=(0.0, 1.5, 0.2 - 0.1454), still=ts, look_at_wheel=True))
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

    # The follow rail: the board path, zero-lag Gaussian smoothing, and its arc length.
    rail_cache = {}

    def rail(sigma_s):
        if sigma_s not in rail_cache:
            # Odd-reflection padding continues the path in a straight line past both ends. Edge
            # padding repeats the end point and pulls the rail 3 m ahead of a moving start.
            sig = hz * sigma_s
            rr = min(int(4 * sig), len(board) - 2)   # a short track truncates the kernel
            k = np.exp(-0.5 * (np.arange(-rr, rr + 1) / sig) ** 2)
            k /= k.sum()
            pad = np.concatenate([2 * board[0] - board[rr:0:-1], board, 2 * board[-1] - board[-2:-rr - 2:-1]])
            r = np.stack([np.convolve(pad[:, i], k, mode="valid") for i in range(3)], axis=1)
            arc = np.concatenate([[0.0], np.cumsum(np.hypot(np.diff(r[:, 0]), np.diff(r[:, 1])))])
            rail_cache[sigma_s] = (r, arc)
        return rail_cache[sigma_s]

    def at(arr, ts):
        ts = min(max(ts, t[0]), t[-1])
        if arr.ndim == 1:
            return float(np.interp(ts, t, arr))
        return np.array([np.interp(ts, t, arr[:, i]) for i in range(arr.shape[1])])

    def ground_under(xy):
        i = int(np.argmin(np.hypot(board[:, 0] - xy[0], board[:, 1] - xy[1])))
        return ground[i]

    scat = load_scatter(os.environ.get("OB_SCATTER", "/tmp/ob-trail/valley_gentle/scatter.json"))
    # Pre-roll: each trail shot gets HANDLE frames on its own camera before its first rendered frame.
    # The shutter is centred on the frame, so frame "start" takes half its temporal samples from
    # start - 0.5. Without the pre-roll those samples see the previous shot's camera: a ghost at the cut.
    handle = HANDLE if a.shots in ("trail", "follow") else 0
    shots, frame = [], 0
    for k, sh in enumerate(SHOTS):
        t1_req = sh["t1"] if sh["t1"] > 0 else t[-1] + sh["t1"]   # t1 <= 0 counts from the track end
        t0, t1 = max(sh["t0"], t[0]), min(t1_req, t[-1])
        if t1 <= t0:
            continue
        n = int(round((t1 - t0) / sh["rate"] * FPS))
        start = frame + handle
        keys, prev_yaw = [], None
        for j in range(-handle, n + 1):
            u = max(j, 0) / max(1, n)
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
            if sh.get("look_down"):
                tgt = ahead + np.array([0, 0, sh["look_down"]])
            if sh.get("fixed"):
                cam = to_ue(*sh["fixed"])
                tgt = to_ue(*sh["fixed_target"]) if sh.get("fixed_target") else b + np.array([0, 0, 90.0])
            if sh.get("fixed_rel"):
                ks = int(np.argmin(np.abs(t - sh["still"])))
                fr = sh["fixed_rel"]
                cam = to_ue(d["px"][ks] + fr[0], d["py"][ks] + fr[1], d["pz"][ks] + fr[2])
                tgt = to_ue(d["px"][ks], d["py"][ks], d["pz"][ks] - 0.10)
            if sh.get("look_at_head"):
                cam = b - fwd * dist + side * sid
                cam[2] = ground_under(cam) + hgt
                tgt = b + np.array([0, 0, sh.get("head_cm", HEAD_ABOVE_BOARD_CM)])
            if sh.get("cam_fixed_ground"):
                cx, cy, ch = sh["cam_fixed_ground"]
                cam = to_ue(cx, cy, 0.0)
                cam[2] = ground_under(cam) + ch
                tgt = b + np.array([0, 0, sh.get("cam_aim_cm", 70.0)])
            if sh.get("follow"):
                r, arc = rail(sh["rail_sigma"])
                ab = at(arc, ts)
                rb = at(r, ts)
                def on_rail(s_cm):
                    if s_cm < 0.0:  # before the track start: carry the rail on along its first direction
                        k = int(np.searchsorted(arc, 100.0))
                        u0 = (r[k] - r[0]) / max(arc[k], 1e-6)
                        return r[0] + u0 * s_cm
                    i = int(np.clip(np.searchsorted(arc, s_cm), 1, len(arc) - 1))
                    f = (s_cm - arc[i - 1]) / max(arc[i] - arc[i - 1], 1e-6)
                    return r[i - 1] + (r[i] - r[i - 1]) * f
                behind = on_rail(ab - sh["rail_dist"])
                tang = on_rail(ab + 50.0) - on_rail(ab - 50.0)   # the rail direction at the board
                tang[2] = 0.0
                tang /= max(np.linalg.norm(tang), 1e-6)
                right = np.array([tang[1], -tang[0], 0.0])  # UE is left-handed: +Y is right of +X
                cam = behind + right * sh["f_side"]
                cam[2] = ground_under(cam) + sh["f_height"]
                ahead = on_rail(ab + sh["look_ahead"])
                ahead[2] = ground_under(ahead) + 60.0
                chest = b + np.array([0, 0, sh["chest_cm"]])
                tgt = sh["aim_mix"] * chest + (1.0 - sh["aim_mix"]) * ahead
            if scat is not None and a.shots in ("trail", "follow"):
                aim = b + np.array([0, 0, 90.0])
                for _ in range(40):
                    if cam_clear(scat, cam, aim):
                        break
                    cam[1] += -math.copysign(20.0, cam[1] - ORIGIN_CM[1])  # toward the path centre line
                else:
                    print("WARNING: %s key %d camera not clear of dressing" % (sh["name"], j))
            dv = tgt - cam
            yaw = math.degrees(math.atan2(dv[1], dv[0]))
            pitch = math.degrees(math.atan2(dv[2], math.hypot(dv[0], dv[1])))
            roll = 0.0
            if sh["shake"]:
                tt = (start + j) / FPS
                pitch += shake(tt, sh["shake"], 10 * k)
                yaw += shake(tt, sh["shake"], 10 * k + 1)
                roll += shake(tt, sh["shake"] * 0.5, 10 * k + 2)
            if prev_yaw is not None:
                while yaw - prev_yaw > 180: yaw -= 360
                while yaw - prev_yaw < -180: yaw += 360
            prev_yaw = yaw
            focus = float(np.linalg.norm((tgt if (sh.get("look_at_head") or sh.get("fixed_rel")) else b + np.array([0, 0, 100.0])) - cam))
            if sh.get("follow"):
                focus = float(np.linalg.norm(b + np.array([0, 0, 100.0]) - cam))
            keys.append([start + j, *map(float, cam), roll, pitch, yaw, focus])
        extra = {k: sh[k] for k in ("ev100", "motion_blur") if k in sh}
        if "still" in sh:
            extra["still"] = start + int(round((min(max(sh["still"], t0), t1) - t0) / sh["rate"] * FPS))
        shots.append(dict(name=sh["name"], pre=frame, start=start, end=start + n, focal=sh["focal"], fstop=sh["fstop"],
                          **extra, sim_t0=t0, sim_t1=t1, replay_rate=sh["rate"],
                          replay_offset=t0 - (start / FPS) * sh["rate"], keys=keys))
        frame = start + n

    out = dict(fps=FPS, frames=frame, origin_cm=ORIGIN_CM.tolist(), origin_yaw_deg=ORIGIN_YAW_DEG, shots=shots)
    json.dump(out, open(a.out, "w"))
    for s in shots:
        print("%-14s frames %4d..%4d  sim %.2f..%.2f  rate %.2f  offset %.4f" % (
            s["name"], s["start"], s["end"], s["sim_t0"], s["sim_t1"], s["replay_rate"], s["replay_offset"]))
    print("total frames", frame, "=", frame / FPS, "s")


if __name__ == "__main__":
    main()
