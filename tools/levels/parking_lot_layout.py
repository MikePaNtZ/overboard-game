"""Parking-lot training circuit (Level 1): the centreline and the element list.

Lot frame = MuJoCo world frame: x east, y north, z up, metres. The lap runs
counter-clockwise from the start line on the south straight.
"""
import math

LOT_HALF_X, LOT_HALF_Y = 62.0, 42.0       # painted lot (kerb ring at the edge)
GRID_HALF_X, GRID_HALF_Y = 65.0, 45.0     # heightfield half-extents (3 m margin)
SPACING_M = 0.05

# Turtle program: ("S", length) or ("A", radius, degrees, +left/-right).
AISLE_X = [28.0, 16.0, 4.0, -8.0]         # serpentine aisles, 12 m apart
HAIRPIN_R = 6.0
PROGRAM = [
    ("S", 86.0),                # south straight: start line, 3 speed bumps, rumble strip
    ("A", 10.0, 90.0),          # turn 1 -> heading north
    ("S", 44.0),                # east side: ramp A (5 % up, deck, 10 % down)
    ("A", 10.0, 90.0),          # turn 2 -> heading west on the north side
    ("S", 10.0),
    ("A", 6.0, 90.0),           # tight left into aisle 1 (heading south)
    ("S", 44.0),                # aisle 1: cone slalom
    ("A", HAIRPIN_R, -180.0),   # hairpin right
    ("S", 44.0),                # aisle 2: the plank
    ("A", HAIRPIN_R, 180.0),    # hairpin left
    ("S", 44.0),                # aisle 3: S-carves (painted line)
    ("A", HAIRPIN_R, -180.0),   # hairpin right
    ("S", 44.0),                # aisle 4: kerb up / island / kerb down
    ("A", 6.0, 90.0),           # tight left -> heading west on the north side
    ("S", 28.0),                # obstacles to steer around
    ("A", 10.0, 90.0),          # turn -> heading south on the west side
    ("S", 44.0),                # west side: ramp B (15 % up, deck, 20 % down)
    ("A", 10.0, 90.0),          # last turn -> back onto the south straight
]
START = (-42.0, -32.0, 0.0)     # x, y, heading (deg) at s = 0


def centreline(step=0.25):
    """Return [(x, y, heading_rad, s)] along the lap."""
    x, y, h = START[0], START[1], math.radians(START[2])
    out, s = [(x, y, h, 0.0)], 0.0
    for cmd in PROGRAM:
        if cmd[0] == "S":
            n = max(1, int(cmd[1] / step))
            for _ in range(n):
                d = cmd[1] / n
                x += d * math.cos(h); y += d * math.sin(h); s += d
                out.append((x, y, h, s))
        else:
            r, deg = cmd[1], cmd[2]
            arc = abs(math.radians(deg)) * r
            n = max(1, int(arc / step))
            dh = math.radians(deg) / n
            for _ in range(n):
                h += dh / 2
                x += (arc / n) * math.cos(h); y += (arc / n) * math.sin(h)
                h += dh / 2; s += arc / n
                out.append((x, y, h, s))
    return out


if __name__ == "__main__":
    pts = centreline()
    x, y, h, s = pts[-1]
    print(f"lap {s:.0f} m; end ({x:.2f}, {y:.2f}, {math.degrees(h)%360:.1f} deg); "
          f"start {START}")


# Elements, placed by (segment index in PROGRAM, offset along it in m).
# Heights and grades are the physical truth that MuJoCo receives.
ELEMENTS = [
    dict(kind="start_finish", seg=0, at=6.0, half_width=3.5),
    dict(kind="speed_bump", seg=0, at=20.0, height=0.075, length=0.9, half_width=3.5),
    dict(kind="speed_bump", seg=0, at=30.0, height=0.075, length=0.9, half_width=3.5),
    dict(kind="speed_bump", seg=0, at=40.0, height=0.075, length=0.9, half_width=3.5),
    dict(kind="rumble_strip", seg=0, at=52.0, length=12.0, height=0.015, pitch=0.60, half_width=3.5),
    # pitch >= 0.60 m: 12 posts per period, so the facets do not make a sawtooth (oracle)
    dict(kind="checkpoint", seg=0, at=74.0, half_width=4.0),                     # CP1
    dict(kind="ramp", seg=2, at=6.0, up_grade=0.05, up_len=10.0, deck_len=10.0,  # ramp A
         down_grade=0.10, half_width=2.5),          # rise 0.50 m, down 5.0 m; 30 deg side slopes + rail boxes
    dict(kind="checkpoint", seg=2, at=36.0, half_width=4.0),                     # CP2
    dict(kind="cone_slalom", seg=6, at=10.0, count=6, spacing=4.0, offset=1.0),
    dict(kind="checkpoint", seg=6, at=40.0, half_width=3.0),                     # CP3
    dict(kind="plank", seg=8, at=12.0, length=12.0, width=0.60, height=0.12, entry_len=1.2),
    dict(kind="checkpoint", seg=8, at=40.0, half_width=3.0),                     # CP4
    dict(kind="s_carve", seg=10, at=6.0, length=32.0, amplitude=1.5, wavelength=20.0),
    dict(kind="checkpoint", seg=10, at=40.0, half_width=3.0),                    # CP5
    dict(kind="kerb_island", seg=12, at=14.0, length=10.0, height=0.10, half_width=3.0,
         up_low_kerb=0.04, kerb_cut_len=1.5),  # c4: >= 8 cm up = nose strike at 2 m/s
    dict(kind="checkpoint", seg=12, at=40.0, half_width=3.0),                    # CP6
    dict(kind="obstacle", seg=14, at=6.0, offset=1.2, size=(0.6, 1.0, 1.0), name="cart"),
    dict(kind="obstacle", seg=14, at=13.0, offset=-1.2, size=(0.6, 0.6, 1.0), name="bin"),
    dict(kind="obstacle", seg=14, at=20.0, offset=1.2, size=(1.2, 1.0, 0.15), name="pallet"),
    dict(kind="checkpoint", seg=14, at=26.0, half_width=4.0),                    # CP7
    dict(kind="ramp", seg=16, at=8.0, up_grade=0.15, up_len=4.0, deck_len=8.0,   # ramp B
         down_grade=0.20, half_width=2.5),          # rise 0.60 m, down 3.0 m
    dict(kind="checkpoint", seg=16, at=36.0, half_width=4.0),                    # CP8
]


def seg_starts():
    """Return the lap distance s at the start of each PROGRAM segment."""
    s, out = 0.0, []
    for cmd in PROGRAM:
        out.append(s)
        s += cmd[1] if cmd[0] == "S" else abs(math.radians(cmd[2])) * cmd[1]
    return out


def pose_at(s, pts=None):
    """Return (x, y, heading) on the centreline at lap distance s."""
    pts = pts or centreline()
    for a, b in zip(pts, pts[1:]):
        if b[3] >= s:
            t = (s - a[3]) / max(b[3] - a[3], 1e-9)
            return (a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]), b[2])
    return pts[-1][:3]
