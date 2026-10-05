"""OBM1 mesh files: the exact-geometry hand-off from tools/trail/*.py to Unreal.

UTrailBuildLibrary::CreateStaticMeshFromObm (C++) reads them. Positions are in the Unreal frame,
in cm. No importer is in the path, so no axis or unit conversion can move a vertex.

Layout (little-endian): b"OBM1", u32 nverts, u32 ntris, u32 nsections, u32 flags
(bit 0 = sRGB vertex colours, bit 1 = UV1), f32 pos[n,3], f32 nrm[n,3], f32 uv0[n,2],
[u8 col[n,4]], [f32 uv1[n,2]], u32 idx[t,3], u32 section[t], then per section u32 len + utf-8 name.
A vertex belongs to the triangles of one section only.
"""
import struct

import numpy as np


class Mesh:
    def __init__(self):
        self.parts = []  # (pos, nrm, uv0, col, uv1, tri, section_name)
        self.sections = []

    def add(self, pos, nrm, uv0, tri, section, col=None, uv1=None):
        if section not in self.sections:
            self.sections.append(section)
        n = len(pos)
        col = np.full((n, 4), 255, np.uint8) if col is None else np.asarray(col, np.uint8)
        uv1 = np.zeros((n, 2), np.float32) if uv1 is None else np.asarray(uv1, np.float32)
        self.parts.append((np.asarray(pos, np.float32), np.asarray(nrm, np.float32), np.asarray(uv0, np.float32),
                           col, uv1, np.asarray(tri, np.uint32), section))

    def merged(self):
        pos, nrm, uv0, col, uv1, tri, sec = [], [], [], [], [], [], []
        base = 0
        for p, n, u, c, u1, t, s in self.parts:
            pos.append(p); nrm.append(n); uv0.append(u); col.append(c); uv1.append(u1)
            tri.append(t + base)
            sec.append(np.full(len(t), self.sections.index(s), np.uint32))
            base += len(p)
        return (np.concatenate(pos), np.concatenate(nrm), np.concatenate(uv0), np.concatenate(col),
                np.concatenate(uv1), np.concatenate(tri), np.concatenate(sec))

    def write(self, path, colors=False, uv1=False):
        pos, nrm, uv0, col, u1, tri, sec = self.merged()
        nl = np.linalg.norm(nrm, axis=1, keepdims=True)
        nrm = nrm / np.maximum(nl, 1e-9)
        flags = (1 if colors else 0) | (2 if uv1 else 0)
        with open(path, "wb") as f:
            f.write(b"OBM1" + struct.pack("<4I", len(pos), len(tri), len(self.sections), flags))
            f.write(pos.astype("<f4").tobytes()); f.write(nrm.astype("<f4").tobytes()); f.write(uv0.astype("<f4").tobytes())
            if colors:
                f.write(col.astype(np.uint8).tobytes())
            if uv1:
                f.write(u1.astype("<f4").tobytes())
            f.write(tri.astype("<u4").tobytes()); f.write(sec.astype("<u4").tobytes())
            for s in self.sections:
                b = s.encode()
                f.write(struct.pack("<I", len(b)) + b)
        return len(pos), len(tri)


def grid_normals(P):
    """Vertex normals of a (rows, cols, 3) grid of points."""
    du = np.gradient(P, axis=1)
    dv = np.gradient(P, axis=0)
    n = np.cross(du, dv)
    return n / np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-9)


def grid_tris(rows, cols, flip=False):
    """Two triangles per quad of a rows x cols vertex grid, wound so +Z faces up for a grid whose
    columns run along +X and rows along +Y (Unreal is left-handed: clockwise seen from above)."""
    r, c = np.meshgrid(np.arange(rows - 1), np.arange(cols - 1), indexing="ij")
    a = (r * cols + c).ravel()
    b, d, e = a + 1, a + cols, a + cols + 1
    t = np.concatenate([np.stack([a, d, b], 1), np.stack([b, d, e], 1)])
    return t[:, ::-1] if flip else t


def box(center, size, yaw_deg=0.0, uv_scale=1.0 / 100.0, uv_offset=(0.0, 0.0)):
    """A box as 24 vertices, 12 triangles, box-mapped UVs in metres (uv_scale per cm).
    Returns pos, nrm, uv, tri in the Unreal frame (cm). yaw about +Z."""
    cx, cy, cz = center
    sx, sy, sz = [s / 2.0 for s in size]
    faces = [  # normal, u axis, v axis
        ((1, 0, 0), (0, 1, 0), (0, 0, 1)), ((-1, 0, 0), (0, -1, 0), (0, 0, 1)),
        ((0, 1, 0), (-1, 0, 0), (0, 0, 1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
        ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (1, 0, 0), (0, -1, 0)),
    ]
    half = np.array([sx, sy, sz])
    pos, nrm, uv, tri = [], [], [], []
    for k, (n, u, v) in enumerate(faces):
        n, u, v = np.array(n, float), np.array(u, float), np.array(v, float)
        corners = [n - u - v, n + u - v, n + u + v, n - u + v]
        for cc in corners:
            p = cc * half
            pos.append(p)
            nrm.append(n)
            uv.append((np.dot(p, u) * uv_scale + uv_offset[0], -np.dot(p, v) * uv_scale + uv_offset[1]))
        b = 4 * k
        # Unreal front faces are clockwise seen from outside (left-handed frame).
        tri += [(b, b + 2, b + 1), (b, b + 3, b + 2)]
    pos, nrm = np.array(pos), np.array(nrm)
    c, s = np.cos(np.radians(yaw_deg)), np.sin(np.radians(yaw_deg))
    R = np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])
    pos = pos @ R.T + np.array([cx, cy, cz])
    nrm = nrm @ R.T
    return pos, nrm, np.array(uv), np.array(tri)
