"""Level 2 (Embarcadero cruise loop), stage 1: OSM + DEM -> local frame, street shapes, route.

Frame: UTM zone 10N (EPSG:26910) metres, shifted so the heightfield grid is centred on the
origin. x = grid east, y = grid north, z = NAVD88 metres (USGS 3DEP 1 m bare-earth DEM).
Map data (c) OpenStreetMap contributors, ODbL 1.0.

No board physics here. This only describes the world; sim-host computes the ride.
"""
import heapq
import math
import os
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path

import numpy as np
import pyproj
import tifffile
from shapely.geometry import LineString, Point, Polygon
from shapely.ops import unary_union

DATA = Path(os.environ.get("OB_LEVELS_DATA",
                           Path.home() / "projects/overboard-viz/out/carve-lab/data/sources/embarcadero"))
LANE_M = 3.0
BIKE_LANE_M = {"lane": 1.6, "track": 2.0, "shared_lane": 0.0}
PARKING_M = 2.4          # SF streets of this class have a parking lane each side
VEHICLE = {"primary", "secondary", "tertiary", "residential", "unclassified", "primary_link",
           "secondary_link", "tertiary_link"}
ODBL = "Map data (c) OpenStreetMap contributors, ODbL 1.0"


class OSM:
    def __init__(self, path=DATA / "embarcadero.osm"):
        root = ET.parse(path).getroot()
        tr = pyproj.Transformer.from_crs(4326, 26910, always_xy=True)
        ids, lon, lat, ntags = [], [], [], {}
        for n in root.iter("node"):
            ids.append(n.get("id"))
            lon.append(float(n.get("lon")))
            lat.append(float(n.get("lat")))
            t = {k.get("k"): k.get("v") for k in n.iter("tag")}
            if t:
                ntags[n.get("id")] = t
        E, N = tr.transform(lon, lat)
        self.utm = {i: (e, nn) for i, e, nn in zip(ids, E, N)}
        self.ntags = ntags
        self.ways = []
        for w in root.iter("way"):
            t = {k.get("k"): k.get("v") for k in w.iter("tag")}
            nd = [x.get("ref") for x in w.iter("nd") if x.get("ref") in self.utm]
            if len(nd) >= 2:
                self.ways.append((t, nd))
        self.origin = (0.0, 0.0)

    def xy(self, nid):
        e, n = self.utm[nid]
        return e - self.origin[0], n - self.origin[1]

    def line(self, nd):
        return [self.xy(n) for n in nd]


def road_half_width(t):
    """Half the kerb-to-kerb width of one OSM road way (estimate from lanes and bike lanes)."""
    oneway = t.get("oneway") == "yes"
    try:
        lanes = int(t.get("lanes", "2" if not oneway else "2").split(";")[0])
    except ValueError:
        lanes = 2
    w = lanes * LANE_M
    for side in ("left", "right"):
        v = t.get(f"cycleway:{side}") or t.get("cycleway:both") or t.get("cycleway")
        if v in BIKE_LANE_M and not (oneway and side == "left"):
            w += BIKE_LANE_M[v]
    if not oneway and t.get("highway") in ("secondary", "tertiary", "residential"):
        w += 2 * PARKING_M
    return w / 2


def graph(osm, name=None, directed=False):
    """Road graph: node -> [(next, length)]. With a name, only that street; directed honours oneway."""
    G = defaultdict(list)
    for t, nd in osm.ways:
        h = t.get("highway")
        if not h or (name and t.get("name") != name and t.get("name", "").split("...")[0] != name):
            continue
        if not name and h not in VEHICLE | {"path", "pedestrian", "footway", "cycleway"}:
            continue
        for a, b in zip(nd, nd[1:]):
            d = math.dist(osm.xy(a), osm.xy(b))
            G[a].append((b, d))
            if not (directed and t.get("oneway") == "yes"):
                G[b].append((a, d))
    return G


def shortest(G, s, t):
    D, P, q = {s: 0.0}, {}, [(0.0, s)]
    while q:
        d, u = heapq.heappop(q)
        if u == t:
            break
        if d > D[u]:
            continue
        for v, c in G[u]:
            if d + c < D.get(v, 1e18):
                D[v], P[v] = d + c, u
                heapq.heappush(q, (d + c, v))
    if t not in D:
        raise ValueError(f"no path {s} -> {t}")
    path = [t]
    while path[-1] != s:
        path.append(P[path[-1]])
    return path[::-1]


def nearest_node(osm, G, xy):
    return min(G, key=lambda n: math.dist(osm.xy(n), xy))


def named_nodes(osm, name):
    out = set()
    for t, nd in osm.ways:
        if t.get("name") == name and t.get("highway") in VEHICLE:
            out |= set(nd)
    return out


def junction(osm, a, b):
    s = named_nodes(osm, a) & named_nodes(osm, b)
    if not s:
        raise ValueError(f"no junction {a} x {b}")
    pts = np.array([osm.xy(n) for n in s])
    c = pts.mean(0)
    return tuple(c)


def offset_right(pts, d):
    """Offset a polyline to the right of travel by d metres."""
    L = LineString(pts)
    o = L.offset_curve(-d, join_style=2, mitre_limit=3.0)
    return list(o.coords)


def resample(pts, step=0.5):
    L = LineString(pts)
    n = max(2, int(L.length / step))
    return [L.interpolate(i * L.length / n).coords[0] for i in range(n + 1)]


class DEM:
    def __init__(self, path=DATA / "dem_x55y419.tif"):
        p = tifffile.TiffFile(path).pages[0]
        tags = {k.name: k.value for k in p.tags.values()}
        self.e0, self.n0 = tags["ModelTiepointTag"][3], tags["ModelTiepointTag"][4]
        self.Z = p.asarray()

    def sample(self, E, N):
        """Bilinear sample at UTM arrays."""
        c = np.asarray(E) - self.e0 - 0.5
        r = self.n0 - np.asarray(N) - 0.5
        c0, r0 = np.floor(c).astype(int), np.floor(r).astype(int)
        fc, fr = c - c0, r - r0
        Z = self.Z
        z = (Z[r0, c0] * (1 - fc) * (1 - fr) + Z[r0, c0 + 1] * fc * (1 - fr) +
             Z[r0 + 1, c0] * (1 - fc) * fr + Z[r0 + 1, c0 + 1] * fc * fr)
        return z
