#!/usr/bin/env python3
"""Play a MuJoCo wipeout case into the game as StateOut packets, to test the Unreal ragdoll.

The game sees the same wire it sees in live play: the board's last seconds before the crash, then
the ADR-0012 handoff latch (bit 4) with the crash velocity, held. From the latch on, Unreal's
ragdoll owns the board and the rider. The game's wipeout probe logs where they come to rest; the
reference rest points come from the same case file (see compare_wipeout.py).

Reference cases: tools/play/wipeouts/reference/<case>.csv, from the controls track (MuJoCo,
city_hill). Columns: t, event bits (1 rider free, 2 dismount, 4 handoff), speed, amps, rider pos
(x, y, z), rider quat (w, x, y, z), then the full qpos (board free joint = qpos[0:7]).

Usage: replay_wipeout.py <case> [--lead 2.0] [--hold 8.0]
Run it with sim-host STOPPED (it sends to the game's state port 127.0.0.1:9601).
"""
import argparse
import os
import socket
import struct
import time

STATE_MAGIC = 0x4F425731
FLAG_ARMED, FLAG_VALID, FLAG_FALLEN, FLAG_HANDOFF = 1, 2, 4, 16
HERE = os.path.dirname(os.path.abspath(__file__))


def load(case):
    rows = []
    for line in open(os.path.join(HERE, "reference", f"{case}.csv")):
        if line.startswith("#") or not line.strip():
            continue
        rows.append([float(x) for x in line.split(",")])
    return rows


def qmul(a, b):
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (aw * bw - ax * bx - ay * by - az * bz,
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw)


def world_angvel(q0, q1, dt):
    # omega_world = 2 * (dq/dt) * conj(q), vector part
    dq = tuple((b - a) / dt for a, b in zip(q0, q1))
    q = q1
    w = qmul(dq, (q[0], -q[1], -q[2], -q[3]))
    return (2 * w[1], 2 * w[2], 2 * w[3])


def packet(seq, t, flags, pos, quat, lin, ang):
    return struct.pack("<IHHQd3f4f7f3f3f", STATE_MAGIC, 3, flags, seq, t,
                       *pos, *quat, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, *lin, *ang)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("case")
    ap.add_argument("--lead", type=float, default=2.0, help="seconds of ride before the crash")
    ap.add_argument("--hold", type=float, default=8.0, help="seconds to hold the latch")
    ap.add_argument("--wait", type=float, default=0.0, help="seconds to send the first frame first (game load)")
    ap.add_argument("--repeat", type=int, default=1, help="crash this many times; the latch clears between (a reset)")
    a = ap.parse_args()

    rows = load(a.case)
    dt = rows[1][0] - rows[0][0]
    ho = next((i for i, r in enumerate(rows) if int(r[1]) & 4), None)
    if ho is None:
        raise SystemExit(f"{a.case}: no handoff in this case")
    start = max(0, ho - int(a.lead / dt))
    board = lambda r: (r[11:14], r[14:18])

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dst = ("127.0.0.1", 9601)
    seq = 0
    p0, q0 = board(rows[start])
    t_end = time.perf_counter() + a.wait
    while time.perf_counter() < t_end:  # park the board at the first frame while the game loads
        s.sendto(packet(seq, rows[start][0], FLAG_ARMED | FLAG_VALID, p0, q0, (0, 0, 0), (0, 0, 0)), dst)
        seq += 1
        time.sleep(0.02)

    for n in range(a.repeat):
        if n > 0:
            # The reset: the latch clears and the board is back at the first frame, as after a
            # sim-host reset; the game ends its handoff and puts the rider back on the deck.
            t_end = time.perf_counter() + 3.0
            while time.perf_counter() < t_end:
                s.sendto(packet(seq, rows[start][0], FLAG_ARMED | FLAG_VALID, p0, q0, (0, 0, 0), (0, 0, 0)), dst)
                seq += 1
                time.sleep(0.02)
        seq = crash(s, dst, seq, rows, start, ho, dt, board, a)


def crash(s, dst, seq, rows, start, ho, dt, board, a):
    t0 = time.perf_counter()
    for i in range(start, ho + 1):
        p, q = board(rows[i])
        pp, pq = board(rows[max(i - 1, 0)])
        lin = tuple((b - c) / dt for b, c in zip(p, pp))
        ang = world_angvel(pq, q, dt)
        flags = FLAG_ARMED | FLAG_VALID | (FLAG_HANDOFF | FLAG_FALLEN if i == ho else 0)
        s.sendto(packet(seq, rows[i][0], flags, p, q, lin, ang), dst)
        seq += 1
        target = t0 + (i - start + 1) * dt
        while time.perf_counter() < target:
            time.sleep(0.001)
    # Hold the latch: the wire freezes pos/quat/vel at the strike cycle, as sim-host does.
    print(f"{a.case}: handoff at t {rows[ho][0]:.2f}, speed {rows[ho][2]:.2f} m/s, "
          f"lin ({lin[0]:.2f},{lin[1]:.2f},{lin[2]:.2f}) m/s, ang ({ang[0]:.2f},{ang[1]:.2f},{ang[2]:.2f}) rad/s", flush=True)
    t_end = time.perf_counter() + a.hold
    while time.perf_counter() < t_end:
        s.sendto(packet(seq, rows[ho][0], FLAG_ARMED | FLAG_VALID | FLAG_HANDOFF | FLAG_FALLEN, p, q, lin, ang), dst)
        seq += 1
        time.sleep(0.02)
    return seq


if __name__ == "__main__":
    main()
