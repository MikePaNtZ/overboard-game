#!/usr/bin/env python3
"""Repaint the MetaHuman head base colour when texture synthesis did not run.

usage: repaint_head_basecolor.py <head_bc.png> <out.png> [size]

Without the Creator's optional content, texture synthesis is off and the baked head base colour
(T_Head_BC_VT) is a flat grey (sRGB about 0.76) with only faint shading. The body base colour
comes from the cloud and is real skin. The bottom strip of the head texture (the neck seam) is
already that skin colour. This keeps the grey's shading and replaces its colour with the neck
skin tone, in linear space: out = skin * (grey / grey_median). Coloured pixels are kept.
Needs numpy and Pillow (the overboard .venv has both). Called by post_build_skater.py.
"""
import sys

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None


def to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 4096
    img = Image.open(src).convert("RGB")
    if img.size[0] != size:
        img = img.resize((size, size), Image.LANCZOS)
    h = np.asarray(img).astype(np.float32) / 255.0
    sat = h.max(-1) - h.min(-1)
    n = h.shape[0]
    neck = h[int(n * 0.965):int(n * 0.985), int(n * 0.42):int(n * 0.58)].reshape(-1, 3)
    skin = np.median(neck[(neck.max(-1) - neck.min(-1)) > 0.12], axis=0)
    grey_px = h[(sat < 0.03) & (h.mean(-1) > 0.5)]
    grey = float(np.median(grey_px.mean(-1)))
    # Weight 1 up to 80 % of the skin's own saturation, 0 at full skin saturation (the neck strip).
    # The grey-to-skin fade above the neck is repainted too; a partial blend left a pale band.
    skin_sat = float(skin.max() - skin.min())
    w = np.clip((skin_sat - sat) / (0.2 * skin_sat), 0.0, 1.0)[..., None]
    lin = to_lin(h)
    shade = lin.mean(-1, keepdims=True) / to_lin(np.float32(grey))
    painted = to_lin(skin)[None, None, :] * shade
    out = to_srgb(w * painted + (1.0 - w) * lin)
    Image.fromarray((out * 255.0 + 0.5).astype(np.uint8)).save(dst)
    print(f"skin sRGB {np.round(skin, 3).tolist()} grey {grey:.3f} grey share {len(grey_px) / n / n:.2f} -> {dst}")


if __name__ == "__main__":
    main()
