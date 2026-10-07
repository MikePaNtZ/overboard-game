#!/usr/bin/env python3
"""Generate the OB_Embarcadero level inputs from the embarcadero course. Runs OUTSIDE Unreal.

    ~/.venvs/ob-levels/bin/python tools/levels/embarcadero/gen_embarcadero.py <course_dir> <out_dir>

The world is DRAWN only from the exported data (HARD RULE: Unreal computes no board physics):
  - the ground mesh is course_height.npy downsampled to 1 m, 5 cm below the exact surface;
  - the ridden corridor (within 12 m of the demo path) is an exact mesh at course_height + 4 mm;
  - every obstacles.csv kerb and rail box is drawn at its exact pose, size and z (z_m = box bottom);
  - the water plane covers the bay (heights -1.1 m);
  - the buildings are the OSM footprints, extruded and grounded;
  - the markings (lane lines, green bike lanes, zebra crosswalks) and the dressing (lamps, trees,
    palms, benches) come from the OSM streets and the route.
tools/levels/embarcadero/verify_embarcadero.py checks the corridor mesh vs course_height.

Frame: MuJoCo world = UTM 10N shifted to the grid centre; ABoardActor maps (x, y, z) m to UE
(100x, -100y, 100z) cm. The PlayerStart sits at the world origin, yaw 0.
Map data (c) OpenStreetMap contributors, ODbL 1.0. Elevation USGS 3DEP 1 m (public domain).
"""
import json
import math
import os
import sys

import numpy as np
import shapely
from scipy import ndimage
from shapely.geometry import LineString, Point, Polygon
from shapely.ops import triangulate, unary_union

BUILDING_H = 3.0         # course_height raises building footprints by this; undo it in the ground mesh

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "city"))
sys.path.insert(0, os.path.join(HERE, "..", "..", "trail"))
sys.path.insert(0, HERE)
import obm  # noqa: E402
import gen_city as gc  # noqa: E402  (import-safe: its main is guarded by __name__)
import osm_stage as osm_mod  # noqa: E402

GROUND_DX = 2.0          # ground mesh spacing (m); a non-ridden backdrop below the exact surface,
                         # coarsened from 1 m to hold the git Content budget (named in the report)
GROUND_DROP = 0.05       # the ground mesh sits 5 cm below the exact surface
GROUND_TILE = 100.0      # ground tiles ~100 m
COR_DS = 0.25            # corridor longitudinal step (m)
COR_HALF = 12.0          # corridor half width (m) each side of the path
COR_DV = 0.5             # corridor lateral step (m); fine enough for the kerb-cut ramps
COR_LIFT = 0.004         # corridor sits 4 mm above the exact surface
COR_TILE_M = 40.0        # corridor tiles ~40 m of path
MARK_LIFT = 0.02         # markings 2 cm above the ground (the kerb step is 15 cm)
WATER_Z = -0.4           # the water plane height (m)
PALM_SPACING = 18.0
LAMP_SPACING = 40.0


def log(*a):
    print(*a, flush=True)


def load(cdir):
    meta = json.load(open(os.path.join(cdir, "metadata.json")))
    lvl = json.load(open(os.path.join(cdir, "level.json")))
    h = np.load(os.path.join(cdir, "course_height.npy"))
    rows = []
    with open(os.path.join(cdir, "obstacles.csv")) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            rows.append(line.strip().split(","))
    return meta, lvl, h, rows


def make_sampler(h, g):
    x0, y0, sp = g["x0_m"], g["y0_m"], g["spacing_m"]

    def z(x, y):
        xa, ya = np.atleast_1d(np.asarray(x, float)), np.atleast_1d(np.asarray(y, float))
        r = ndimage.map_coordinates(h, [(ya - y0) / sp, (xa - x0) / sp], order=1, mode="nearest")
        return r.reshape(np.shape(x)) if np.ndim(x) else float(r[0])
    return z


# --- ground: 1 m mesh of the whole grid, below the exact surface ---------------------------------
def build_ground(zfn, out, hx, hy, bgeom):
    """A coarse ground mesh below the exact surface. Inside building footprints the course height is
    3 m higher (export raises the blocks); subtract it so the ground stays flat and the drawn
    buildings sit on it, instead of 3 m plateaus with pyramid-edged steps."""
    if bgeom is not None:
        shapely.prepare(bgeom)
    n = 0
    for xi in np.arange(-hx, hx, GROUND_TILE):
        for yi in np.arange(-hy, hy, GROUND_TILE):
            x1, y1 = min(xi + GROUND_TILE, hx), min(yi + GROUND_TILE, hy)
            xs = np.arange(xi, x1 + GROUND_DX * 0.5, GROUND_DX)
            ys = np.arange(yi, y1 + GROUND_DX * 0.5, GROUND_DX)
            X, Y = np.meshgrid(xs, ys, indexing="ij")
            Z = zfn(X, Y) - GROUND_DROP
            if bgeom is not None:
                inb = shapely.contains_xy(bgeom, X.ravel(), Y.ravel()).reshape(X.shape)
                Z = np.where(inb, Z - BUILDING_H, Z)
            m = obm.Mesh()
            gc.add_grid(m, X, Y, Z, "Ground", X, Y)
            gc.ue_winding(m).write(out("ground_%d.obm" % n))
            n += 1
    log("ground tiles:", n)
    return n


# --- corridor: an exact ribbon along the demo path -----------------------------------------------
def build_corridor(zfn, path, out, bgeom, prom_geom):
    from scipy.spatial import cKDTree
    P = np.array([(p[0], p[1]) for p in path])
    # resample the closed path at COR_DS
    seg = np.r_[0, np.cumsum(np.hypot(*np.diff(np.vstack([P, P[0]]), axis=0).T))]
    total = seg[-1]
    ss = np.arange(0, total, COR_DS)
    xs = np.interp(ss, seg, np.r_[P[:, 0], P[0, 0]])
    ys = np.interp(ss, seg, np.r_[P[:, 1], P[0, 1]])
    # heading and curvature per point
    dx = np.gradient(xs); dy = np.gradient(ys)
    ddx = np.gradient(dx); ddy = np.gradient(dy)
    hd = np.arctan2(dy, dx)
    nx, ny = -np.sin(hd), np.cos(hd)
    kap = (dx * ddy - dy * ddx) / (np.hypot(dx, dy) ** 3 + 1e-9)   # signed curvature (+ = turns left)
    rad = 0.85 / np.maximum(np.abs(kap), 1e-6)                     # clamp the inner side to this
    # self-proximity: where the route passes near another part of itself (the racket loop), shrink
    # the ribbon so the two strips do not overlap and z-fight (the promenade ghost triangles).
    tree = cKDTree(np.column_stack([xs, ys]))
    hw = np.full(len(ss), COR_HALF)
    for i in range(len(ss)):
        for j in tree.query_ball_point([xs[i], ys[i]], 2 * COR_HALF):
            ad = abs(ss[i] - ss[j]); ad = min(ad, total - ad)
            if ad > 30.0:
                hw[i] = min(hw[i], 0.46 * math.hypot(xs[i] - xs[j], ys[i] - ys[j]))
    hw = np.minimum(hw, rad)
    vs = np.arange(-COR_HALF, COR_HALF + COR_DV * 0.5, COR_DV)
    V = np.clip(np.tile(vs[None, :], (len(ss), 1)).astype(float), -hw[:, None], hw[:, None])
    shapely.prepare(bgeom); shapely.prepare(prom_geom)
    ntile = 0
    per = max(1, int(COR_TILE_M / COR_DS))
    for i0 in range(0, len(ss), per):
        i1 = min(i0 + per + 1, len(ss))
        if i1 - i0 < 2:
            break
        X = xs[i0:i1, None] + nx[i0:i1, None] * V[i0:i1, :]
        Y = ys[i0:i1, None] + ny[i0:i1, None] * V[i0:i1, :]
        Z = zfn(X, Y) + COR_LIFT
        # do not draw raised building blocks: lower corridor posts inside a footprint by 3 m
        inb = shapely.contains_xy(bgeom, X.ravel(), Y.ravel()).reshape(X.shape)
        Z = np.where(inb, Z - BUILDING_H, Z)
        mid = (xs[(i0 + i1) // 2], ys[(i0 + i1) // 2])
        section = "Pave" if prom_geom.contains(Point(*mid)) else "Road"
        m = obm.Mesh()
        gc.add_grid(m, X, Y, Z, section, X, Y)
        # the ridden surface is near-flat; drop any triangle with a large vertical extent. These are
        # the building-block lowering cliffs and the pinch spikes where the ribbon narrows -- not
        # ground the rider ever touches (the demo path is checked by verify, and is not affected).
        pp = m.parts[0]
        zc = pp[0][:, 2]
        tri = pp[5]
        keep = (zc[tri].max(1) - zc[tri].min(1)) < 60.0     # cm
        m.parts[0] = pp[:5] + (tri[keep], pp[6])
        gc.ue_winding(m).write(out("corridor_%d.obm" % ntile))
        ntile += 1
    log("corridor tiles:", ntile)
    return ntile


# --- kerbs and rails from obstacles.csv ----------------------------------------------------------
def build_boxes(zfn, rows, out):
    kerbs = obm.Mesh()
    rails = obm.Mesh()
    nk = nr = 0
    for r in rows:
        typ, bid = r[0], r[1]
        x, y, lx, ly, lz = float(r[2]), float(r[3]), float(r[4]), float(r[5]), float(r[6])
        yaw = float(r[7])
        zm = r[10].strip()
        if bid.startswith("kerb"):
            cz = (float(zm) if zm else zfn(x, y)) + lz / 2.0   # z_m = box bottom
            gc.box_mesh(kerbs, (x, y, cz), (lx, ly, lz), "Kerb", yaw_deg=-yaw)
            nk += 1
        else:  # rail
            cz = (float(zm) if zm else zfn(x, y)) + lz / 2.0
            gc.box_mesh(rails, (x, y, cz), (lx, ly, lz), "Rail", yaw_deg=-yaw)
            nr += 1
    gc.ue_winding(kerbs).write(out("kerbs.obm"))
    gc.ue_winding(rails).write(out("rails.obm"))
    log("kerbs:", nk, "rails:", nr)


# --- water plane over the bay --------------------------------------------------------------------
def build_water(h, g, out):
    # the bay is where the heights are at the water floor (-1.1 m); find its bounding box
    x0, y0, sp = g["x0_m"], g["y0_m"], g["spacing_m"]
    water = h <= -1.0
    if not water.any():
        json.dump({}, open(out("water.json"), "w"))
        return
    rr, cc = np.where(water)
    xlo, xhi = x0 + cc.min() * sp, x0 + cc.max() * sp
    ylo, yhi = y0 + rr.min() * sp, y0 + rr.max() * sp
    json.dump({"xlo": float(xlo), "xhi": float(xhi), "ylo": float(ylo), "yhi": float(yhi),
               "z": WATER_Z}, open(out("water.json"), "w"))
    log("water bbox x[%.0f,%.0f] y[%.0f,%.0f]" % (xlo, xhi, ylo, yhi))


# A warm plaster / brick / stone palette for the building facades (sRGB 0..255). Mid tones, so
# they do not blow out to white under the daylight exposure.
FACADE_COLS = [(150, 120, 96), (122, 92, 78), (150, 146, 138), (120, 104, 84), (164, 140, 110),
               (112, 78, 62), (134, 128, 120), (104, 96, 86), (146, 116, 96), (118, 126, 132)]


def build_buildings(osm, zfn, out, hx, hy):
    mesh = obm.Mesh()
    n = 0
    foots = []
    grid = Polygon([(-hx, -hy), (hx, -hy), (hx, hy), (-hx, hy)])
    for wi, (t, nd) in enumerate(osm.ways):
        if "building" not in t or nd[0] != nd[-1] or len(nd) < 4:
            continue
        try:
            poly = Polygon(osm.line(nd))
            if not poly.is_valid:
                poly = poly.buffer(0)
            poly = poly.intersection(grid)
        except Exception:
            continue
        if poly.is_empty or poly.area < 10.0:
            continue
        polys = poly.geoms if poly.geom_type == "MultiPolygon" else [poly]
        for pg in polys:
            if pg.area < 10.0 or pg.geom_type != "Polygon":
                continue
            ht = building_height(t)
            cx, cy = pg.centroid.x, pg.centroid.y
            gz = float(zfn(cx, cy))
            base = gz - 7.0           # bury the foot so no sawtooth shows
            top = gz - 3.0 + ht       # roof `ht` above the street (gz minus the 3 m block raise)
            pe = pg.buffer(0.5, join_style=2, mitre_limit=3.0)   # 0.5 m proud: hides the block edge,
            #                                                      stays behind the 1.0 m kit veneer
            if pe.geom_type != "Polygon":
                pe = pg
            col = FACADE_COLS[(wi * 7 + int(abs(cx) + abs(cy))) % len(FACADE_COLS)]
            extrude_polygon(mesh, pe, base, top, gz, col)
            foots.append([[round(x, 2), round(y, 2)] for x, y in pg.exterior.coords])
            n += 1
    gc.ue_winding(mesh).write(out("buildings.obm"), colors=True)
    json.dump({"footprints": foots}, open(out("buildings.json"), "w"))
    log("buildings:", n)


# --- real City Sample facades on every street-facing edge -----------------------------------------
# We dress EVERY building edge that faces open space (a street, plaza or the bay) with real kit
# facades, cut from the source levels SFA_Ref_N1 / SFB_Ref_N1 and the SFC kit. Each bay is a stack
# of WHOLE floor bands (ground + N repeat floors + cornice) chosen from the building height -- no
# vertical stretch, so the windows keep true proportions. Three styles vary by building. The
# procedural building body + roof stay behind (build_buildings), so the raised MuJoCo block stays
# covered; a plain plaster material (no stripes) dresses the body and the hidden back/side faces.
FAC_PROUD_M = 1.00               # the kit street face sits this far proud of the OSM edge. RULE: no
                                 # building geometry past footprint + 1.2 m, so the veneer face is at
                                 # 1.0 m and the procedural body buffer (0.5 m) stays behind it, not
                                 # occluding it. Nothing sits within 1.5 m of the demo path.
FAC_BOX_OFF = 0.80               # blank/corner/plinth boxes are 0.4 m deep; offset their centre so
                                 # the outer face lands at FAC_PROUD_M (0.8 + 0.2 = 1.0 m proud)
_A = "/Game/Building/SF/A/Kit_Bldg_SFA_%s_A/Mesh/SM_BLDG_SFA_%s_A_Wall_01_N1"
_B = "/Game/Building/SF/B/Kit_Bldg_SFB_%s_A/Mesh/SM_BLDG_SFB_%s_A_Wall_03_N1"
_C = "/Game/Building/SF/C/Kit_Bldg_SFC_%s_A/Mesh/SM_BLDG_SFC_%s_A1_Wall_0%d_N1"
# style -> bay width m, wall depth m, (ground mesh, h), [floor (mesh, h)...], (cornice mesh, h)
FAC_STYLES = {
    "A": dict(bay=3.25, depth=1.30, ground=(_A % ("L2", "L2"), 7.5),
              floors=[(_A % ("L4", "L4"), 3.75), (_A % ("L5", "L5"), 3.75), (_A % ("L6", "L6"), 3.75)],
              cornice=(_A % ("L9", "L9"), 2.14)),
    "B": dict(bay=4.50, depth=1.05, ground=(_B % ("L2", "L2"), 6.75),
              floors=[(_B % ("L3", "L3"), 3.75)], cornice=(_B % ("L4", "L4"), 4.75)),
    "C": dict(bay=1.50, depth=0.50, ground=(_C % ("L1", "L1", 2), 5.0),
              floors=[(_C % ("L2", "L2", 1), 4.0), (_C % ("L3", "L3", 1), 4.0)],
              cornice=(_C % ("L4", "L4", 1), 1.25)),
}
FAC_STYLE_KEYS = ["A", "B", "C"]


def facade_stack(style, h_m):
    """The whole-floor piece stack for a building of height h_m: ground + N repeat floors + cornice,
    with no vertical scale. Returns ([(mesh, dz_m)...], total_height_m)."""
    s = FAC_STYLES[style]
    gm, gh = s["ground"]
    cm, ch = s["cornice"]
    fl = s["floors"]
    n = max(0, int(round((h_m - gh - ch) / fl[0][1])))
    pieces = [(gm, 0.0)]
    dz = gh
    for i in range(n):
        m, fh = fl[i % len(fl)]
        pieces.append((m, dz))
        dz += fh
    pieces.append((cm, dz))
    return pieces, dz + ch


def build_facades(osm, zfn, out, hx, hy, bgeom):
    """Place kit facade modules on EVERY street-facing edge in the grid. Writes facades.json in
    MuJoCo metres; build_embarcadero.py converts to UE and instances the kit meshes."""
    from shapely.geometry.polygon import orient
    shapely.prepare(bgeom)
    grid = Polygon([(-hx, -hy), (hx, -hy), (hx, hy), (-hx, hy)])
    meshes, midx = [], {}
    pieces, blanks, plinths, corners = [], [], [], []

    def mesh_id(p):
        if p not in midx:
            midx[p] = len(meshes)
            meshes.append(p)
        return midx[p]

    wi = -1
    for t, nd in osm.ways:
        if "building" not in t or nd[0] != nd[-1] or len(nd) < 4:
            continue
        wi += 1
        try:
            poly = Polygon(osm.line(nd))
            if not poly.is_valid:
                poly = poly.buffer(0)
            poly = poly.intersection(grid)
        except Exception:
            continue
        if poly.is_empty or poly.area < 10.0:
            continue
        polys = poly.geoms if poly.geom_type == "MultiPolygon" else [poly]
        for pg in polys:
            if pg.area < 30.0 or pg.geom_type != "Polygon":
                continue
            style = FAC_STYLE_KEYS[wi % len(FAC_STYLE_KEYS)]
            bay = FAC_STYLES[style]["bay"]
            depth = FAC_STYLES[style]["depth"]
            h_m = building_height(t)
            stack, total_h = facade_stack(style, h_m)
            ring = list(orient(pg, sign=1.0).exterior.coords)[:-1]     # CCW
            for a, b in zip(ring, ring[1:] + ring[:1]):
                dx, dy = b[0] - a[0], b[1] - a[1]
                L = math.hypot(dx, dy)
                if L < 1.5:                        # skip only tiny edges; short edges get a blank
                    continue
                mid = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
                tx, ty = dx / L, dy / L
                nx, ny = dy / L, -dx / L                               # outward normal (CCW)
                if pg.contains(Point(mid[0] + nx * 0.5, mid[1] + ny * 0.5)):
                    nx, ny = -nx, -ny                                  # safety: force outward
                # a street-facing edge faces open space: 1 m out is not inside ANOTHER building
                # (only tightly-abutting shared walls fail this, so every wall a rider sees is kept)
                if shapely.contains_xy(bgeom, mid[0] + nx * 1.0, mid[1] + ny * 1.0):
                    continue
                yaw = math.degrees(math.atan2(-ny, -nx))               # kit local -X -> outward
                z0 = min(float(zfn(a[0] + nx * 2.0, a[1] + ny * 2.0)),
                         float(zfn(b[0] + nx * 2.0, b[1] + ny * 2.0)),
                         float(zfn(mid[0] + nx * 2.0, mid[1] + ny * 2.0))) - BUILDING_H
                nbay = int(L / bay)
                ox, oy = nx * (FAC_PROUD_M - depth), ny * (FAC_PROUD_M - depth)
                for k in range(nbay):
                    s = k * bay
                    ax, ay = a[0] + tx * s + ox, a[1] + ty * s + oy
                    for mp, dz in stack:
                        pieces.append([mesh_id(mp), round(ax, 3), round(ay, 3),
                                       round(z0 + dz, 3), round(yaw, 2)])
                rem = L - nbay * bay
                bx, by = nx * FAC_BOX_OFF, ny * FAC_BOX_OFF
                if rem > 0.6:                                          # blank wall fills the end
                    s = nbay * bay + rem / 2
                    blanks.append([round(a[0] + tx * s + bx, 3), round(a[1] + ty * s + by, 3),
                                   round(z0, 3), round(yaw, 2), round(rem, 3), round(total_h, 2)])
                plinths.append([round(a[0] + bx, 3), round(a[1] + by, 3),
                                round(b[0] + bx, 3), round(b[1] + by, 3), round(z0, 3)])
                corners.append([round(a[0] + bx, 3), round(a[1] + by, 3),
                                round(z0, 3), round(total_h, 2)])
    # per-mesh local XY bounds (m) for the verify rectangle check: local X [-depth,0], Y [-bay,0]
    kitb = {}
    for sk, sv in FAC_STYLES.items():
        for mp, _ in [sv["ground"], sv["cornice"]] + sv["floors"]:
            kitb[mp] = (-sv["depth"], 0.0, -sv["bay"], 0.0)
    mbnd = [list(kitb.get(m, (-1.5, 0.0, -4.5, 0.0))) for m in meshes]
    data = dict(meshes=meshes, meshes_bounds=mbnd, pieces=pieces, blanks=blanks,
                plinths=plinths, corners=corners)
    json.dump(data, open(out("facades.json"), "w"))
    log("facades: %d kit instances (%d meshes), %d blanks, %d plinths, %d corners"
        % (len(pieces), len(meshes), len(blanks), len(plinths), len(corners)))


def building_height(t):
    if t.get("height"):
        try:
            return max(4.0, float(str(t["height"]).split()[0]))
        except ValueError:
            pass
    for k in ("building:levels", "levels"):
        if t.get(k):
            try:
                return max(3.5, float(t[k].split(";")[0]) * 3.5)
            except ValueError:
                pass
    return 15.0


def extrude_polygon(mesh, poly, base, top, street_z, col):
    """Walls (per edge, outward-facing) and a flat roof for a footprint, in MuJoCo m -> UE cm. The
    ring is oriented CCW, so the outward normal of edge a->b is (dy, -dx). Wall UV0 is metres: u
    along the wall, v = height above the street, so the facade window grid reads as windows, not
    stripes. col is the per-building facade tint (sRGB), carried as vertex colour."""
    from shapely.geometry.polygon import orient
    poly = orient(poly, sign=1.0)              # exterior CCW
    ring = list(poly.exterior.coords)[:-1]
    col4 = (col[0], col[1], col[2], 255)
    for a, b in zip(ring, ring[1:] + ring[:1]):
        dx, dy = b[0] - a[0], b[1] - a[1]
        ln = math.hypot(dx, dy)
        if ln < 1e-6:
            continue
        nrm = (dy / ln, -dx / ln, 0.0)
        p = [(a[0], a[1], base), (b[0], b[1], base), (b[0], b[1], top), (a[0], a[1], top)]
        vb, vt = base - street_z, top - street_z            # height above the street (m)
        uv = np.array([[0, vb], [ln, vb], [ln, vt], [0, vt]], np.float32)
        quad(mesh, p, "Facade", nrm, uv, col4)
    for tri in triangulate(poly):
        if tri.area < 0.5 or not poly.contains(tri.centroid):
            continue
        c = list(tri.exterior.coords)[:3]
        p = [(c[0][0], c[0][1], top), (c[1][0], c[1][1], top), (c[2][0], c[2][1], top)]
        P = gc.to_ue(np.array(p))
        nrm = np.tile([0, 0, 1.0], (3, 1))
        cc = np.tile(col4, (3, 1))
        mesh.add(P, nrm, np.zeros((3, 2), np.float32), np.array([[0, 1, 2]], np.uint32), "Roof", col=cc)


def quad(mesh, pts4, section, normal, uv, col4):
    P = gc.to_ue(np.array(pts4))
    nrm = np.tile(np.array(normal, float), (4, 1))
    tri = np.array([[0, 1, 2], [0, 2, 3]], np.uint32)
    mesh.add(P, nrm, uv, tri, section, col=np.tile(col4, (4, 1)))


# --- markings: lane lines, green bike lanes, crosswalks ------------------------------------------
def ribbon(mesh, zfn, pts, width, section, lift=MARK_LIFT):
    if len(pts) < 2:
        return
    P = np.array(pts, float)
    dx = np.gradient(P[:, 0]); dy = np.gradient(P[:, 1])
    hd = np.arctan2(dy, dx)
    nx, ny = -np.sin(hd), np.cos(hd)
    X = np.stack([P[:, 0] + nx * (-width / 2), P[:, 0] + nx * (width / 2)], 1)
    Y = np.stack([P[:, 1] + ny * (-width / 2), P[:, 1] + ny * (width / 2)], 1)
    Z = zfn(X, Y) + lift
    gc.add_grid(mesh, X, Y, Z, section, X, Y)


def offset_lines(line, d):
    """Offset a polyline right by d; return a list of coord lists (an offset may split)."""
    try:
        o = LineString(line).offset_curve(-d, join_style=2, mitre_limit=3.0)
    except Exception:
        return []
    if o.is_empty:
        return []
    geoms = o.geoms if o.geom_type == "MultiLineString" else [o]
    return [list(gg.coords) for gg in geoms if gg.geom_type == "LineString" and len(gg.coords) >= 2]


def street_lines(osm, name):
    """Ordered centre polylines of a named vehicle street, in the local frame."""
    out = []
    for t, nd in osm.ways:
        if t.get("name") == name and t.get("highway") in osm_mod.VEHICLE:
            out.append((t, osm.line(nd)))
    return out


def build_marks(osm, zfn, lvl, out, hx, hy, bgeom):
    marks = obm.Mesh()
    grid = Polygon([(-hx, -hy), (hx, -hy), (hx, hy), (-hx, hy)])

    def clip(line):
        g = LineString(line).intersection(grid)
        segs = g.geoms if g.geom_type == "MultiLineString" else [g]
        return [list(s.coords) for s in segs if s.geom_type == "LineString" and s.length > 2]

    # green SF bike lane: a clean 1.5 m ribbon centred on the demo line where it runs on a bike
    # street (Brannan/King/2nd), with white edge lines. Clipped to the street carriageway, so no
    # paint runs onto the sidewalk or over a kerb.
    bike_parts = []
    for name in ("Brannan Street", "King Street", "2nd Street"):
        for t, line in street_lines(osm, name):
            bike_parts.append(LineString(line).buffer(osm_mod.road_half_width(t) - 0.6))
    bike_geom = unary_union(bike_parts) if bike_parts else Polygon()
    dp = [(p[0], p[1]) for p in lvl["demo_path"]]
    run = []
    for i, p in enumerate(dp + [None]):
        on = p is not None and bike_geom.contains(Point(p))
        if on:
            run.append(p)
        elif run:
            if len(run) >= 3:
                pts = osm_mod.resample(run, 1.0)
                ribbon(marks, zfn, pts, 1.5, "Bike")
                for d in (0.8, -0.8):                       # straight white edge lines
                    for e in offset_lines(pts, d):
                        ribbon(marks, zfn, e, 0.1, "White")
            run = []

    # white edge + centre lines along every vehicle street in the grid
    for t, nd in osm.ways:
        if t.get("highway") not in osm_mod.VEHICLE:
            continue
        line = osm.line(nd)
        hw = osm_mod.road_half_width(t)
        # the half width is an estimate; keep the edge line safely on the road side of the kerb
        edge = min(hw - 0.3, hw * 0.85)
        for d in (edge, -edge):
            for off in offset_lines(line, d):
                for s in clip(off):
                    ribbon(marks, zfn, osm_mod.resample(s, 1.0), 0.12, "White")
        if t.get("oneway") != "yes":   # double yellow centre on two-way streets
            for s in clip(line):
                rs = osm_mod.resample(s, 1.0)
                for d in (-0.12, 0.12):
                    for off in offset_lines(rs, d):
                        ribbon(marks, zfn, off, 0.1, "Yellow")

    # carriageways: carr_full is kerb-to-kerb (used to clip the route crossings); carr_drive is the
    # DRIVABLE travel lanes (kerb-to-kerb minus the ~2 m parking/gutter band), written for
    # verify_embarcadero.py -- a facade on the sidewalk must not count as over the road.
    full_parts, drive_parts = [], []
    for t, nd in osm.ways:
        if t.get("highway") in osm_mod.VEHICLE:
            ls = LineString(osm.line(nd))
            hw = osm_mod.road_half_width(t)
            full_parts.append(ls.buffer(hw))
            drive_parts.append(ls.buffer(max(2.5, hw - 2.0)))
    carr = unary_union(full_parts).intersection(grid) if full_parts else Polygon()
    # the open drivable road = the travel lanes MINUS the footprints grown by the 1.2 m veneer
    # allowance (OSM street centrelines run close to buildings, so the raw buffer overlaps them). A
    # facade overlaps this only if it protrudes MORE than 1.2 m past its footprint into the open
    # road -- exactly the rule the verify check must enforce.
    carr_drive = (unary_union(drive_parts).difference(bgeom.buffer(1.2)).intersection(grid)
                  if drive_parts else Polygon())
    dpolys = (carr_drive.geoms if carr_drive.geom_type == "MultiPolygon"
              else ([carr_drive] if not carr_drive.is_empty else []))

    def ring(r):
        return [[round(x, 2), round(y, 2)] for x, y in r.coords]

    json.dump({"polys": [{"ext": ring(g.exterior), "holes": [ring(h) for h in g.interiors]}
                         for g in dpolys]}, open(out("carriageway.json"), "w"))

    # zebra crosswalks: the two route crossings, plus one clean zebra per CLUSTER of OSM crossings
    # (SF junctions hold many crossing nodes; cluster them within 8 m so the bars do not tangle).
    route = LineString([(p[0], p[1]) for p in lvl["demo_path"]])
    drawn = []
    for c in lvl["crossings"]:
        zebra(marks, zfn, c["a"], c["b"], clip_geom=carr)
        drawn.append(((c["a"][0] + c["b"][0]) / 2, (c["a"][1] + c["b"][1]) / 2))
    cross_pts = []
    for nid, tg in osm.ntags.items():
        if tg.get("highway") == "crossing" or tg.get("crossing"):
            p = osm.xy(nid)
            if grid.contains(Point(p)) and route.distance(Point(p)) <= 60.0:
                cross_pts.append(p)
    for p in cross_pts:
        if any((p[0] - dx) ** 2 + (p[1] - dy) ** 2 < 64.0 for dx, dy in drawn):   # 8 m cluster
            continue
        if zebra_at(marks, zfn, p, osm):
            drawn.append((p[0], p[1]))
    gc.ue_winding(marks).write(out("marks.obm"))
    log("marks written")


def zebra(mesh, zfn, a, b, clip_geom=None, bars=7, half=1.8):
    """A zebra from a to b (across the street): bars run perpendicular to a->b (along the street),
    each clipped to clip_geom so no bar spills past the carriageway into the intersection."""
    a, b = np.array(a, float), np.array(b, float)
    d = b - a
    L = np.linalg.norm(d)
    if L < 1:
        return
    u = d / L
    nrm = np.array([-u[1], u[0]])
    for i in range(bars):
        t = (i + 0.5) / bars
        c = a + d * t
        seg = LineString([tuple(c - nrm * half), tuple(c + nrm * half)])
        if clip_geom is not None:
            seg = seg.intersection(clip_geom)
        parts = seg.geoms if seg.geom_type == "MultiLineString" else [seg]
        for sp in parts:
            if sp.geom_type == "LineString" and sp.length > 0.25:
                cs = list(sp.coords)
                ribbon(mesh, zfn, [cs[0], cs[-1]], 0.45, "White")


def zebra_at(mesh, zfn, p, osm):
    """One clean zebra across the nearest vehicle street at p: the bars span the carriageway and run
    along the street (perpendicular to the crossing direction), clipped to that street's own width so
    perpendicular crossings at a junction do not overlap. Returns True if a zebra is drawn."""
    pp = Point(p)
    best = None
    for t, nd in osm.ways:
        if t.get("highway") not in osm_mod.VEHICLE:
            continue
        ls = LineString(osm.line(nd))
        d = ls.distance(pp)
        if best is None or d < best[0]:
            best = (d, ls, t)
    if best is None or best[0] > 8.0:
        return False
    _, ls, t = best
    hw = osm_mod.road_half_width(t)
    s = ls.project(pp)
    a0 = ls.interpolate(max(0.0, s - 0.5))
    b0 = ls.interpolate(min(ls.length, s + 0.5))
    tl = math.hypot(b0.x - a0.x, b0.y - a0.y) or 1.0
    tx, ty = (b0.x - a0.x) / tl, (b0.y - a0.y) / tl
    nx, ny = -ty, tx                       # across the street
    c = ls.interpolate(s)
    a = (c.x - nx * hw * 0.95, c.y - ny * hw * 0.95)
    b = (c.x + nx * hw * 0.95, c.y + ny * hw * 0.95)
    zebra(mesh, zfn, a, b, clip_geom=ls.buffer(hw * 0.98), bars=max(4, int(round(2 * hw / 0.8))))
    return True


# --- dressing: lamps, trees, palms, benches ------------------------------------------------------
def build_dressing(osm, zfn, lvl, out, hx, hy):
    route = LineString([(p[0], p[1]) for p in lvl["demo_path"]])
    grid = Polygon([(-hx, -hy), (hx, -hy), (hx, hy), (-hx, hy)])
    D = {"lamp": [], "tree": [], "palm": [], "bench": []}

    def clear(x, y):
        return route.distance(Point(x, y)) > 1.5

    def emit(kind, x, y, yaw=0.0, scale=1.0):
        if grid.contains(Point(x, y)) and clear(x, y):
            D[kind].append([round(x, 2), round(y, 2), round(float(zfn(x, y)), 2), round(yaw, 1), scale])

    # street lamps + trees along the sidewalk of each vehicle street
    for t, nd in osm.ways:
        if t.get("highway") not in osm_mod.VEHICLE:
            continue
        line = osm.line(nd)
        hw = osm_mod.road_half_width(t)
        for sgn in (1.0, -1.0):
            for off in offset_lines(line, sgn * (hw + 1.5)):
                rs = osm_mod.resample(off, LAMP_SPACING)
                for k, (x, y) in enumerate(rs):
                    emit("lamp", x, y)
                    if k % 2 == 1:
                        emit("tree", x + 6.0, y)

    # palms + benches along The Embarcadero legs (the promenade); palms on the water side
    for name in ("The Embarcadero", "Herb Caen Way", "Herb Caen Way...The Embarcadero"):
        for t, line in street_lines(osm, name):
            rs = osm_mod.resample(line, PALM_SPACING)
            for x, y in rs:
                emit("palm", x, y)
            for x, y in osm_mod.resample(line, 30.0):
                emit("bench", x, y)
    json.dump(D, open(out("dressing.json"), "w"))
    log("dressing:", {k: len(v) for k, v in D.items()})


# --- meta ----------------------------------------------------------------------------------------
def write_meta(out, meta, cdir, zfn, hx, hy):
    # a coarse 5 m heightmap, so the stills camera can find the ground z without an editor raycast
    hdx = 5.0
    xs = np.arange(-hx, hx + hdx * 0.5, hdx)
    ys = np.arange(-hy, hy + hdx * 0.5, hdx)
    X, Y = np.meshgrid(xs, ys, indexing="ij")
    Zc = zfn(X, Y)
    m = dict(origin_cm=[0.0, 0.0, 0.0], origin_yaw_deg=0.0,
             course_dir=os.path.abspath(cdir),
             spawn=meta["spawn"], grid=json.load(open(os.path.join(cdir, "level.json")))["grid"],
             corridor_lift_m=COR_LIFT, ground_drop_m=GROUND_DROP, water_z_m=WATER_Z,
             half_x=meta["half_extent_x_m"], half_y=meta["half_extent_y_m"],
             heightmap=dict(dx=hdx, x0=float(xs[0]), y0=float(ys[0]), nx=len(xs), ny=len(ys),
                            z=[round(float(v), 2) for v in Zc.ravel(order="C")]))
    json.dump(m, open(out("meta.json"), "w"))


def main():
    cdir, odir = sys.argv[1], sys.argv[2]
    os.makedirs(odir, exist_ok=True)
    out = lambda n: os.path.join(odir, n)
    meta, lvl, h, rows = load(cdir)
    g = lvl["grid"]
    hx, hy = meta["half_extent_x_m"], meta["half_extent_y_m"]
    zfn = make_sampler(h, g)
    osm = osm_mod.OSM()
    osm.origin = (meta["frame"]["origin_easting"], meta["frame"]["origin_northing"])
    log("staged OSM, origin", osm.origin)

    grid = Polygon([(-hx, -hy), (hx, -hy), (hx, hy), (-hx, hy)])
    foots = []
    for t, nd in osm.ways:
        if "building" in t and nd[0] == nd[-1] and len(nd) >= 4:
            try:
                p = Polygon(osm.line(nd))
                foots.append(p if p.is_valid else p.buffer(0))
            except Exception:
                pass
    bgeom = unary_union(foots).intersection(grid) if foots else grid.buffer(-1e6)
    log("building footprints union ready")
    # the promenade (Herb Caen Way / The Embarcadero): a buffer, used to pave the corridor there
    prom_lines = []
    for nm in ("The Embarcadero", "Herb Caen Way", "Herb Caen Way...The Embarcadero"):
        prom_lines += [LineString(ln) for _, ln in street_lines(osm, nm)]
    prom_geom = unary_union(prom_lines).buffer(9.0) if prom_lines else grid.buffer(-1e6)

    build_ground(zfn, out, hx, hy, bgeom)
    build_corridor(zfn, lvl["demo_path"], out, bgeom, prom_geom)
    build_boxes(zfn, rows, out)
    build_water(h, g, out)
    build_buildings(osm, zfn, out, hx, hy)
    build_facades(osm, zfn, out, hx, hy, bgeom)
    build_marks(osm, zfn, lvl, out, hx, hy, bgeom)
    build_dressing(osm, zfn, lvl, out, hx, hy)
    write_meta(out, meta, cdir, zfn, hx, hy)
    log("done ->", odir)


if __name__ == "__main__":
    main()
