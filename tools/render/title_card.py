#!/usr/bin/env python3
"""Draw the title overlay (transparent 1920x1080 PNG) for the carve render.

usage: title_card.py OUT.png "TITLE" "subtitle line"
ffmpeg overlays it on the first seconds with a fade (this ffmpeg build has no drawtext).
"""
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

out, title, sub = sys.argv[1], sys.argv[2], sys.argv[3]
W, H = 1920, 1080
img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
font_t = ImageFont.truetype("/System/Library/Fonts/Avenir Next.ttc", 112, index=5)
font_s = ImageFont.truetype("/System/Library/Fonts/Avenir Next.ttc", 38, index=0)

# Soft shadow layer for legibility over a bright sky, then the text.
shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
ds = ImageDraw.Draw(shadow)
d = ImageDraw.Draw(img)
tw = d.textlength(title, font=font_t)
sw = d.textlength(sub, font=font_s)
ty, sy = H * 0.40, H * 0.40 + 140
for dd in (ds,):
    dd.text(((W - tw) / 2, ty), title, font=font_t, fill=(0, 0, 0, 170))
    dd.text(((W - sw) / 2, sy), sub, font=font_s, fill=(0, 0, 0, 170))
shadow = shadow.filter(ImageFilter.GaussianBlur(10))
img = Image.alpha_composite(img, shadow)
d = ImageDraw.Draw(img)
d.text(((W - tw) / 2, ty), title, font=font_t, fill=(255, 244, 230, 255))
d.text(((W - sw) / 2, sy), sub, font=font_s, fill=(255, 236, 214, 235))
img.save(out)
print("wrote", out)
