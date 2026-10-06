"""Export a level layout to the files that sim-host loads, plus level.json for the game.

    python tools/levels/export_level.py parking_lot [OUT_DIR]

Outputs (OUT_DIR, default ~/projects/overboard-viz/out/carve-lab/data/courses/<level>):
  course_hfield.bin  i32 nrow, i32 ncol, f32 heights row-major (row = +Y, col = +X)
  metadata.json      grid, spawn, bounds (sim-host --terrain)
  obstacles.csv      fixed boxes and cones (sim-host --obstacles)
  level.json         grid spec, checkpoints, demo_path, element list (game side)
  course_height.npy  the same heights as the .bin, for the Unreal ground build

No board physics here. This file only describes the world; sim-host computes the ride.
Rule: surfaces with a slope under about 30 deg are in the heightfield. Steps, and every
object to steer around, are boxes (the tyre contacts boxes directly, c4 cecbd1d).
"""
import json
import math
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

SIDE_SLOPE = math.tan(math.radians(30.0))   # ramp deck side slopes
CONE_SIZE = (0.30, 0.30, 0.45)              # as sim/carve/obstacles.py


def heading_to_yaw_deg(heading_rad):
    """sim-host spawn yaw: yaw 0 = board forward along world -X (c4 cecbd1d)."""
    y = (math.degrees(heading_rad) + 180.0) % 360.0
    return y - 360.0 if y > 180.0 else y


class Level:
    def __init__(self, L):
        self.L = L
        self.pts = L.centreline(step=0.05)
        self.s_arr = np.array([p[3] for p in self.pts])
        self.seg_s = L.seg_starts()
        hx, hy, dx = L.GRID_HALF_X, L.GRID_HALF_Y, L.SPACING_M
        self.ncol = round(2 * hx / dx) + 1
        self.nrow = round(2 * hy / dx) + 1
        assert self.ncol % 2 == 1 and self.nrow % 2 == 1
        self.xs = -hx + dx * np.arange(self.ncol)
        self.ys = -hy + dx * np.arange(self.nrow)
        self.X, self.Y = np.meshgrid(self.xs, self.ys)     # [row, col]
        self.Z = np.zeros_like(self.X, dtype=np.float64)
        self.boxes = []          # (type, id, x, y, lx, ly, lz, yaw, pitch, roll, z_m)
        self.checkpoints = []
        self.offsets = []        # (s0, s1, fn(s) -> lateral offset) for the demo path
        self.speed_caps = []     # (s0, s1, v_max)

    # --- geometry helpers ---------------------------------------------------------
    def pose(self, s):
        s = s % self.s_arr[-1]
        i = int(np.clip(np.searchsorted(self.s_arr, s), 1, len(self.pts) - 1))
        a, b = self.pts[i - 1], self.pts[i]
        t = (s - a[3]) / max(b[3] - a[3], 1e-9)
        return a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), b[2]

    def s_of(self, e, extra=0.0):
        return self.seg_s[e["seg"]] + e["at"] + extra

    def local_uv(self, s0):
        """Grid coordinates in a straight segment's frame: u along, v left (+normal)."""
        x0, y0, h = self.pose(s0)
        c, sn = math.cos(h), math.sin(h)
        u = (self.X - x0) * c + (self.Y - y0) * sn
        v = -(self.X - x0) * sn + (self.Y - y0) * c
        return u, v, h

    def add_box(self, typ, bid, x, y, lx, ly, lz, yaw_rad=0.0, z_m=None):
        self.boxes.append((typ, bid, x, y, lx, ly, lz, math.degrees(yaw_rad), z_m))

    def box_along(self, bid, s_mid, lat, length, width, height, z_m=None, typ="box"):
        x, y, h = self.pose(s_mid)
        n = (-math.sin(h), math.cos(h))
        self.add_box(typ, bid, x + n[0] * lat, y + n[1] * lat, length, width, height, h, z_m)

    # --- elements ------------------------------------------------------------------
    def speed_bump(self, e, k):
        u, v, _ = self.local_uv(self.s_of(e))
        L, hw = e["length"], e["half_width"]
        prof = np.where(np.abs(u) < L / 2, 0.5 * (1 + np.cos(2 * np.pi * u / L)), 0.0)
        taper = np.clip((hw + 0.3 - np.abs(v)) / 0.3, 0.0, 1.0)
        self.Z = np.maximum(self.Z, e["height"] * prof * taper)
        self.speed_caps.append((self.s_of(e, -3), self.s_of(e, 2), 3.0))

    def rumble_strip(self, e, k):
        u, v, _ = self.local_uv(self.s_of(e))
        inside = (u >= 0) & (u <= e["length"]) & (np.abs(v) <= e["half_width"])
        ridge = 0.5 * (1 - np.cos(2 * np.pi * u / e["pitch"])) * e["height"]
        self.Z = np.maximum(self.Z, np.where(inside, ridge, 0.0))

    def ramp(self, e, k):
        s0 = self.s_of(e)
        u, v, h = self.local_uv(s0)
        ul, dl = e["up_len"], e["deck_len"]
        rise = e["up_grade"] * ul
        down = rise / e["down_grade"]
        z_axis = np.select(
            [u < 0, u < ul, u < ul + dl, u < ul + dl + down],
            [0.0, u * e["up_grade"], rise, rise - (u - ul - dl) * e["down_grade"]], 0.0)
        side = np.maximum(np.abs(v) - e["half_width"], 0.0)
        self.Z = np.maximum(self.Z, np.maximum(z_axis - side * SIDE_SLOPE, 0.0))
        for sgn, nm in ((1, "L"), (-1, "R")):     # rails on the flat deck edges
            self.box_along(f"ramp{k}_rail{nm}", s0 + ul + dl / 2,
                           sgn * (e["half_width"] - 0.05), dl, 0.10, 0.90, z_m=rise)
        name = "A" if e["seg"] == 2 else "B"
        if name == "B":   # oracle: enter at >= 3 m/s, go down at 1.5 m/s
            self.speed_caps.append((s0 - 8, s0 + ul, 3.5))
            self.speed_caps.append((s0 + ul + dl - 2, s0 + ul + dl + down + 2, 1.5))
        else:
            self.speed_caps.append((s0 + ul + dl - 2, s0 + ul + dl + down + 2, 2.5))

    def cone_slalom(self, e, k):
        s0, sp, A = self.s_of(e), e["spacing"], e["weave"]
        for j in range(e["count"]):
            self.box_along(f"cone{j}", s0 + j * sp, 0.0, *CONE_SIZE, typ="cone")
        s1 = s0 + (e["count"] - 1) * sp
        self.offsets.append((s0 - sp, s1 + sp,
                             lambda s, s0=s0, sp=sp, A=A: A * math.cos(math.pi * (s - s0) / sp)))
        self.speed_caps.append((s0 - sp, s1 + sp, math.sqrt(2.0 / (A * (math.pi / sp) ** 2))))

    def plank(self, e, k):
        s0 = self.s_of(e)
        u, v, _ = self.local_uv(s0)
        el, H, w = e["entry_len"], e["height"], e["width"]
        wedge = np.where((u > -el) & (u <= 0.0) & (np.abs(v) <= w / 2), (u + el) / el * H, 0.0)
        self.Z = np.maximum(self.Z, wedge)
        self.box_along("plank", s0 + e["length"] / 2, 0.0, e["length"], w, H)
        self.speed_caps.append((s0 - el - 4, s0 + e["length"] + 1, 2.3))

    def s_carve(self, e, k):
        s0, A, lam = self.s_of(e), e["amplitude"], e["wavelength"]
        s1 = s0 + e["length"]
        self.offsets.append((s0, s1, lambda s, s0=s0, A=A, lam=lam:
                             A * math.sin(2 * math.pi * (s - s0) / lam)))
        self.speed_caps.append((s0, s1, math.sqrt(2.0 / (A * (2 * math.pi / lam) ** 2))))

    def kerb_island(self, e, k):
        s0, Ln, H, hw = self.s_of(e), e["length"], e["height"], e["half_width"]
        self.box_along("island", s0 + Ln / 2, 0.0, Ln, 2 * hw, H)
        # Kerb cut: a heightfield wedge up to the island top, on the left of the lane.
        u, v, _ = self.local_uv(s0)
        cl, co, cw = e["cut_len"], e["cut_offset"], e["cut_width"]
        wedge = np.where((u > -cl) & (u <= 0.02) & (np.abs(v - co) <= cw / 2),
                         np.clip((u + cl) / cl, 0, 1) * H, 0.0)
        self.Z = np.maximum(self.Z, wedge)
        self.offsets.append((s0 - 10, s0 + Ln + 4, lambda s, a=s0 - 10, b=s0 + Ln + 4, co=co:
                             co * _bump(s, a, b, 6.0)))
        self.speed_caps.append((s0 - 4, s0 + Ln + 2, 2.5))

    def obstacle(self, e, k):
        lx, ly, lz = e["size"]
        self.box_along(e["name"], self.s_of(e), e["offset"], lx, ly, lz)
        s = self.s_of(e)
        self.offsets.append((s - 6, s + 6, lambda q, a=s - 6, b=s + 6, o=-e["offset"]:
                             o * _bump(q, a, b, 4.0)))

    def checkpoint(self, e, k, kind="checkpoint"):
        x, y, h = self.pose(self.s_of(e))
        self.checkpoints.append(dict(type=kind, id=f"cp{len(self.checkpoints)}" if kind == "checkpoint" else "start_finish",
                                     order=len(self.checkpoints), x=round(x, 3), y=round(y, 3),
                                     heading_deg=round(math.degrees(h), 2),
                                     half_width=e["half_width"], s=round(self.s_of(e), 2)))

    def start_finish(self, e, k):
        self.checkpoint(e, k, kind="start_finish")

    # --- lot -----------------------------------------------------------------------
    def lot(self):
        L = self.L
        hx, hy = L.LOT_HALF_X, L.LOT_HALF_Y
        for nm, x, y, lx, ly in (("kerbN", 0, hy, 2 * hx, 0.3), ("kerbS", 0, -hy, 2 * hx, 0.3),
                                 ("kerbE", hx, 0, 0.3, 2 * hy), ("kerbW", -hx, 0, 0.3, 2 * hy)):
            self.add_box("box", nm, x, y, lx, ly, 0.15)
        # Axis marker: one tall light pole in the NE corner only (proves x, y and yaw sign).
        self.add_box("box", "pole_NE", hx - 2.0, hy - 2.0, 0.3, 0.3, 6.0)

    # --- demo path -----------------------------------------------------------------
    def demo_path(self, step=1.0, v_cruise=4.5, a_max=0.8):
        n = int(self.s_arr[-1] / step)
        out = []
        for i in range(n):
            s = i * step
            x, y, h = self.pose(s)
            off = 0.0
            for s0, s1, fn in self.offsets:
                if s0 <= s <= s1:
                    off += fn(s)
            out.append([x - math.sin(h) * off, y + math.cos(h) * off])
        P = np.array(out)
        # Curvature cap of the rider layer: kappa <= min(0.25, 2/v^2) -> v <= sqrt(2/kappa).
        d1 = np.roll(P, -1, 0) - np.roll(P, 1, 0)
        d2 = np.roll(P, -1, 0) - 2 * P + np.roll(P, 1, 0)
        kap = np.abs(d1[:, 0] * d2[:, 1] - d1[:, 1] * d2[:, 0]) / (np.hypot(*d1.T) ** 3 + 1e-9) * 4
        kap = np.maximum(kap, np.convolve(np.r_[kap[-3:], kap, kap[:3]], np.ones(7) / 7, "valid"))
        v = np.minimum(v_cruise, np.sqrt(2.0 / np.maximum(kap, 1e-6)) * 0.85)
        for s0, s1, cap in self.speed_caps:
            for i in range(n):
                if s0 <= i * step <= s1:
                    v[i] = min(v[i], cap)
        for _ in range(3):   # acceleration limit, both directions, around the lap
            for i in range(1, 2 * n):
                v[i % n] = min(v[i % n], math.sqrt(v[(i - 1) % n] ** 2 + 2 * a_max * step))
            for i in range(2 * n, 0, -1):
                v[(i - 1) % n] = min(v[(i - 1) % n], math.sqrt(v[i % n] ** 2 + 2 * a_max * step))
        return [[round(p[0], 3), round(p[1], 3), round(float(vi), 2)] for p, vi in zip(P, v)], kap

    # --- export --------------------------------------------------------------------
    def build(self):
        self.lot()
        for k, e in enumerate(self.L.ELEMENTS):
            getattr(self, e["kind"])(e, k)

    def write(self, out_dir, name):
        out = Path(out_dir)
        out.mkdir(parents=True, exist_ok=True)
        Z = self.Z.astype(np.float32)
        with open(out / "course_hfield.bin", "wb") as f:
            f.write(struct.pack("<ii", self.nrow, self.ncol))
            f.write(Z.tobytes(order="C"))
        np.save(out / "course_height.npy", Z)
        L = self.L
        x0, y0, h0 = L.START[0], L.START[1], math.radians(L.START[2])
        m = 2.0   # bounds brake starts 2 m inside the kerb ring
        z_min, z_max = float(Z.min()), float(Z.max())
        meta = {
            "nrow": self.nrow, "ncol": self.ncol,
            "half_extent_m": L.GRID_HALF_X,
            "half_extent_x_m": L.GRID_HALF_X, "half_extent_y_m": L.GRID_HALF_Y,
            "spacing_m": L.SPACING_M,
            "z_min_m": z_min, "z_max_m": max(z_max, z_min + 0.01),
            "row_col_convention": "height[row, col]; row increases with MuJoCo +Y, col with +X",
            "spawn": {"x": x0, "y": y0, "yaw_deg": heading_to_yaw_deg(h0)},
            "bounds": {"xmin": -L.LOT_HALF_X + m, "xmax": L.LOT_HALF_X - m,
                       "ymin": -L.LOT_HALF_Y + m, "ymax": L.LOT_HALF_Y - m},
            "source": f"overboard-game tools/levels/export_level.py ({name}, authored, not measured)",
        }
        (out / "metadata.json").write_text(json.dumps(meta, indent=1))
        with open(out / "obstacles.csv", "w") as f:
            f.write("# type,id,x_m,y_m,lx_m,ly_m,lz_m,yaw_deg,pitch_deg,roll_deg,z_m\n")
            f.write(f"# {name}: written by overboard-game tools/levels/export_level.py\n")
            for typ, bid, x, y, lx, ly, lz, yaw, zm in self.boxes:
                zs = "" if zm is None else f"{zm:.4f}"
                f.write(f"{typ},{bid},{x:.4f},{y:.4f},{lx:.4f},{ly:.4f},{lz:.4f},{yaw:.3f},0,0,{zs}\n")
        path, _ = self.demo_path()
        level = {
            "level": name, "frame": "MuJoCo world: x east, y north, z up, metres",
            "grid": {"nrow": self.nrow, "ncol": self.ncol, "spacing_m": L.SPACING_M,
                     "x0_m": float(self.xs[0]), "y0_m": float(self.ys[0]),
                     "half_extent_x_m": L.GRID_HALF_X, "half_extent_y_m": L.GRID_HALF_Y},
            "spawn": meta["spawn"], "lap_length_m": round(float(self.s_arr[-1]), 2),
            "checkpoints": self.checkpoints,
            "demo_path": path,
            "elements": L.ELEMENTS,
        }
        (out / "level.json").write_text(json.dumps(level, indent=1, default=list))
        return meta, path


def _bump(s, a, b, ramp):
    """1 inside [a + ramp, b - ramp], smooth 0 -> 1 -> 0 at the ends."""
    t = min(max((s - a) / ramp, 0.0), 1.0, max((b - s) / ramp, 0.0))
    return t * t * (3 - 2 * t)


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "parking_lot"
    out_dir = sys.argv[2] if len(sys.argv) > 2 else str(
        Path.home() / "projects/overboard-viz/out/carve-lab/data/courses" / name)
    L = __import__(f"{name}_layout")
    lv = Level(L)
    lv.build()
    meta, path = lv.write(out_dir, name)
    t = sum(math.dist(path[i][:2], path[(i + 1) % len(path)][:2]) /
            max(0.5 * (path[i][2] + path[(i + 1) % len(path)][2]), 0.1) for i in range(len(path)))
    print(f"{name}: {lv.nrow}x{lv.ncol} posts, z {meta['z_min_m']:.3f}..{meta['z_max_m']:.3f} m, "
          f"{len(lv.boxes)} boxes, {len(lv.checkpoints)} lines, lap {lv.s_arr[-1]:.0f} m, "
          f"demo lap {t:.0f} s")
    print(f"--terrain {Path(out_dir) / 'course_hfield.bin'} --obstacles {Path(out_dir) / 'obstacles.csv'}")


if __name__ == "__main__":
    main()
