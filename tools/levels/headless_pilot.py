#!/usr/bin/env python3
"""Headless test pilot: ride a level's demo_path on a live sim-host, with no Unreal.

    tools/levels/headless_pilot.py tools/play/elements/parking_lot.json [--laps N] [--log out.csv]

It stands in for the game on the wire (binds StateOut 19601, sends InputIn to 19602 at 100 Hz;
its own ports, so a live game on 9601/9602 is not disturbed),
arms the board, and steers by the same laws as DemoRider: pure pursuit on the demo path
(curvature / full-stick curvature, at most 0.6 stick, 5 stick/s rate limit) and a PI speed loop
on the lean. It scores the ordered lines (start_finish + checkpoints) as the game will.
It is a test tool for layout work only. sim-host computes the ride; this sends pad inputs.
"""
import argparse
import csv
import json
import math
import socket
import struct
import sys
import time

STATE_MAGIC, INPUT_MAGIC = 0x4F425731, 0x4F424931
ARM, RESET = 1 << 0, 1 << 1
HANDOFF = 1 << 4

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from export_level import full_stick_kappa  # noqa: E402  (measured curvature, not the formula)


def parse_state(buf):
    if len(buf) < 104 or struct.unpack_from("<I", buf, 0)[0] != STATE_MAGIC:
        return None
    flags = struct.unpack_from("<H", buf, 6)[0]
    t = struct.unpack_from("<d", buf, 16)[0]
    pos = struct.unpack_from("<3f", buf, 24)
    wheel_rate, pitch, yaw = struct.unpack_from("<3f", buf, 56)
    vel = struct.unpack_from("<3f", buf, 80)
    return dict(flags=flags, t=t, x=pos[0], y=pos[1], z=pos[2], v=wheel_rate * 0.146,
                pitch=pitch, yaw=yaw, vx=vel[0], vy=vel[1])


class Lines:
    """Ordered line crossings: start_finish, then the checkpoints by order."""

    def __init__(self, els, target):
        self.lines = sorted([e for e in els if e["type"] in ("start_finish", "checkpoint")],
                            key=lambda e: e["order"])
        self.n = len(self.lines)
        self.target = target
        self.lap_start = None
        self.next = 1
        self.missed = []
        self.laps = []

    def cross(self, e, p0, p1):
        h = math.radians(e["heading_deg"])
        tx, ty = math.cos(h), math.sin(h)
        a0 = (p0[0] - e["x"]) * tx + (p0[1] - e["y"]) * ty
        a1 = (p1[0] - e["x"]) * tx + (p1[1] - e["y"]) * ty
        if not (a0 < 0.0 <= a1):
            return False
        lat = -(p1[0] - e["x"]) * ty + (p1[1] - e["y"]) * tx
        return abs(lat) <= e["half_width"]

    def update(self, t, p0, p1):
        for i, e in enumerate(self.lines):
            if not self.cross(e, p0, p1):
                continue
            if i == 0:
                if self.lap_start is not None:
                    miss = self.missed + [l["id"] for l in self.lines[self.next:]]
                    self.laps.append((t - self.lap_start, miss))
                    print(f"  LAP {len(self.laps)}: {t - self.lap_start:6.1f} s"
                          f"{'  CLEAN' if not miss else '  missed ' + ','.join(miss)}", flush=True)
                self.lap_start, self.next, self.missed = t, 1, []
            elif self.lap_start is not None and i >= self.next:
                self.missed += [l["id"] for l in self.lines[self.next:i]]
                self.next = i + 1
                print(f"  {e['label']:<16} {t - self.lap_start:6.1f} s", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elements")
    ap.add_argument("--laps", type=int, default=1)
    ap.add_argument("--log", default="/tmp/ob-levels/pilot.csv")
    ap.add_argument("--speed-scale", type=float, default=1.0)
    ap.add_argument("--timeout", type=float, default=400.0)
    ap.add_argument("--state-port", type=int, default=19601)
    ap.add_argument("--preview-s", type=float, default=1.0)
    ap.add_argument("--pp-gain", type=float, default=0.6)
    ap.add_argument("--until-idx", type=int, default=-1, help="stop when the path index passes this")
    ap.add_argument("--input-port", type=int, default=19602)
    a = ap.parse_args()
    doc = json.load(open(a.elements))
    path = doc["demo_path"]
    n = len(path)
    lines = Lines(doc["elements"], doc.get("target_lap_s"))
    path_kappa = []   # signed curvature (left +) of the demo path at each point
    for i in range(n):
        p0, p1, p2 = path[i - 1], path[i], path[(i + 1) % n]
        h1 = math.atan2(p1[1] - p0[1], p1[0] - p0[0])
        h2 = math.atan2(p2[1] - p1[1], p2[0] - p1[0])
        dh = (h2 - h1 + math.pi) % (2 * math.pi) - math.pi
        path_kappa.append(dh / max(0.5 * (math.dist(p0[:2], p1[:2]) + math.dist(p1[:2], p2[:2])), 1e-3))

    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", a.state_port))
    rx.setblocking(False)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    seq, idx = 0, 0
    st, prev = None, None
    fv, fv_prev, integ, steer_out, ramped = 0.0, 0.0, 0.0, 0.0, 0.0
    heading = None
    t_wall0 = t_last = time.time()
    log = csv.writer(open(a.log, "w"))
    log.writerow(["t", "x", "y", "v", "v_target", "pitch_deg", "steer", "lean", "flags", "idx"])
    fell = False
    while time.time() - t_wall0 < a.timeout:
        try:
            while True:
                s = parse_state(rx.recv(256))
                if s:
                    st = s
        except BlockingIOError:
            pass
        lean, steer, flags = 0.0, 0.0, ARM
        if st:
            if st["flags"] & HANDOFF and not fell:
                fell = True
                print(f"  FALL at t={st['t']:.1f} s, ({st['x']:.1f}, {st['y']:.1f}), path idx {idx}",
                      flush=True)
                break
            spd = math.hypot(st["vx"], st["vy"])
            if spd > 0.4:
                heading = math.atan2(st["vy"], st["vx"])
            elif heading is None:
                heading = math.atan2(path[1][1] - path[0][1], path[1][0] - path[0][0])
            # Advance the nearest path index (search forward only, so laps work). The first
            # sample searches the whole path (a test spawn need not be at s = 0).
            best, bi = 1e18, idx
            for k in (range(n) if prev is None else range(idx, idx + 60)):
                p = path[k % n]
                d = (p[0] - st["x"]) ** 2 + (p[1] - st["y"]) ** 2
                if d < best:
                    best, bi = d, k
            idx = bi
            v = st["v"]
            ld = max(3.5, 1.5 * abs(v))
            tgt = path[(idx + int(round(ld))) % n]
            alpha = math.atan2(tgt[1] - st["y"], tgt[0] - st["x"]) - heading
            alpha = (alpha + math.pi) % (2 * math.pi) - math.pi
            # Pure pursuit + a curvature feedforward 0.7 s ahead: a lean-steer board needs time
            # to build roll, so the reversal must start before the line turns.
            kappa_left = a.pp_gain * 2.0 * math.sin(alpha) / ld + 0.8 * path_kappa[(idx + int(round(a.preview_s * abs(v) + 1))) % n]
            full = full_stick_kappa(max(abs(v), 1.0))
            cap = 1.0 if abs(v) < 3.2 else 0.6
            want = max(-cap, min(cap, -kappa_left / full))   # positive steer = right
            steer_out += max(-0.05, min(0.05, want - steer_out))   # 5 stick/s at 100 Hz
            steer = steer_out
            vt = path[(idx + 2) % n][2] * a.speed_scale
            now = time.time()
            dt = min(now - t_last, 0.05)
            t_last = now
            ramped = min(vt, ramped + 0.6 * dt) if vt >= ramped else vt
            fv += (1 - math.exp(-0.01 / 0.25)) * (v - fv)
            err = ramped - fv
            icap = 0.5 if abs(v) < 0.3 else 1.0
            integ = max(-icap, min(icap, integ + err * 0.01))
            # Lean sets an ACCELERATION on a balancing board (the board integrates it), so the
            # speed loop is P + a small damping on the speed change; an integral term overshoots.
            accel = (fv - fv_prev) / 0.01
            fv_prev = fv
            lean = max(-0.35, min(0.35, 0.20 * err - 0.05 * accel + 0.01 * integ))
            if prev:
                lines.update(st["t"], (prev["x"], prev["y"]), (st["x"], st["y"]))
            prev = st
            log.writerow([f"{st['t']:.3f}", f"{st['x']:.3f}", f"{st['y']:.3f}", f"{v:.3f}",
                          f"{ramped:.2f}", f"{math.degrees(st['pitch']):.2f}", f"{steer:.3f}",
                          f"{lean:.3f}", st["flags"], idx])
            if len(lines.laps) >= a.laps or (a.until_idx >= 0 and idx >= a.until_idx):
                break
        tx.sendto(struct.pack("<IHHQfff", INPUT_MAGIC, 1, flags, seq, lean, 0.0, steer),
                  ("127.0.0.1", a.input_port))
        seq += 1
        time.sleep(0.01)
    if a.until_idx >= 0:
        print(f"RESULT section idx {a.until_idx} reached={idx >= a.until_idx} fell={fell}")
        sys.exit(0 if idx >= a.until_idx and not fell else 1)
    clean = [l for l in lines.laps if not l[1]]
    print(f"RESULT laps={len(lines.laps)} clean={len(clean)} fell={fell} "
          f"best={min((l[0] for l in clean), default=float('nan')):.1f} s")
    sys.exit(0 if len(clean) >= a.laps else 1)


if __name__ == "__main__":
    main()
