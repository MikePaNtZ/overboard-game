#!/usr/bin/env python3
"""Measure input -> sim -> state latency on the live wire, without Unreal.

Stands in for the game: binds the state port (9601), sends InputIn to sim-host (9602) at 100 Hz,
arms the board, and every PERIOD seconds sets the kick bit (InputIn flag bit 2) in one packet.
The latency of one sample is the time from that send to the first StateOut packet whose world
angular velocity changes by more than THRESH rad/s. It also reports the gaps between state packets.

Run it with the game NOT running (both use port 9601):
  tools/play/run_sim.sh --stats-path /tmp/stats.txt &   # then
  tools/play/latency_probe.py [samples]
"""
import socket
import statistics
import struct
import sys
import time

STATE_ADDR = ("127.0.0.1", 9601)
INPUT_ADDR = ("127.0.0.1", 9602)
STATE_MAGIC, INPUT_MAGIC = 0x4F425731, 0x4F424931
ARM, RESET, KICK = 1 << 0, 1 << 1, 1 << 2
RESET_SECONDS = 0.3  # a kick makes the board fall; reset, then arm again before the next kick
PERIOD = 2.0
THRESH = 0.05  # rad/s away from the value at the kick; balance noise is far smaller
SEND_HZ = 100.0


def input_packet(seq: int, flags: int) -> bytes:
    return struct.pack("<IHHQfff", INPUT_MAGIC, 1, flags, seq, 0.0, 0.0, 0.0)


def parse_state(buf: bytes):
    if len(buf) != 104:
        return None
    magic, ver, flags, seq = struct.unpack_from("<IHHQ", buf, 0)
    if magic != STATE_MAGIC or ver != 3:
        return None
    ang = struct.unpack_from("<3f", buf, 92)
    return seq, flags, ang


def main() -> None:
    samples = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(STATE_ADDR)
    rx.setblocking(False)
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    seq = 0
    lat_ms, gaps_ms = [], []
    last_rx = None
    last_ang = None
    kick_sent_at = None
    reset_until = 0.0
    kick_base = None
    next_send = time.perf_counter()
    next_kick = next_send + 1.5
    while len(lat_ms) < samples:
        now = time.perf_counter()
        if now >= next_send:
            flags = RESET if now < reset_until else ARM
            if kick_sent_at is None and now >= next_kick and now >= reset_until + 0.5:
                flags |= KICK
                kick_sent_at = now
                kick_base = last_ang
                next_kick = now + PERIOD
            tx.sendto(input_packet(seq, flags), INPUT_ADDR)
            seq += 1
            next_send += 1.0 / SEND_HZ
        while True:
            try:
                buf = rx.recv(512)
            except BlockingIOError:
                break
            t = time.perf_counter()
            st = parse_state(buf)
            if st is None:
                continue
            _, _, ang = st
            if last_rx is not None:
                gaps_ms.append(1e3 * (t - last_rx))
            last_rx = t
            if kick_sent_at is not None and kick_base is not None:
                jump = max(abs(a - b) for a, b in zip(ang, kick_base))
                if jump > THRESH:
                    lat_ms.append(1e3 * (t - kick_sent_at))
                    print(f"kick {len(lat_ms)}: {lat_ms[-1]:.1f} ms", flush=True)
                    kick_sent_at = None
                    reset_until = t + 0.2
            last_ang = ang
        time.sleep(0.0005)

    def pct(xs, p):
        xs = sorted(xs)
        return xs[min(len(xs) - 1, int(p / 100.0 * len(xs)))]

    print(f"input -> sim -> state, {len(lat_ms)} kicks: "
          f"median {statistics.median(lat_ms):.1f} ms, p90 {pct(lat_ms, 90):.1f} ms, max {max(lat_ms):.1f} ms")
    print(f"state packet gaps, {len(gaps_ms)} packets: "
          f"median {statistics.median(gaps_ms):.2f} ms, p99 {pct(gaps_ms, 99):.1f} ms, max {max(gaps_ms):.1f} ms")


if __name__ == "__main__":
    main()
