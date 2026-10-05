#!/usr/bin/env python3
"""Write the rider-warning buzz as a WAV track for one rendered shot. Runs OUTSIDE Unreal.

usage: buzz_track.py <track.npz> <cameras.json> <shot> <out.wav>

The buzz is the sound a rider would feel through the deck: a low motor buzz (a 140 Hz square-ish
tone with a soft attack), pulsed at 2 Hz while flags bit 5 is set and steady while bit 6 is set,
the same signal as the board's amber LEDs and the HUD chip. Silence elsewhere. The track starts at
the shot's first rendered frame, on the replay clock (replay_offset + frame / fps * replay_rate).
"""
import json
import sys
import wave

import numpy as np

SR = 48000
F0 = 140.0
PULSE_HZ = 2.0


def main():
    npz, cams_path, shot_name, out = sys.argv[1:5]
    d = np.load(npz)
    t, flags = d["t"], d["flags"].astype(np.int64)
    cams = json.load(open(cams_path))
    shot = [s for s in cams["shots"] if s["name"] == shot_name][0]
    n_frames = shot["end"] - shot["start"]
    dur = n_frames / cams["fps"]
    ta = np.arange(int(dur * SR)) / SR                         # audio time from the first frame
    ts = shot["replay_offset"] + (shot["start"] / cams["fps"] + ta) * shot["replay_rate"]
    i = np.clip(np.searchsorted(t, ts) - 1, 0, len(t) - 1)
    fl = flags[i]
    solid = (fl & 64) != 0
    pulsed = ((fl & 32) != 0) & ~solid
    gate = np.where(solid, 1.0, np.where(pulsed, (np.mod(ts * PULSE_HZ, 1.0) < 0.5).astype(float), 0.0))
    # smooth the gate edges (8 ms) so the buzz has no clicks
    k = int(0.008 * SR)
    gate = np.convolve(gate, np.ones(k) / k, mode="same")
    tone = np.tanh(3.0 * np.sin(2 * np.pi * F0 * ta)) * 0.6 + 0.25 * np.sin(2 * np.pi * 2 * F0 * ta)
    y = 0.35 * gate * tone
    pcm = (np.clip(y, -1, 1) * 32767).astype("<i2")
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print("%s: %.2f s, buzz %.2f s" % (out, dur, float((gate > 0.5).sum()) / SR))


if __name__ == "__main__":
    main()
