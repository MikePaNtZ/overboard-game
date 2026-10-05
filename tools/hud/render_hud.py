#!/usr/bin/env python3
"""Draw the Overboard rider HUD as transparent PNG overlays, one per rendered frame. Runs OUTSIDE Unreal.

usage: render_hud.py <track.npz> <cameras.json> <shot> <out_dir> [--size 1920x1080] [--over FRAMES_DIR]
                     [--frames A:B] [--spec tools/hud/hud_spec.json]

The layout, colours, fonts, thresholds and smoothing come from hud_spec.json (see docs/hud-spec.md),
the same file the game HUD reads. The values come from the track npz (the controls track's HUD
columns: speed_mph, torque_nm, torque_lim_nm, batt_soc, batt_v, flags). Frame f of a shot shows sim
time replay_offset + f / fps * replay_rate, the clock ABoardActor replays the board on.

--over FRAMES_DIR composites the HUD over the rendered frames (jpeg or png, same frame numbers) and
writes jpegs instead of transparent PNGs: for look-dev and review stills.
"""
import argparse
import glob
import json
import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_DIR = "/Users/Shared/Epic Games/UE_5.7/Engine/Content/Slate/Fonts"
SS = 2  # supersampling


def rgba(c):
    h, a = c
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), int(round(255 * a)))


def ema(t, x, tau):
    """Causal first-order smoothing (the game can do the same on the live wire)."""
    if tau <= 0:
        return x.copy()
    y = np.empty_like(x)
    y[0] = x[0]
    for i in range(1, len(x)):
        a = 1.0 - math.exp(-(t[i] - t[i - 1]) / tau)
        y[i] = y[i - 1] + a * (x[i] - y[i - 1])
    return y


class Hud:
    def __init__(self, spec, d):
        self.s = spec
        self.t = d["t"]
        sp = spec
        self.speed = ema(self.t, d[sp["speed"]["field"]].astype(float), sp["speed"]["smooth_s"])
        self.torque = ema(self.t, d[sp["torque"]["field"]].astype(float), sp["torque"]["smooth_s"])
        self.tlim = d[sp["torque"]["limit_field"]].astype(float)
        self.soc = ema(self.t, d[sp["battery"]["soc_field"]].astype(float), sp["battery"]["smooth_s"])
        self.volt = ema(self.t, d[sp["battery"]["volt_field"]].astype(float), sp["battery"]["smooth_s"])
        self.flags = d[sp["warning"]["flags_field"]].astype(np.int64)
        mf = sp.get("margin", {}).get("field")
        self.margin = d[mf].astype(float) if mf and mf in d else None
        self.fonts = {}

    def font(self, role, px):
        key = (role, px)
        if key not in self.fonts:
            self.fonts[key] = ImageFont.truetype(os.path.join(FONT_DIR, self.s["fonts"][role]), px)
        return self.fonts[key]

    def at(self, arr, ts):
        return float(np.interp(ts, self.t, arr))

    def flag_at(self, ts):
        i = int(np.clip(np.searchsorted(self.t, ts) - 1, 0, len(self.t) - 1))
        return int(self.flags[i])

    def draw(self, size, ts):
        W, H = size
        k = H / 1080.0 * SS
        S = self.s
        C = {n: rgba(v) for n, v in S["colours"].items()}
        MINUS = S.get("text", {}).get("minus", "-")
        w = S["warning"]
        fl = self.flag_at(ts)
        handoff = bool(fl & w["handoff_bit"])
        solid, pulsed = bool(fl & w["solid_bit"]), bool(fl & w["pulsed_bit"])
        pulse = 0.5 + 0.5 * math.cos(2 * math.pi * w["pulse_hz"] * ts)
        chip_alpha = 1.0 if (handoff or solid) else w["pulse_min_alpha"] + (1 - w["pulse_min_alpha"]) * pulse

        def dim(col):  # after the handoff every value is muted: the controller no longer rides
            if not handoff:
                return col
            m = C["muted"]
            return m[:3] + (int(m[3] * w["dim_alpha"]),)

        img = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
        g = ImageDraw.Draw(img)
        P = S["panel"]
        pw, ph = P["size"][0] * k, P["size"][1] * k
        x0 = P["margin"][0] * k
        y0 = H * SS - P["margin"][1] * k - ph

        # the panel edge: teal; it warms to amber as the authority margin falls, turns the chip
        # colour on a warning, red on the handoff
        edge, edge_w = C["panel_edge"], P["edge"]
        mg = S.get("margin")
        if mg and self.margin is not None:
            m = self.at(self.margin, ts)
            u = max(0.0, min(1.0, (m - mg["warm_from"]) / (mg["warn_at"] - mg["warm_from"])))
            if u > 0:
                a_ = mg["edge_alpha"][0] + u * (mg["edge_alpha"][1] - mg["edge_alpha"][0])
                edge = C[mg["colour"]][:3] + (int(255 * a_),)
        if handoff or solid or pulsed:
            base = C["overboard"] if handoff else C["warn"]
            edge = base[:3] + (int(255 * w["edge_alpha"] * (1.0 if (handoff or solid) else chip_alpha)),)
            edge_w = w["edge_px"]
        g.rounded_rectangle([x0, y0, x0 + pw, y0 + ph], radius=P["radius"] * k, fill=C["panel"],
                            outline=edge, width=max(1, int(round(edge_w * k))))
        px, py = x0 + P["pad"][0] * k, y0 + P["pad"][1] * k
        inner_w = pw - 2 * P["pad"][0] * k

        # speed: a large numeral, the unit beside it on the same baseline
        sp = S["speed"]
        v = max(0.0, self.at(self.speed, ts))
        num = ("%%.%df" % sp["decimals"]) % v
        fN = self.font("numeral", int(sp["numeral_px"] * k))
        org = (px - 0.04 * sp["numeral_px"] * k, py - 0.20 * sp["numeral_px"] * k)
        g.text(org, num, font=fN, fill=dim(C["text"]))
        nb = g.textbbox(org, num, font=fN)
        fU = self.font("label", int(sp["unit_px"] * k))
        base_n = org[1] + fN.getmetrics()[0]                    # the numeral's baseline
        self._tracked(g, (nb[2] + 12 * k, base_n - fU.getmetrics()[0]), sp["unit"], fU, dim(C["muted"]), sp["tracking"])

        # the warning chip, top right of the panel
        chip = (w["handoff_text"], C["overboard"]) if handoff else ((w["text"], C["warn"]) if (solid or pulsed) else None)
        if chip:
            text, col = chip
            fC = self.font("label", int(w["chip_px"] * k))
            tw = self._tracked_width(g, text, fC, 0.12)
            cw, chh = tw + 2 * w["chip_pad"][0] * k, w["chip_px"] * k + 2 * w["chip_pad"][1] * k
            cx1 = x0 + pw - P["pad"][0] * k
            cy0 = py + 4 * k
            g.rounded_rectangle([cx1 - cw, cy0, cx1, cy0 + chh], radius=chh / 2, fill=col[:3] + (int(255 * chip_alpha),))
            self._tracked(g, (cx1 - cw + w["chip_pad"][0] * k, cy0 + w["chip_pad"][1] * k - 0.12 * w["chip_px"] * k),
                          text, fC, C["warn_text"][:3] + (int(255 * chip_alpha),), 0.12)

        # torque: label, then "value / limit", then a centred bar with the limits at its ends
        tq = S["torque"]
        ty = py + tq.get("top_px", 118) * k
        fL = self.font("label", int(tq["label_px"] * k))
        self._tracked(g, (px, ty), tq["label"], fL, dim(C["muted"]), 0.16)
        tau = self.at(self.torque, ts)
        lim = max(1e-6, self.at(self.tlim, ts))
        over = abs(tau) >= lim * tq.get("over_at_frac", 1.0)
        fV = self.font("value", int(tq["value_px"] * k))
        sign = "+" if tau >= 0.5 else (MINUS if tau <= -0.5 else "")
        v_txt = "%s%.0f" % (sign, abs(tau))
        l_txt = " / %.0f %s" % (lim, tq["unit"])
        lw, vw = g.textlength(l_txt, font=fV), g.textlength(v_txt, font=fV)
        bw, bh = min(tq["bar"][0] * k, inner_w), tq["bar"][1] * k
        vy = ty - 0.22 * tq["value_px"] * k
        g.text((px + bw - lw, vy), l_txt, font=fV, fill=dim(C["muted"]))
        g.text((px + bw - lw - vw, vy), v_txt, font=fV, fill=dim(C["over_limit"] if over else C["text"]))
        by = ty + tq["label_px"] * k + tq["bar_gap"] * k
        bx0, bx1, bc = px, px + bw, px + bw / 2
        g.rounded_rectangle([bx0, by, bx1, by + bh], radius=bh / 2, fill=C["track"])
        frac = max(-1.0, min(1.0, tau / lim))
        if handoff:   # keep the fill, muted, so the bar agrees with the number
            col = C["muted"][:3] + (int(255 * w.get("dim_fill_alpha", 0.35)),)
        elif over:
            col = C["over_limit"]
        elif abs(frac) >= tq["near_limit_frac"]:
            col = C["near_limit"]
        else:
            col = C["drive"] if frac >= 0 else C["brake"]
        if col and abs(frac) > 1e-3:
            a_, b_ = sorted((bc, bc + frac * bw / 2))
            g.rounded_rectangle([a_, by, max(b_, a_ + bh), by + bh], radius=bh / 2, fill=col)
        for xt in (bx0, bc, bx1):   # the two limits and zero
            g.rectangle([xt - 0.75 * k, by - 4 * k, xt + 0.75 * k, by + bh + 4 * k], fill=C["tick"])
        if over and not handoff:    # a red cap past the end the request went beyond
            xe = bx1 if tau > 0 else bx0
            d = tq.get("over_cap_px", 3) * k
            g.rectangle([xe + (2 * k if tau > 0 else -2 * k - d), by - 5 * k, xe + (2 * k + d if tau > 0 else -2 * k), by + bh + 5 * k],
                        fill=C["over_limit"])

        # battery: icon filled to the charge, the charge, the pack voltage
        bt = S["battery"]
        iw, ih = bt["icon"][0] * k, bt["icon"][1] * k
        yb = y0 + ph - P["pad"][1] * k - ih
        soc = self.at(self.soc, ts)
        bcol = C["battery_low"] if soc < bt["low_soc"] else C["battery_ok"]
        g.rounded_rectangle([px, yb, px + iw, yb + ih], radius=3 * k, outline=dim(C["muted"]), width=max(1, int(1.5 * k)))
        g.rectangle([px + iw, yb + ih * 0.3, px + iw + 3 * k, yb + ih * 0.7], fill=dim(C["muted"]))
        inset = 3 * k
        g.rectangle([px + inset, yb + inset, px + inset + (iw - 2 * inset) * max(0.0, min(1.0, soc)), yb + ih - inset],
                    fill=dim(bcol))
        fB = self.font("value", int(bt["value_px"] * k))
        txt = ("%%.%df%%%%" % bt["decimals"]) % (100 * soc)
        tx = px + iw + 16 * k
        g.text((tx, yb + ih / 2 - 0.62 * bt["value_px"] * k), txt, font=fB, fill=dim(C["text"]))
        tx2 = tx + g.textlength(txt, font=fB) + 14 * k
        fv = self.font("value", int(bt["volt_px"] * k))
        g.text((tx2, yb + ih / 2 - 0.60 * bt["volt_px"] * k), "%.1f V" % self.at(self.volt, ts), font=fv, fill=dim(C["muted"]))

        return img.resize((W, H), Image.LANCZOS)

    @staticmethod
    def _tracked_width(g, text, font, tracking):
        sz = font.size
        return sum(g.textlength(ch, font=font) for ch in text) + tracking * sz * (len(text) - 1)

    @staticmethod
    def _tracked(g, xy, text, font, fill, tracking):
        x, y = xy
        for ch in text:
            g.text((x, y), ch, font=font, fill=fill)
            x += g.textlength(ch, font=font) + tracking * font.size


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("npz")
    ap.add_argument("cameras")
    ap.add_argument("shot")
    ap.add_argument("out_dir")
    ap.add_argument("--size", default="1920x1080")
    ap.add_argument("--over", help="composite over the rendered frames in this directory")
    ap.add_argument("--frames", help="A:B, a frame range (default: the whole shot)")
    ap.add_argument("--spec", default=os.path.join(HERE, "hud_spec.json"))
    a = ap.parse_args()
    spec = json.load(open(a.spec))
    hud = Hud(spec, np.load(a.npz))
    cams = json.load(open(a.cameras))
    shot = [s for s in cams["shots"] if s["name"] == a.shot][0]
    f0, f1 = shot["start"], shot["end"]
    if a.frames:
        f0, f1 = (int(v) for v in a.frames.split(":"))
    os.makedirs(a.out_dir, exist_ok=True)
    W, H = (int(v) for v in a.size.split("x"))
    under = {}
    if a.over:
        for p in glob.glob(os.path.join(a.over, "*")):
            try:
                under[int(os.path.basename(p).split(".")[-2])] = p
            except ValueError:
                pass
    n = 0
    for f in range(f0, f1):
        if a.over and f not in under:
            continue
        ts = shot["replay_offset"] + f / cams["fps"] * shot["replay_rate"]
        if a.over:
            base = Image.open(under[f]).convert("RGBA")
            ov = hud.draw(base.size, ts)
            Image.alpha_composite(base, ov).convert("RGB").save(os.path.join(a.out_dir, "hud.%04d.jpg" % f), quality=92)
        else:
            hud.draw((W, H), ts).save(os.path.join(a.out_dir, "hud.%04d.png" % f))
        n += 1
    print("%d frames -> %s" % (n, a.out_dir))


if __name__ == "__main__":
    main()
