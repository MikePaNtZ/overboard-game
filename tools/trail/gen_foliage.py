#!/usr/bin/env python3
"""Procedural ground foliage for OB_Trail: grass clumps, wildflowers, ferns and reeds, as true
blade geometry (no alpha cards) for Nanite. Runs OUTSIDE Unreal (numpy). Course-independent.

usage: gen_foliage.py <out_dir>      writes <kind>.obm (Unreal frame, cm, origin at the base)

Colour lives in the vertex colours (sRGB): RGB is the albedo, A runs 0 at the root to 1 at the tip
(the material reads it as ambient occlusion and as the root-to-tip gradient). UV0 is (across,
along) each blade. No asset pack provides grass or flowers locally (see docs/trail-level.md),
which is why these exist.
"""
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import obm  # noqa: E402


def srgb(c):
    return np.clip(np.asarray(c, float), 0, 255)


class Builder:
    def __init__(self, seed):
        self.rng = np.random.default_rng(seed)
        self.mesh = obm.Mesh()

    def strip(self, pts, widths, side, col_root, col_tip, section="Foliage", up_bias=0.45, ao_root=0.25):
        """A flat ribbon along pts (k, 3) with half-widths (k,), oriented by side (3,) vectors."""
        k = len(pts)
        side = np.asarray(side, float)
        if side.ndim == 1:
            side = np.tile(side, (k, 1))
        L = pts[:, None, :] + np.stack([-side, side], 1) * widths[:, None, None]
        P = L.reshape(-1, 3)
        t = np.gradient(pts, axis=0)
        n = np.cross(side, t)
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
        n = n * (1 - up_bias) + np.array([0, 0, 1.0]) * up_bias
        N = np.repeat(n, 2, axis=0)
        s = np.linspace(0, 1, k)
        uv = np.stack([np.tile([0.0, 1.0], k), np.repeat(s, 2)], 1)
        c = col_root[None, :] * (1 - s[:, None]) + col_tip[None, :] * s[:, None]
        a = ao_root + (1 - ao_root) * s
        col = np.concatenate([np.repeat(c, 2, axis=0), np.repeat(a * 255, 2)[:, None]], 1)
        tri = []
        for i in range(k - 1):
            a0, b0, a1, b1 = 2 * i, 2 * i + 1, 2 * i + 2, 2 * i + 3
            tri += [(a0, a1, b0), (b0, a1, b1)]
        self.mesh.add(P, N, uv, np.array(tri), section, col=col)

    def blade(self, base, height, lean_az, lean, width, col_root, col_tip, segs=5, curl=1.6, twist=0.0):
        r = self.rng
        s = np.linspace(0, 1, segs + 1)
        ang = lean * s ** curl                      # bends more toward the tip
        dirx, diry = math.cos(lean_az), math.sin(lean_az)
        # integrate the curve: each segment points at angle ang from vertical toward lean_az
        seg = height / segs
        pts = [np.array(base, float)]
        for i in range(1, segs + 1):
            a = ang[i]
            step = np.array([dirx * math.sin(a), diry * math.sin(a), math.cos(a)]) * seg
            pts.append(pts[-1] + step)
        pts = np.array(pts)
        tw = lean_az + math.pi / 2 + twist * s
        side = np.stack([np.cos(tw), np.sin(tw), np.zeros_like(s)], 1)
        w = width * (1 - s ** 1.4) + 0.02
        self.strip(pts, w, side, col_root, col_tip)
        return pts

    def write(self, path):
        return self.mesh.write(path, colors=True)


GREEN_ROOT = srgb([46, 58, 22])
GREEN_TIPS = [srgb([104, 126, 46]), srgb([86, 116, 38]), srgb([122, 130, 58]), srgb([74, 102, 34])]
DRY_TIP = srgb([176, 150, 92])


def grass(seed, n, h0, h1, r0, w0, dry=0.15, seed_heads=0, dark=1.0):
    b = Builder(seed)
    r = b.rng
    for _ in range(n):
        rr = r0 * math.sqrt(r.random())
        th = r.uniform(0, 2 * math.pi)
        base = (rr * math.cos(th), rr * math.sin(th), -1.0)
        h = r.uniform(h0, h1)
        tip = GREEN_TIPS[r.integers(len(GREEN_TIPS))]
        if r.random() < dry:
            tip = DRY_TIP * r.uniform(0.85, 1.05)
        b.blade(base, h, th + r.uniform(-0.6, 0.6), r.uniform(0.15, 0.75), w0 * r.uniform(0.7, 1.3),
                GREEN_ROOT * dark, tip * dark, segs=5, twist=r.uniform(-0.8, 0.8))
    for _ in range(seed_heads):
        th = r.uniform(0, 2 * math.pi)
        base = (r0 * 0.5 * math.cos(th), r0 * 0.5 * math.sin(th), -1.0)
        h = r.uniform(h1 * 1.05, h1 * 1.35)
        stem = b.blade(base, h, th, r.uniform(0.1, 0.35), 0.18, srgb([70, 82, 40]), srgb([150, 140, 80]), segs=6)
        top, d = stem[-1], stem[-1] - stem[-2]
        d /= np.linalg.norm(d)
        # a panicle: small spikelets along the last 18 % of the stem
        for k in range(14):
            p = stem[-2] + (stem[-1] - stem[-2]) * r.random() + d * r.uniform(0, 8)
            az = r.uniform(0, 2 * math.pi)
            b.blade(tuple(p), r.uniform(2.0, 4.0), az, r.uniform(0.6, 1.2), 0.45,
                    srgb([150, 138, 84]), srgb([196, 176, 120]), segs=2, curl=1.0)
    return b


def flower(seed, kind):
    b = Builder(seed)
    r = b.rng
    # a few leaves at the base
    for _ in range(10):
        th = r.uniform(0, 2 * math.pi)
        b.blade((r.uniform(-3, 3), r.uniform(-3, 3), -1.0), r.uniform(10, 22), th, r.uniform(0.5, 1.1), 0.9,
                GREEN_ROOT, srgb([92, 112, 44]), segs=4)
    stems = {"daisy": (3, 6), "buttercup": (4, 8), "knapweed": (2, 4)}[kind]
    for _ in range(r.integers(*stems)):
        th = r.uniform(0, 2 * math.pi)
        base = (r.uniform(-4, 4), r.uniform(-4, 4), -1.0)
        h = r.uniform(28, 52) if kind != "buttercup" else r.uniform(22, 44)
        stem = b.blade(base, h, th, r.uniform(0.05, 0.3), 0.22, srgb([58, 76, 32]), srgb([84, 104, 42]), segs=6)
        top = stem[-1]
        up = stem[-1] - stem[-2]
        up /= np.linalg.norm(up)
        if kind == "daisy":
            n_p, plen, pw = 18, r.uniform(1.3, 1.8), 0.32
            for k in range(n_p):
                a = 2 * math.pi * k / n_p + r.uniform(-0.08, 0.08)
                d = np.array([math.cos(a), math.sin(a), r.uniform(-0.05, 0.12)])
                pts = np.array([top + d * plen * t for t in (0.15, 0.6, 1.0)])
                side = np.array([-math.sin(a), math.cos(a), 0.0])
                b.strip(pts, np.array([pw * 0.6, pw, pw * 0.5]), side, srgb([238, 236, 226]), srgb([250, 250, 244]),
                        up_bias=0.6, ao_root=0.9)
            disc(b, top + up * 0.15, 0.45, srgb([214, 160, 30]))
        elif kind == "buttercup":
            for k in range(5):
                a = 2 * math.pi * k / 5 + r.uniform(-0.1, 0.1)
                d = np.array([math.cos(a) * 0.8, math.sin(a) * 0.8, 0.6])
                pts = np.array([top + d * 1.1 * t for t in (0.1, 0.55, 1.0)])
                side = np.array([-math.sin(a), math.cos(a), 0.0])
                b.strip(pts, np.array([0.35, 0.6, 0.4]), side, srgb([226, 178, 18]), srgb([248, 210, 40]),
                        up_bias=0.5, ao_root=0.8)
            disc(b, top + up * 0.2, 0.25, srgb([200, 150, 20]))
        else:  # knapweed: a dense purple tuft on a dark bract cup
            disc(b, top, 0.55, srgb([70, 66, 40]))
            for k in range(40):
                a = r.uniform(0, 2 * math.pi)
                el = r.uniform(0.2, 1.2)
                d = np.array([math.cos(a) * math.cos(el), math.sin(a) * math.cos(el), math.sin(el)])
                pts = np.array([top + d * t for t in (0.2, 1.0, 1.9)])
                side = np.array([-math.sin(a), math.cos(a), 0.0])
                b.strip(pts, np.array([0.12, 0.1, 0.04]), side, srgb([128, 52, 120]), srgb([170, 80, 160]),
                        up_bias=0.5, ao_root=0.8)
    return b


def disc(b, c, rad, col, n=10):
    a = np.linspace(0, 2 * math.pi, n + 1)[:-1]
    P = np.concatenate([[c + np.array([0, 0, 0.1])], c + np.stack([np.cos(a) * rad, np.sin(a) * rad, np.zeros(n)], 1)])
    N = np.tile([0, 0, 1.0], (n + 1, 1))
    uv = np.zeros((n + 1, 2))
    colr = np.tile(np.append(col, 255), (n + 1, 1))
    tri = [(0, 1 + (k + 1) % n, 1 + k) for k in range(n)]
    b.mesh.add(P, N, uv, np.array(tri), "Foliage", col=colr)


def fern(seed):
    b = Builder(seed)
    r = b.rng
    n_fronds = r.integers(7, 12)
    for f in range(n_fronds):
        az = 2 * math.pi * f / n_fronds + r.uniform(-0.25, 0.25)
        length = r.uniform(45, 85)
        rise = r.uniform(0.35, 0.75)            # launch angle above horizontal
        segs = 14
        s = np.linspace(0, 1, segs + 1)
        el = rise - (rise + 0.5) * s ** 1.5     # arches over
        dx, dy = math.cos(az), math.sin(az)
        pts = [np.array([dx * 2, dy * 2, -1.0])]
        for i in range(1, segs + 1):
            e = el[i]
            pts.append(pts[-1] + np.array([dx * math.cos(e), dy * math.cos(e), math.sin(e)]) * length / segs)
        pts = np.array(pts)
        side = np.array([-dy, dx, 0.0])
        b.strip(pts, np.full(len(pts), 0.25), side, srgb([60, 70, 30]), srgb([96, 112, 44]), up_bias=0.3)
        # pinnae: pairs of leaflets, longest a third of the way out
        for i in range(2, segs):
            t = s[i]
            pl = length * 0.28 * math.sin(math.pi * min(1.0, t * 1.15)) + 1.5
            for sd in (-1, 1):
                d = side * sd * 0.9 + np.array([dx, dy, 0.0]) * 0.45 + np.array([0, 0, -0.15])
                d /= np.linalg.norm(d)
                q = np.array([pts[i] + d * pl * u for u in (0.0, 0.35, 0.7, 1.0)])
                q[:, 2] -= np.array([0, 0.4, 1.2, 2.4]) * (pl / 15)
                w = np.array([0.5, 1.0, 0.85, 0.15]) * (0.9 + pl * 0.06)
                pside = np.cross(d, [0, 0, 1.0])
                tip = srgb([108, 124, 46]) if r.random() > 0.08 else srgb([150, 128, 60])
                b.strip(q, w, pside / max(np.linalg.norm(pside), 1e-6), srgb([52, 66, 26]), tip, up_bias=0.55, ao_root=0.5)
    return b


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    kinds = {
        "grass_meadow_a": lambda: grass(1, 85, 22, 55, 8.0, 0.55, dry=0.12, seed_heads=3),
        "grass_meadow_b": lambda: grass(2, 110, 18, 36, 8.0, 0.5, dry=0.1),
        "grass_meadow_c": lambda: grass(3, 60, 35, 70, 6.0, 0.6, dry=0.3, seed_heads=7),
        "grass_forest": lambda: grass(4, 45, 15, 34, 9.0, 0.5, dry=0.05, dark=0.8),
        "grass_reed": lambda: grass(5, 55, 80, 150, 9.0, 0.9, dry=0.25, seed_heads=3),
        "flower_daisy": lambda: flower(6, "daisy"),
        "flower_buttercup": lambda: flower(7, "buttercup"),
        "flower_knapweed": lambda: flower(8, "knapweed"),
        "fern": lambda: fern(9),
    }
    for name, fn in kinds.items():
        nv, nt = fn().write(os.path.join(out, name + ".obm"))
        print("%-18s %6d verts %6d tris" % (name, nv, nt))


if __name__ == "__main__":
    main()
