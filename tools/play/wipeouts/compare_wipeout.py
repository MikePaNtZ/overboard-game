#!/usr/bin/env python3
"""Compare the Unreal ragdoll's rest points (WipeoutProbe lines in a game log) with MuJoCo's.

Distances are in the plane (x, y), in metres, measured from the board position at the handoff.
Target (controls track): the rider comes to rest within about 1-2 m of MuJoCo's rest point.

Usage: compare_wipeout.py <case> <game.log>
"""
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def reference(case):
    rows = [[float(x) for x in l.split(",")] for l in open(os.path.join(HERE, "reference", f"{case}.csv"))
            if not l.startswith("#") and l.strip()]
    ho = next(i for i, r in enumerate(rows) if int(r[1]) & 4)
    start = rows[ho][11:13]
    last = rows[-1]
    return start, last[4:6], last[11:13]


def main():
    case, log = sys.argv[1], sys.argv[2]
    start, ref_rider, ref_board = reference(case)
    probe = [m.groups() for m in (re.search(r"WipeoutProbe: t ([0-9.]+) board (\S+) (\S+) \S+ rider (\w+) (\S+) (\S+)", l)
                                  for l in open(log, errors="replace")) if m]
    if not probe:
        sys.exit(f"{case}: no WipeoutProbe lines in {log} (no handoff reached the game?)")
    t, bx, by, kind, rx, ry = probe[-1]
    ue_board = (float(bx), float(by))
    ue_rider = (float(rx), float(ry)) if kind == "ragdoll" else None
    d = lambda p: math.dist(start, p)
    print(f"{case}: handoff at ({start[0]:.2f}, {start[1]:.2f}); last probe t {t} s")
    print(f"  board  MuJoCo slide {d(ref_board):5.2f} m  Unreal {d(ue_board):5.2f} m  rest gap {math.dist(ref_board, ue_board):5.2f} m")
    if ue_rider:
        gap = math.dist(ref_rider, ue_rider)
        print(f"  rider  MuJoCo slide {d(ref_rider):5.2f} m  Unreal {d(ue_rider):5.2f} m  rest gap {gap:5.2f} m  "
              f"{'PASS' if gap <= 2.0 else 'FAIL'} (<= 2 m)")
    else:
        print("  rider  Unreal: no ragdoll body found  FAIL")


if __name__ == "__main__":
    main()
