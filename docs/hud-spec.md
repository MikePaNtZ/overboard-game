# The Overboard rider HUD

One HUD for the offline renders and the game. `tools/hud/hud_spec.json` holds every number:
layout, colours, fonts, thresholds and smoothing. `tools/hud/render_hud.py` is the reference
implementation (PIL, offline). The game HUD (`OverboardHUD`, a Canvas AHUD) draws the same thing
from the same file. If the two disagree, the JSON is right.

## What it shows

One panel, bottom left. Nothing else on screen.

| Row | Shows | Source (npz column / live wire) |
|---|---|---|
| Speed | A large numeral and "MPH" | `speed_mph` / wheel rate x 0.146 m, in mph |
| Torque | "TORQUE", the value and the limit ("+17 / 59 N·m"), and a centred bar | `torque_nm`, `torque_lim_nm` / Kt x motor current, Kt x current limit |
| Battery | An icon filled to the charge, the charge (one decimal), the pack voltage | `batt_soc`, `batt_v` |
| Warning | A chip at the top right of the panel | `flags`: bit 5 pulsed, bit 6 solid, bit 4 handoff |

- **Torque bar.** Zero is the centre tick, and each end is the motor limit. Drive fills to the
  right of zero and braking to the left, both teal: the side and the sign carry the direction.
  Above 85 % of the limit the fill is amber. Beyond the limit the fill pins at the end in red,
  a red cap shows past that end, and the value turns red.
- **Warning.** It is the same signal as the board's amber LEDs (L5). Pulsed: an amber "PUSHBACK"
  chip at 2 Hz, alpha 0.55 to 1. Solid: the same chip, steady. The panel edge takes the chip
  colour, 2 px.
- **Overboard.** On the handoff bit (Unreal owns the board) the chip reads "OVERBOARD" in red, the
  edge is red, and every value is muted at alpha 0.6: the controller no longer rides.
- **Authority margin.** It adds no element. As the authority used rises from 0.5 to 0.70 (the
  pulsed threshold), the panel edge warms from teal to amber, so the panel warns before the chip.

## Rules for an implementation

1. Sizes are pixels at a 1080-pixel-high frame. Multiply by frame height / 1080.
2. Fonts are Roboto from `Engine/Content/Slate/Fonts` (Light for the speed, Medium for labels,
   Regular for values). The minus sign is U+2212.
3. Smoothing is causal first-order (an exponential filter) with the `smooth_s` time constant of
   each row, so a live HUD can do exactly what the offline renderer does.
4. Draw order: panel fill and edge, speed, chip, torque label and value, bar track, fill, ticks,
   over-limit cap, battery.
5. Everything is a rounded rectangle, a rectangle or text, so a Canvas AHUD can draw it without UMG.

## Offline use

```
PY=/Users/mike/projects/overboard/.venv/bin/python
$PY tools/hud/render_hud.py <track.npz> <cameras.json> <shot> /tmp/hud_png            # transparent PNGs
$PY tools/hud/render_hud.py <track.npz> <cameras.json> <shot> /tmp/hud_jpg --over <frames_dir>
```

Frame f shows sim time `replay_offset + f / fps * replay_rate`, the clock the board replays on.
Composite the PNGs over the MRQ frames when you encode, for example with ffmpeg `overlay`.

## Honesty

The kinematic carve track's current is an estimate, not a sim result: its braking torque passes
the limit, and the HUD shows that in red. Its battery values are illustrative. The edge-case
tracks come from closed-loop MuJoCo runs, so their HUD values are sim values.
