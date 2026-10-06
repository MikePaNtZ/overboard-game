#!/usr/bin/env python3
"""Measure the board's real turn radius on a live sim-host: hold a speed, then a constant steer.

    tools/levels/turn_test.py "2.0:-0.6" "2.5:-1.0" ...   (speed m/s : steer; negative = left)

For each case: reset, arm, ride straight along the spawn heading to the speed (PI on the lean),
then hold the steer for 6 s and fit a circle to the positions. Prints the radius, the curvature
asked for by the game's formula (steer * min(0.25, 0.6 g / v^2)), peak roll and any fall.
Same ports as headless_pilot.py (19601/19602). sim-host computes the ride.
"""
import math
import socket
import struct
import sys
import time

import numpy as np

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from headless_pilot import ARM, HANDOFF, INPUT_MAGIC, RESET, parse_state  # noqa: E402


def main():
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", 19601))
    rx.setblocking(False)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    seq = 0
    st = None

    def step(flags, lean, steer):
        nonlocal seq, st
        try:
            while True:
                s = parse_state(rx.recv(256))
                if s:
                    st = s
        except BlockingIOError:
            pass
        tx.sendto(struct.pack("<IHHQfff", INPUT_MAGIC, 1, flags, seq, lean, 0.0, steer),
                  ("127.0.0.1", 19602))
        seq += 1
        time.sleep(0.01)

    for case in sys.argv[1:]:
        v_t, steer = (float(x) for x in case.split(":"))
        for _ in range(30):
            step(RESET, 0.0, 0.0)
        for _ in range(100):
            step(0, 0.0, 0.0)
        integ, fv = 0.0, 0.0
        pts, rolls, fell, t0 = [], [], False, time.time()
        while time.time() - t0 < 16.0:
            turning = time.time() - t0 > 10.0
            v = st["v"] if st else 0.0
            fv += 0.04 * (v - fv)
            err = min(v_t, 0.4 * (time.time() - t0)) - fv
            integ = max(-1.0, min(1.0, integ + err * 0.01))
            lean = max(-0.35, min(0.35, 0.15 * err + 0.05 * integ))
            step(ARM, lean, steer if turning else 0.0)
            if st and st["flags"] & HANDOFF:
                fell = True
                break
            if st and turning and time.time() - t0 > 11.0:
                pts.append((st["x"], st["y"], v))
        if len(pts) > 50:
            P = np.array(pts)
            A = np.c_[2 * P[:, 0], 2 * P[:, 1], np.ones(len(P))]
            b = P[:, 0] ** 2 + P[:, 1] ** 2
            cx, cy, c = np.linalg.lstsq(A, b, rcond=None)[0]
            R = math.sqrt(c + cx * cx + cy * cy)
            vm = float(P[:, 2].mean())
            asked = abs(steer) * min(0.25, 0.6 * 9.81 / max(vm * vm, 0.01))
            print(f"v_target {v_t:.1f} steer {steer:+.2f}: v {vm:.2f} m/s, R {R:5.2f} m "
                  f"(kappa {1 / R:.3f}, game formula {asked:.3f}), fell={fell}", flush=True)
        else:
            print(f"v_target {v_t:.1f} steer {steer:+.2f}: no circle, fell={fell}", flush=True)


if __name__ == "__main__":
    main()
