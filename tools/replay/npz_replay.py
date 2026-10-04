#!/usr/bin/env python3
"""Replay a recorded MuJoCo run (npz track) into the game, as wire-v3 packets.

Two modes, one packet builder:

  --bin OUT.bin   Write every sample as a 104-byte wire-v3 packet, concatenated. ABoardActor
                  plays this file offline with -ObReplay=OUT.bin (deterministic clock, for
                  Movie Render Queue). Nothing is sent.
  --udp           Send the packets to 127.0.0.1:9601, paced against an absolute clock, so the
                  live BoardActor path drives the board exactly as a live host would.

The npz keys are those of overboard-viz carve-lab tracks: flags seq t px py pz qw qx qy qz
wheel_angle wheel_rate pitch yaw current rider_fa rider_lat vx vy vz wx wy wz.
This script computes no physics. It only re-encodes what MuJoCo recorded.
"""
import argparse
import socket
import struct
import time

import numpy as np

MAGIC = 0x4F425731
SCHEMA = 3
FMT = "<IHHQd3f4f5f2f3f3f"
assert struct.calcsize(FMT) == 104


def packets(d, t0, t1):
    t = d["t"]
    idx = np.nonzero((t >= t0) & (t <= t1))[0]
    for i in idx:
        g = lambda k: float(d[k][i])
        yield g("t"), struct.pack(
            FMT, MAGIC, SCHEMA, int(d["flags"][i]), int(d["seq"][i]), g("t"),
            g("px"), g("py"), g("pz"),
            g("qw"), g("qx"), g("qy"), g("qz"),
            g("wheel_angle"), g("wheel_rate"), g("pitch"), g("yaw"), g("current"),
            g("rider_fa"), g("rider_lat"),
            g("vx"), g("vy"), g("vz"),
            g("wx"), g("wy"), g("wz"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("npz")
    ap.add_argument("--t0", type=float, default=0.0)
    ap.add_argument("--t1", type=float, default=1e9)
    ap.add_argument("--bin", help="write concatenated packets to this file")
    ap.add_argument("--udp", action="store_true", help="send live to 127.0.0.1:9601")
    ap.add_argument("--rate", type=float, default=1.0, help="playback speed for --udp")
    a = ap.parse_args()
    d = np.load(a.npz)

    if a.bin:
        n = 0
        with open(a.bin, "wb") as f:
            for _, p in packets(d, a.t0, a.t1):
                f.write(p)
                n += 1
        print(f"wrote {n} packets to {a.bin}")
    if a.udp:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        start = None
        for ts, p in packets(d, a.t0, a.t1):
            if start is None:
                start, sim0 = time.perf_counter(), ts
            # Absolute schedule: never sleep a fixed step (macOS coalesces short sleeps).
            due = start + (ts - sim0) / a.rate
            while True:
                now = time.perf_counter()
                if now >= due:
                    break
                if due - now > 0.002:
                    time.sleep(0.001)
            sock.sendto(p, ("127.0.0.1", 9601))


if __name__ == "__main__":
    main()
