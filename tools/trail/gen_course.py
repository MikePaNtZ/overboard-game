#!/usr/bin/env python3
"""Generate the OB_Trail level inputs from a MuJoCo course directory. Runs OUTSIDE Unreal (numpy).

usage: gen_course.py <course_dir> <out_dir> [--seed N] [--no-creek]

The course directory is an overboard-viz carve-lab course (course_height.npy, metadata.json,
course.json, lane.json). The course heightfield is the single source of truth for shape: inside
the course square the landscape IS the course, resampled on every second post, except where the
creek and its pond are cut (outside the path and its verge, and under the bridge deck). Outside
the course square the generator extends the valley and adds hills; nothing there is ridden.

Frames. MuJoCo is right-handed, Z up, metres; board forward is -X. Unreal is left-handed, Z up,
cm. ABoardActor maps MuJoCo (x, y, z) to Unreal (100x, -100y, 100z), then rotates by the
PlayerStart yaw and adds its location. OB_Trail puts the PlayerStart at the world origin with
yaw 0, so the MuJoCo origin is the landscape centre and Unreal (X, Y) = (100x, -100y).

Outputs in <out_dir> (all read by build_trail_level.py):
  meta.json            landscape layout, path, creek, bridge, verification numbers
  height_main.r16      2033 x 2033 uint16, 0.2 m posts (+-203.2 m), Z scale 40
  layer_<Name>.r8      the landscape paint layers, uint8 weights that sum to 255
  height_far.r16       1009 x 1009 uint16, 10 m posts (+-5.04 km), the backdrop hills
  path.obm, bridge.obm, water.obm   exact meshes (see obm.py)
  scatter.json         instance transforms per kind (Unreal cm, degrees)
"""
import argparse
import json
import math
import os
import sys
import time

import numpy as np
from scipy import ndimage
from scipy.spatial import cKDTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import obm  # noqa: E402

# --- landscape layout ---------------------------------------------------------------------------
N = 2033            # 8 x 8 components of 2 x 2 sections of 127 quads
SECTIONS, QUADS = 2, 127
DX = 0.2            # m per landscape quad: every fourth course post. The path surface is a profile
                    # in x plus a crown in y, so a 0.2 m grid holds it to the 16-bit step (1.6 mm);
                    # 0.1 m would quadruple the level file for no gain on the ridden surface.
C = (N - 1) // 2    # centre vertex = MuJoCo origin
ZS = 40.0           # landscape Z scale: 1 LSB = ZS/128 cm = 3.1 mm, range +-102.4 m
NF = 1009           # backdrop: 16 x 16 components of 63 quads
QF = 63
DXF = 10.0
CF = (NF - 1) // 2
ZSF = 300.0         # 1 LSB = 2.3 cm, range +-768 m

RIBBON_LIFT_M = 0.003   # the asphalt mesh sits this far above the landscape it covers
WHEEL_R = 0.1454


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


class Noise:
    """Value-noise fBm on a world-space lattice (metres), cubic interpolation, deterministic."""

    def __init__(self, seed):
        self.rng = np.random.default_rng(seed)
        self.lat = {}

    def octave(self, x, y, wavelength, key):
        if key not in self.lat:
            self.lat[key] = self.rng.standard_normal((256, 256)).astype(np.float32)
        g = self.lat[key]
        return ndimage.map_coordinates(g, [np.asarray(y) / wavelength + 128.3, np.asarray(x) / wavelength + 128.7],
                                       order=3, mode="grid-wrap")

    def fbm(self, x, y, wavelength, octaves=4, gain=0.5, key=0):
        out = np.zeros(np.shape(x), np.float32)
        amp, wl, tot = 1.0, wavelength, 0.0
        for o in range(octaves):
            out += amp * self.octave(x, y, wl, (key, o))
            tot += amp
            amp *= gain
            wl /= 2.03
        return out / tot / 0.45  # ~unit variance


def load_course(cdir):
    meta = json.load(open(os.path.join(cdir, "metadata.json")))
    course = json.load(open(os.path.join(cdir, "course.json")))
    lane = np.array(json.load(open(os.path.join(cdir, "lane.json"))), float)
    h = np.load(os.path.join(cdir, "course_height.npy")).astype(np.float64)
    assert h.shape == (meta["nrow"], meta["ncol"]), "course_height.npy does not match metadata.json"
    if course["path"]["axis"] not in ("-X", "+X"):
        raise SystemExit("only paths along X are supported (course.json path.axis)")
    return meta, course, lane, h


def course_sampler(meta, h):
    sp = meta["spacing_m"]
    c0 = meta["center_post_index"]

    def sample(x, y, order=1):
        # height[row, col]: row with MuJoCo +Y, col with +X. mode=nearest clamps outside the square.
        return ndimage.map_coordinates(h, [c0 + np.asarray(y) / sp, c0 + np.asarray(x) / sp], order=order, mode="nearest")
    return sample


def lane_fn(lane, half_width):
    xs = lane[:, 0]
    o = np.argsort(xs)
    xs, mid = xs[o], 0.5 * (lane[o, 1] + lane[o, 2])

    def centre(x):
        return np.interp(x, xs, mid)
    return centre, half_width


def floor_segment(course):
    """x range of the valley floor: the 0 % segment with the lowest profile height."""
    s0, best = 0.0, None
    prof_s, prof_z = np.array(course["profile"]["s_m"]), np.array(course["profile"]["z_m"])
    sign = -1.0 if course["path"]["axis"] == "-X" else 1.0
    x_start = course["path"]["start_x_m"]
    for seg in course["segments"]:
        s1 = s0 + seg["length_m"]
        if abs(seg["grade_pct"]) < 1e-6 and s1 - s0 >= 12.0:
            z = float(np.interp(0.5 * (s0 + s1), prof_s, prof_z))
            if best is None or z < best[2]:
                best = (s0, s1, z)
        s0 = s1
    if best is None:
        return None
    xa, xb = x_start + sign * best[0], x_start + sign * best[1]
    return min(xa, xb), max(xa, xb), best[2]


# --- creek and pond -----------------------------------------------------------------------------
class Creek:
    """A brook that comes down the +Y wall, crosses under the path at the valley floor and ends in
    a pond on the -Y side. Water levels never rise downstream. All in MuJoCo metres."""

    WC = 1.3          # water half-width
    BANK = 0.55       # bank rise per metre beyond the water's edge
    FREEBOARD = 0.70  # deck/path height above the water at the crossing

    def __init__(self, floor, sample, noise, path_centre):
        xa, xb, zf = floor
        self.xc = 0.5 * (xa + xb)
        self.zf = float(sample(np.array([self.xc]), np.array([path_centre(self.xc)]))[0])
        self.L0 = self.zf - self.FREEBOARD
        yc = float(path_centre(self.xc))
        ys = np.arange(yc - 9.0, yc + 48.0, 0.05)
        b = smoothstep(4.0, 10.0, np.abs(ys - yc))
        xs = self.xc + b * (2.2 * np.sin((ys - yc) / 8.0) + 1.1 * np.sin((ys - yc) / 3.7 + 1.0))
        terr = sample(xs, ys, order=1)
        depth = 0.75 + 0.6 * smoothstep(8.0, 30.0, ys - yc)
        L = np.where(ys - yc < 7.0, self.L0, terr - depth)
        # flow runs toward -Y: the level may only rise with y
        L = np.maximum(self.L0, np.maximum.accumulate(L))
        self.cx, self.cy, self.L = xs, ys, L
        self.tree = cKDTree(np.stack([xs, ys], 1))
        # pond on the -Y side, its shore a few metres off the verge
        self.pc = np.array([self.xc + 0.8, yc - 13.5])
        th = np.linspace(0, 2 * np.pi, 361)
        r = 7.6 + 1.3 * noise.fbm(np.cos(th) * 20, np.sin(th) * 20, 18.0, 3, key=77)
        self.pth, self.pr = th, np.clip(r, 6.0, 9.2)
        self.yc = yc

    def pond_radius(self, ang):
        return np.interp(np.mod(ang, 2 * np.pi), self.pth, self.pr)

    def surface(self, x, y):
        """Carved ground height (inf where the creek does not cut) and water distance terms."""
        d, k = self.tree.query(np.stack([x, y], 1), distance_upper_bound=12.0)
        k = np.minimum(k, len(self.L) - 1)
        L = self.L[k]
        dd = np.where(np.isfinite(d), d, 99.0)
        bed = L - 0.45 * (1 - (dd / self.WC) ** 2) - 0.06
        e = dd - self.WC
        s_creek = np.where(dd < self.WC, bed, L + np.minimum(e, 2.0) * self.BANK + np.maximum(e - 2.0, 0.0) * 0.9)
        s_creek = np.where(dd < 12.0, s_creek, np.inf)
        px, py = x - self.pc[0], y - self.pc[1]
        rho, ang = np.hypot(px, py), np.arctan2(py, px)
        rr = self.pond_radius(ang)
        e = rho - rr
        s_pond = np.where(rho < rr, self.L0 - 1.1 * (1 - (rho / rr) ** 2) - 0.08,
                          self.L0 + np.minimum(e, 2.5) * 0.42 + np.maximum(e - 2.5, 0.0) * 0.85)
        return np.minimum(s_creek, s_pond), dd, rho - rr

    def water_mesh(self, to_ue):
        m = obm.Mesh()
        # creek ribbon, upstream to the pond
        sel = np.arange(0, len(self.cx), 5)
        x, y, L = self.cx[sel], self.cy[sel], self.L[sel]
        t = np.gradient(np.stack([x, y], 1), axis=0)
        t /= np.linalg.norm(t, axis=1, keepdims=True)
        nrm = np.stack([-t[:, 1], t[:, 0]], 1)
        across = np.linspace(-(self.WC + 0.9), self.WC + 0.9, 9)
        P = np.zeros((len(x), len(across), 3))
        for a, off in enumerate(across):
            P[:, a, 0] = x + nrm[:, 0] * off
            P[:, a, 1] = y + nrm[:, 1] * off
            P[:, a, 2] = L
        s = np.concatenate([[0], np.cumsum(np.hypot(np.diff(x), np.diff(y)))])
        uv = np.stack(np.meshgrid(s[::-1], across, indexing="ij"), -1)[..., ::-1]
        Pu = to_ue(P.reshape(-1, 3))
        nr = np.tile([0, 0, 1.0], (len(Pu), 1))
        tri = obm.grid_tris(len(x), len(across))
        m.add(Pu, nr, uv.reshape(-1, 2), orient_up(Pu, tri), "Creek")
        # pond disc
        rs = np.linspace(0, 1, 24)
        th = np.linspace(0, 2 * np.pi, 145)
        R, T = np.meshgrid(rs, th, indexing="ij")
        rad = R * (self.pond_radius(T) + 1.6)
        P = np.stack([self.pc[0] + rad * np.cos(T), self.pc[1] + rad * np.sin(T), np.full_like(R, self.L0)], -1)
        Pu = to_ue(P.reshape(-1, 3))
        uv = P.reshape(-1, 3)[:, :2]
        tri = obm.grid_tris(len(rs), len(th))
        m.add(Pu, np.tile([0, 0, 1.0], (len(Pu), 1)), uv, orient_up(Pu, tri), "Pond")
        return m


def orient_up(P, tri):
    """Wind every triangle so its geometric normal faces +Z in Unreal (clockwise from above)."""
    a, b, c = P[tri[:, 0]], P[tri[:, 1]], P[tri[:, 2]]
    nz = np.cross(b - a, c - a)[:, 2]
    # Unreal: a front face is clockwise seen from its front. With X forward and Y right, seen from
    # above, that is a negative Z component of (b-a)x(c-a) in Unreal's own (left-handed) axes.
    flip = nz > 0
    t = tri.copy()
    t[flip] = t[flip][:, ::-1]
    return t


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("course_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--no-creek", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    out = lambda n: os.path.join(a.out_dir, n)
    meta, course, lane, hc = load_course(a.course_dir)
    sample = course_sampler(meta, hc)
    noise = Noise(a.seed)
    half_ext = meta["half_extent_m"]
    path_half = 0.5 * course["path"]["width_m"]
    path_centre, _ = lane_fn(lane, path_half)
    verge = 1.5

    def to_ue(P):
        P = np.asarray(P, float)
        return np.stack([P[..., 0] * 100.0, -P[..., 1] * 100.0, P[..., 2] * 100.0], -1)

    # ---- main heights -----------------------------------------------------------------------------
    log("main heights")
    idx = np.arange(N)
    X1 = (idx - C) * DX                     # MuJoCo x of landscape column i
    Y1 = -(idx - C) * DX                    # MuJoCo y of landscape row j (Unreal +Y = MuJoCo -Y)
    X, Y = np.meshgrid(X1, Y1)              # [j, i]
    H = sample(X, Y).astype(np.float64)     # inside: exact posts; outside: edge-clamped
    inside = (np.abs(X) <= half_ext + 1e-6) & (np.abs(Y) <= half_ext + 1e-6)

    # outer ring: the valley continues along X; hills rise beyond the walls
    dxo = np.maximum(np.abs(X) - half_ext, 0.0)
    dyo = np.maximum(np.abs(Y) - half_ext, 0.0)
    dout = np.hypot(dxo, dyo)
    lat = np.abs(Y - path_centre(X))
    corridor = smoothstep(9.0, 30.0, lat)
    rise = 0.20 * dyo + 0.0007 * dyo ** 2
    n_big = noise.fbm(X, Y, 90.0, 4, key=1)
    grow = corridor * (rise + 7.0 * (1 - np.exp(-dout / 45.0)) * n_big)
    H = np.where(inside, H, H + grow)

    # creek and pond
    creek = None
    floor = None if a.no_creek else floor_segment(course)
    carve_mask = np.zeros_like(H, bool)
    cdist = np.full_like(H, 99.0)
    pdist = np.full_like(H, 99.0)
    if floor:
        log("creek at the valley floor, x %.1f..%.1f" % (floor[0], floor[1]))
        creek = Creek(floor, sample, noise, path_centre)
        box = (np.abs(X - creek.xc) < 40) & (Y > creek.yc - 40) & (Y < creek.yc + 56)
        jj, ii = np.nonzero(box)
        S, dd, pd = creek.surface(X[jj, ii], Y[jj, ii])
        cut = S < H[jj, ii]
        H[jj[cut], ii[cut]] = S[cut]
        carve_mask[jj[cut], ii[cut]] = True
        edge = (np.abs(X[jj, ii] - creek.xc) > 39) | (Y[jj, ii] < creek.yc - 39) | (Y[jj, ii] > creek.yc + 55)
        if (cut & edge).any():
            raise SystemExit("the creek carve reaches the edge of its box; widen the box")
        cdist[jj, ii] = dd
        pdist[jj, ii] = pd

    # ---- quantise -------------------------------------------------------------------------------------
    q = np.clip(np.round(32768.0 + H * 100.0 * 128.0 / ZS), 0, 65535).astype(np.uint16)
    Hq = (q.astype(np.float64) - 32768.0) * ZS / 128.0 / 100.0
    q.astype("<u2").tofile(out("height_main.r16"))
    log("height_main.r16 written, range %.2f..%.2f m" % (Hq.min(), Hq.max()))

    # ---- verification: landscape vs course on the ridden surface --------------------------------------
    on_path = inside & (lat <= path_half) & ~carve_mask
    on_verge = inside & (lat <= path_half + verge) & ~carve_mask
    err_path = np.abs(Hq - sample(X, Y))[on_path]
    err_verge = np.abs(Hq - sample(X, Y))[on_verge]
    err_inside = np.abs(Hq - sample(X, Y))[inside & ~carve_mask]
    ver = dict(path_max_err_mm=float(err_path.max() * 1000), verge_max_err_mm=float(err_verge.max() * 1000),
               inside_uncarved_max_err_mm=float(err_inside.max() * 1000), ribbon_lift_mm=RIBBON_LIFT_M * 1000,
               carved_posts=int(carve_mask.sum()), carved_on_path=int((carve_mask & inside & (lat <= path_half)).sum()))
    log("verify", ver)

    # ---- layers --------------------------------------------------------------------------------------
    log("layers")
    gy, gx = np.gradient(Hq, DX)
    slope = np.degrees(np.arctan(np.hypot(gx, gy)))
    n1 = noise.fbm(X, Y, 4.0, 3, key=2)
    n2 = noise.fbm(X, Y, 22.0, 3, key=3)
    d = lat - path_half                       # metres beyond the asphalt edge
    rem = np.ones_like(H)
    W = {}

    def take(name, w):
        w = np.clip(w, 0, 1) * rem
        W[name] = w
        return rem - w

    wcreek = np.zeros_like(H)
    if creek is not None:
        wcreek = np.maximum(1 - smoothstep(Creek.WC + 0.4, Creek.WC + 1.6, cdist), 1 - smoothstep(0.6, 2.2, pdist))
        wcreek = wcreek * smoothstep(path_half - 0.2, path_half + 0.6, lat)  # not on the asphalt
    rem = take("Creek", wcreek)
    rem = take("Asphalt", 1 - smoothstep(-0.02, 0.08, d))
    g_edge = 0.95 + 0.25 * n1
    rem = take("Gravel", 1 - smoothstep(g_edge - 0.15, g_edge + 0.25, d))
    rock = smoothstep(25.0, 31.0, slope + 4.0 * n1 + 6.0 * n2) * smoothstep(0.0, 0.6, n2 + 0.2) * smoothstep(6.0, 9.0, d)
    if creek is not None:
        rock = np.maximum(rock, smoothstep(0.0, 0.8, 1.0 - np.abs(cdist - Creek.WC - 0.6)) * smoothstep(9.0, 16.0, Y - creek.yc) * 0.8)
    rem = take("Rock", rock)
    m_edge = 6.0 + 2.2 * n2 + 0.8 * n1
    rem = take("Meadow", 1 - smoothstep(m_edge - 1.5, m_edge + 1.5, d))
    W["Forest"] = rem
    names = ["Asphalt", "Gravel", "Meadow", "Forest", "Rock", "Creek"]
    stack = np.stack([W[n] for n in names])
    w8 = np.floor(stack * 255.0 + 0.5).astype(np.int32)
    w8[names.index("Forest")] += 255 - w8.sum(0)  # exact sum 255
    w8 = np.clip(w8, 0, 255).astype(np.uint8)
    for k, n in enumerate(names):
        w8[k].tofile(out("layer_%s.r8" % n))
    del stack

    # ---- path ribbon ----------------------------------------------------------------------------------
    log("path ribbon")
    interp = lambda xx, yy: ndimage.map_coordinates(Hq, [C - yy / DX, C + xx / DX], order=1, mode="nearest")
    bridge = None
    if creek is not None:
        span = Creek.WC + Creek.FREEBOARD / Creek.BANK + 0.85
        bridge = dict(x0=creek.xc - span, x1=creek.xc + span, xc=creek.xc, yc=creek.yc)
    xs_all = X1[(X1 >= X1[0]) & (X1 <= X1[-1])]
    across = np.linspace(-path_half, path_half, int(round(2 * path_half / DX)) + 1)
    path_mesh = obm.Mesh()
    pieces = [xs_all] if bridge is None else [xs_all[xs_all <= bridge["x0"] + 1e-6], xs_all[xs_all >= bridge["x1"] - 1e-6]]
    for xs in pieces:
        XX, AA = np.meshgrid(xs, across, indexing="ij")
        YY = path_centre(XX) + AA
        ZZ = interp(XX, YY) + RIBBON_LIFT_M
        P = to_ue(np.stack([XX, YY, ZZ], -1))
        nrm = obm.grid_normals(P)
        nrm *= np.sign(nrm[..., 2:3] + 1e-9)
        uv = np.stack([AA + path_half, -XX], -1)  # u across (m), v along the riding direction (m)
        tri = orient_up(P.reshape(-1, 3), obm.grid_tris(len(xs), len(across)))
        path_mesh.add(P.reshape(-1, 3), nrm.reshape(-1, 3), uv.reshape(-1, 2), tri, "Asphalt")
    nv, nt = path_mesh.write(out("path.obm"))
    log("path.obm %d verts %d tris" % (nv, nt))

    # ---- bridge ---------------------------------------------------------------------------------------
    if bridge is not None:
        b_x = np.linspace(bridge["x0"], bridge["x1"], 50)
        BX, BA = np.meshgrid(b_x, np.linspace(-path_half, path_half, 21), indexing="ij")
        zs = sample(BX, path_centre(BX) + BA)
        deck = 0.5 * (zs.max() + zs.min())
        bridge.update(deck_z=float(deck), deck_err_mm=float(0.5 * (zs.max() - zs.min()) * 1000))
        make_bridge(bridge, path_centre, to_ue, np.random.default_rng(a.seed + 5)).write(out("bridge.obm"), uv1=True)
        creek.water_mesh(to_ue).write(out("water.obm"), uv1=False)
        log("bridge deck z %.4f (+-%.1f mm to the course), span x %.2f..%.2f" % (deck, bridge["deck_err_mm"], bridge["x0"], bridge["x1"]))

    # ---- backdrop -------------------------------------------------------------------------------------
    log("backdrop")
    f1 = (np.arange(NF) - CF) * DXF
    FX, FY = np.meshgrid(f1, -f1)
    ext = (N - 1) * DX / 2.0
    E = ndimage.map_coordinates(Hq, [C - np.clip(FY, -ext, ext) / DX, C + np.clip(FX, -ext, ext) / DX], order=1, mode="nearest")
    fdx = np.maximum(np.abs(FX) - ext, 0.0)
    fdy = np.maximum(np.abs(FY) - ext, 0.0)
    fd = np.hypot(fdx, fdy)
    flat = np.abs(FY - path_centre(np.clip(FX, -half_ext, half_ext)))
    valley = smoothstep(40.0, 900.0, flat)            # the valley stays open along X
    ridged = np.clip(1.0 - np.abs(noise.fbm(FX, FY, 1400.0, 5, 0.5, key=9)), 0.0, 1.0)
    hills = valley * (0.12 * fd + 260.0 * smoothstep(0.0, 2500.0, fd) * ridged ** 1.6)
    hills += 18.0 * smoothstep(0.0, 400.0, fd) * noise.fbm(FX, FY, 260.0, 4, key=10)
    HF = np.where(fd > 0, E + hills, E - 1.5)
    qf = np.clip(np.round(32768.0 + HF * 100.0 * 128.0 / ZSF), 0, 65535).astype(np.uint16)
    qf.astype("<u2").tofile(out("height_far.r16"))

    # ---- scatter --------------------------------------------------------------------------------------
    log("scatter")
    scat = scatter(a.seed, Hq, X1, Y1, path_centre, path_half, verge, slope, w8, names, creek, interp, noise, HF, f1)
    json.dump(scat, open(out("scatter.json"), "w"), separators=(",", ":"))
    log("scatter", {k: len(v) for k, v in scat["kinds"].items()})

    m = dict(
        course_dir=os.path.abspath(a.course_dir), seed=a.seed,
        main=dict(size=N, sections=SECTIONS, quads=QUADS, scale=[DX * 100, DX * 100, ZS],
                  location=[-C * DX * 100, -C * DX * 100, 0.0], heights="height_main.r16",
                  layers=names, layer_files=["layer_%s.r8" % n for n in names]),
        far=dict(size=NF, sections=1, quads=QF, scale=[DXF * 100, DXF * 100, ZSF],
                 location=[-CF * DXF * 100, -CF * DXF * 100, 0.0], heights="height_far.r16"),
        origin_cm=[0.0, 0.0, 0.0], origin_yaw_deg=0.0,
        path=dict(half_width_m=path_half, verge_m=verge, start_x_m=course["path"]["start_x_m"], axis=course["path"]["axis"],
                  spawn_x_m=course.get("spawn_x_m"), x_end_m=float(lane[:, 0].min()), ribbon_lift_m=RIBBON_LIFT_M),
        creek=None if creek is None else dict(xc=creek.xc, water_z=creek.L0, pond_centre=creek.pc.tolist()),
        bridge=bridge, verify=ver,
    )
    json.dump(m, open(out("meta.json"), "w"), indent=1)
    log("done")


def make_bridge(b, path_centre, to_ue, rng):
    """A timber deck on four glulam stringers and two concrete abutments, with a post-and-rail
    railing. Unreal cm. The plank tops are the ridden surface (deck_z, MuJoCo)."""
    m = obm.Mesh()
    yc = float(path_centre(b["xc"]))
    z = b["deck_z"] * 100.0
    x0, x1 = b["x0"] * 100.0, b["x1"] * 100.0
    half_w = 320.0
    plank_w, gap, th = 18.0, 1.4, 6.0
    x = x0 + 4.0
    k = 0
    while x + plank_w <= x1 - 4.0 + 1e-6:
        cx = x + plank_w / 2
        jitter = rng.uniform(-0.12, 0.08)
        p, n, uv, t = obm.box((cx, -yc * 100, z - th / 2 + jitter), (plank_w, 2 * half_w + rng.uniform(-3, 3), th),
                              yaw_deg=rng.uniform(-0.25, 0.25), uv_offset=(rng.uniform(0, 4), rng.uniform(0, 4)))
        m.add(p, n, uv, t, "Wood", uv1=np.tile([k % 7 / 7.0, 0.0], (len(p), 1)))
        x += plank_w + gap
        k += 1
    sz = 34.0
    for yy in (-250.0, -85.0, 85.0, 250.0):
        p, n, uv, t = obm.box(((x0 + x1) / 2, -yc * 100 + yy, z - th - sz / 2), (x1 - x0 + 70.0, 20.0, sz))
        m.add(p, n, uv, t, "WoodDark", uv1=np.tile([0.5, 0.0], (len(p), 1)))
    # abutments: concrete, top under the stringers, foot well below the creek bed
    for xa in (x0 - 5.0, x1 + 5.0):
        top = z - th - sz
        p, n, uv, t = obm.box((xa, -yc * 100, top - 140.0), (70.0, 2 * half_w + 60.0, 280.0))
        m.add(p, n, uv, t, "Concrete")
    # railing: posts, a top rail and a mid rail on each side
    for side in (-1, 1):
        yy = -yc * 100 + side * (half_w - 8.0)
        n_posts = int(round((x1 - x0) / 140.0)) + 1
        for xp in np.linspace(x0 + 6.0, x1 - 6.0, n_posts):
            p, n, uv, t = obm.box((xp, yy, z + 52.0 - 20.0), (12.0, 12.0, 144.0), uv_offset=(rng.uniform(0, 3), 0))
            m.add(p, n, uv, t, "Wood", uv1=np.tile([0.3, 0.0], (len(p), 1)))
        for hz, hh in ((104.0, 9.0), (55.0, 7.0)):
            p, n, uv, t = obm.box(((x0 + x1) / 2, yy + side * 1.0, z + hz), (x1 - x0 - 4.0, 7.0, hh))
            m.add(p, n, uv, t, "Wood", uv1=np.tile([0.6, 0.0], (len(p), 1)))
    return m


# --- scatter -------------------------------------------------------------------------------------------
# Footprint radius (m, at scale 1) of each kind: the verge rule tests distance - radius x scale.
# Canopy aspens (aspen_01/02) and snags are exempt from it: their crowns hang high over the path,
# as real ones do; their trunks keep at least 5 m from the asphalt.
FOOT = dict(aspen_01=0.4, aspen_02=0.3, aspen_03=1.0, aspen_04=0.55, hazel_01=2.4, hazel_02=0.9, hazel_03=0.7,
            hazel_04=0.35, snag_birch_a=0.4, snag_birch_c=0.4, snag_alder_b=0.4, rock=0.5,
            grass_meadow_a=0.3, grass_meadow_b=0.3, grass_meadow_c=0.3, grass_forest=0.3, grass_reed=0.45,
            flower_daisy=0.16, flower_buttercup=0.16, flower_knapweed=0.16, fern=0.8)


def scatter(seed, Hq, X1, Y1, path_centre, path_half, verge, slope, w8, names, creek, interp, noise, HF, f1):
    rng = np.random.default_rng(seed + 11)
    li = {n: k for k, n in enumerate(names)}
    ext = (len(X1) - 1) * DX / 2.0

    def at(arr, x, y):
        return ndimage.map_coordinates(arr, [C - y / DX, C + x / DX], order=1, mode="nearest")

    def far_z(x, y):
        return ndimage.map_coordinates(HF, [CF - y / DXF, CF + x / DXF], order=1, mode="nearest") / 1.0

    def jitter_grid(x0, x1, y0, y1, cell):
        gx = np.arange(x0, x1, cell)
        gy = np.arange(y0, y1, cell)
        GX, GY = np.meshgrid(gx, gy)
        return (GX + rng.uniform(0, cell, GX.shape)).ravel(), (GY + rng.uniform(0, cell, GY.shape)).ravel()

    def lat_d(x, y):
        return np.abs(y - path_centre(x)) - path_half

    def wt(name, x, y):
        return at(w8[li[name]].astype(np.float32) / 255.0, x, y)

    kinds = {}

    def emit(kind, x, y, z, yaw, scale, pitch=None, roll=None, sink=0.0):
        n = len(x)
        if n == 0:
            return
        pitch = np.zeros(n) if pitch is None else pitch
        roll = np.zeros(n) if roll is None else roll
        rows = np.stack([x * 100, -y * 100, (z - sink) * 100, yaw, pitch, roll, scale], 1)
        kinds.setdefault(kind, []).extend(np.round(rows, 2).tolist())

    def tilt(x, y, amount):
        gx = (at(Hq, x + 0.3, y) - at(Hq, x - 0.3, y)) / 0.6
        gy = (at(Hq, x, y + 0.3) - at(Hq, x, y - 0.3)) / 0.6
        # Unreal: pitch about Y (nose up = +), roll about X. MuJoCo +y is Unreal -Y.
        return np.degrees(np.arctan(gx)) * amount, np.degrees(np.arctan(gy)) * amount

    # canopy trees on the main landscape: quaking aspen stands, a few dead snags
    E = ext - 1.0
    x, y = jitter_grid(-E, E, -E, E, 3.8)
    d = lat_d(x, y)
    forest = wt("Forest", x, y) + 0.6 * wt("Rock", x, y)
    clump = noise.fbm(x, y, 35.0, 3, key=21)
    edge = 6.0 + 2.5 * noise.fbm(x, y, 15.0, 2, key=22)
    keep = (forest > 0.55) & (d > np.maximum(edge, 5.0)) & (rng.random(len(x)) < np.clip(0.8 + 0.35 * clump, 0.12, 1.0))
    if creek is not None:
        _, cdist, pdist = creek.surface(x, y)
        keep &= (cdist > Creek.WC + 1.5) & (pdist > 1.5)
    x, y = x[keep], y[keep]

    def canopy(x, y, z, sink):
        r = rng.random(len(x))
        snag = r < 0.02
        a1 = (~snag) & (r < 0.58)
        a2 = (~snag) & ~a1
        emit("aspen_01", x[a1], y[a1], z[a1], rng.uniform(0, 360, a1.sum()), rng.uniform(1.0, 1.4, a1.sum()), sink=sink)
        emit("aspen_02", x[a2], y[a2], z[a2], rng.uniform(0, 360, a2.sum()), rng.uniform(1.1, 1.6, a2.sum()), sink=sink)
        k = rng.integers(0, 3, len(x))
        for kk, nm in enumerate(["snag_birch_a", "snag_birch_c", "snag_alder_b"]):
            s = snag & (k == kk)
            emit(nm, x[s], y[s], z[s], rng.uniform(0, 360, s.sum()), rng.uniform(1.1, 1.4, s.sum()), sink=sink)
    canopy(x, y, at(Hq, x, y), 0.15)
    # a sparser ring on the backdrop out to 420 m, so the main landscape's edge stays in the forest
    R = 420.0
    x, y = jitter_grid(-R, R, -R, R, 6.5)
    r_out = np.maximum(np.abs(x), np.abs(y))
    keep = (r_out > E) & (np.hypot(x, y) < R) & (np.abs(y - path_centre(np.clip(x, -100, 100))) > 9.0)
    x, y = x[keep], y[keep]
    canopy(x, y, far_z(x, y), 0.6)

    # young aspens and hazel: a dense shrub layer along the forest edge, thinner inside
    def shrubs(cell, kinds, scales, p_edge, p_deep, dmin, dmax):
        x, y = jitter_grid(-E, E, -E, E, cell)
        d = lat_d(x, y)
        k = rng.integers(0, len(kinds), len(x))
        sc = rng.uniform(scales[0], scales[1], len(x))
        foot = np.array([FOOT[kinds[i]] for i in k]) * sc
        p = np.where(d < 14, p_edge, p_deep)
        keep = (d - foot > verge + 0.3) & (d > dmin) & (d < dmax) & (rng.random(len(x)) < p)
        keep &= wt("Forest", x, y) + 0.5 * wt("Meadow", x, y) > 0.4
        if creek is not None:
            _, cdist, pdist = creek.surface(x, y)
            keep &= (cdist > Creek.WC + 0.6) & (pdist > 0.6)
        for kk, nm in enumerate(kinds):
            s = keep & (k == kk)
            emit(nm, x[s], y[s], at(Hq, x[s], y[s]), rng.uniform(0, 360, s.sum()), sc[s], sink=0.05)
    shrubs(3.0, ["hazel_01", "hazel_02"], (0.8, 1.25), 0.55, 0.3, 3.0, 200.0)
    shrubs(2.4, ["hazel_03", "hazel_04"], (0.8, 1.3), 0.45, 0.25, 2.0, 120.0)
    shrubs(5.0, ["aspen_03", "aspen_04"], (0.8, 1.3), 0.4, 0.15, 3.0, 200.0)

    # rocks: boulders on steep ground and in the forest, stones along the creek
    x, y = jitter_grid(-E, E, -E, E, 3.0)
    d = lat_d(x, y)
    p_rock = 0.02 + 0.5 * wt("Rock", x, y) + 0.03 * wt("Forest", x, y)
    sc = np.where(rng.random(len(x)) < 0.15, rng.uniform(1.8, 3.6, len(x)), rng.uniform(0.35, 1.4, len(x)))
    keep = (d - FOOT["rock"] * sc > verge + 0.3) & (rng.random(len(x)) < p_rock)
    x, y, sc = x[keep], y[keep], sc[keep]
    pt, rl = tilt(x, y, 1.0)
    emit("rock", x, y, at(Hq, x, y), rng.uniform(0, 360, len(x)), sc, pt + rng.uniform(-12, 12, len(x)),
         rl + rng.uniform(-12, 12, len(x)), sink=0.18 * sc)
    if creek is not None:
        sel = np.arange(0, len(creek.cx), 6)
        cx, cy = creek.cx[sel], creek.cy[sel]
        for side in (-1, 1):
            off = side * (Creek.WC + rng.uniform(-0.3, 0.5, len(cx)))
            t = np.gradient(np.stack([cx, cy], 1), axis=0)
            t /= np.linalg.norm(t, axis=1, keepdims=True)
            x = cx - t[:, 1] * off
            y = cy + t[:, 0] * off
            keep = (rng.random(len(x)) < 0.55) & (np.abs(y - creek.yc) > path_half + verge + 1.0)
            x, y = x[keep], y[keep]
            sc = rng.uniform(0.25, 0.9, len(x))
            emit("rock", x, y, at(Hq, x, y), rng.uniform(0, 360, len(x)), sc, rng.uniform(-20, 20, len(x)),
                 rng.uniform(-20, 20, len(x)), sink=0.2 * sc)

    # meadow grass and wildflowers. Nothing within the verge: an instance's footprint (radius
    # r_foot x scale) must clear path_half + verge.
    G = 140.0  # metres along X either side covered at full density (the rest gets a thinner pass)
    for (kind_list, cell, prob, rfoot) in (
            (["grass_meadow_a", "grass_meadow_b", "grass_meadow_c"], 0.22, 1.0, 0.3),
            (["flower_daisy", "flower_buttercup", "flower_knapweed"], 0.45, 0.8, 0.16)):
        x, y = jitter_grid(-E, E, -14, 14, cell)
        y = y + path_centre(x)
        d = lat_d(x, y)
        mw = wt("Meadow", x, y) + 0.5 * wt("Forest", x, y) * (d < 12)
        dens = np.clip(mw * (0.75 + 0.45 * noise.fbm(x, y, 3.0, 2, key=30 + len(kind_list))), 0, 1) * prob
        sc = rng.uniform(0.75, 1.25, len(x))
        keep = (d - rfoot * sc > verge + 0.02) & (rng.random(len(x)) < dens) & (wt("Creek", x, y) < 0.2)
        keep &= (np.abs(x) < G) | (rng.random(len(x)) < 0.6)
        x, y, sc = x[keep], y[keep], sc[keep]
        z = at(Hq, x, y)
        pt, rl = tilt(x, y, 0.6)
        k = rng.integers(0, len(kind_list), len(x))
        if "grass_meadow_b" in kind_list:
            # short grass at the front of the verge, the tall and seeding grasses behind it
            near = lat_d(x, y) < verge + 1.2 + 0.8 * rng.random(len(x))
            k = np.where(near & (rng.random(len(x)) < 0.75), 1, k)
        if "flower_daisy" in kind_list:
            k = np.where(noise.fbm(x, y, 9.0, 2, key=33) > 0.3, 0, np.where(noise.fbm(x, y, 7.0, 2, key=34) > 0.2, 1, k))
        for kk, nm in enumerate(kind_list):
            s = k == kk
            emit(nm, x[s], y[s], z[s], rng.uniform(0, 360, s.sum()), sc[s], pt[s], rl[s], sink=0.02)

    # forest floor: ferns and sparse grass within 45 m of the path
    x, y = jitter_grid(-E, E, -E, E, 1.3)
    d = lat_d(x, y)
    fw = wt("Forest", x, y)
    keep = (d > verge + 2.5) & (d < 140) & (rng.random(len(x)) < fw * np.clip(0.35 + 0.5 * noise.fbm(x, y, 8.0, 2, key=40), 0.05, 0.9))
    x, y = x[keep], y[keep]
    k = rng.random(len(x))
    z = at(Hq, x, y)
    pt, rl = tilt(x, y, 0.7)
    s = k < 0.55
    emit("fern", x[s], y[s], z[s], rng.uniform(0, 360, s.sum()), rng.uniform(0.7, 1.35, s.sum()), pt[s], rl[s], sink=0.03)
    s = ~s
    emit("grass_forest", x[s], y[s], z[s], rng.uniform(0, 360, s.sum()), rng.uniform(0.7, 1.2, s.sum()), pt[s], rl[s], sink=0.02)
    # reeds at the pond and creek edges
    if creek is not None:
        x, y = jitter_grid(creek.xc - 22, creek.xc + 22, creek.yc - 26, creek.yc + 20, 0.45)
        _, cdist, pdist = creek.surface(x, y)
        band = ((np.abs(pdist - 0.2) < 0.9) | (np.abs(cdist - Creek.WC - 0.3) < 0.6))
        keep = band & (lat_d(x, y) > verge + 1.0) & (rng.random(len(x)) < 0.5 * np.clip(0.5 + noise.fbm(x, y, 3.0, 2, key=50), 0, 1))
        x, y = x[keep], y[keep]
        emit("grass_reed", x, y, at(Hq, x, y), rng.uniform(0, 360, len(x)), rng.uniform(0.8, 1.3, len(x)), sink=0.05)

    # the clearance rule, checked on what was emitted
    worst, worst_kind = 99.0, None
    for kname, rows in kinds.items():
        r = np.array(rows)
        xm, ym = r[:, 0] / 100.0, -r[:, 1] / 100.0
        inside = np.abs(xm) < 203
        if not inside.any():
            continue
        foot = 0.0 if kname in ("aspen_01", "aspen_02") or kname.startswith("snag") else FOOT.get(kname, 0.0)
        c = float((lat_d(xm[inside], ym[inside]) - foot * r[inside, 6]).min())
        if c < worst:
            worst, worst_kind = c, kname
    return dict(kinds=kinds, min_clearance_from_path_edge_m=worst, min_clearance_kind=worst_kind)


if __name__ == "__main__":
    main()
