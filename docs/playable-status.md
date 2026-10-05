# Playable city — status

Owner: game play track (session overboard-14). Branch: `feat/game/playable-city`.
Updated at the end of each work block.

## Rule

sim-host (MuJoCo) computes all board physics. Unreal sends inputs and draws the state.
The only coupling is the UDP wire. Nothing in this branch computes a board quantity.

## What works

| Item | State | Evidence |
|---|---|---|
| Editor build, UE 5.7, Mac | Works | `Build.sh OverboardGameEditor Mac Development` → Succeeded |
| sim-host live on city_hill | Works | 500 Hz, wire v3 parsed on all packets (wire-probe) |
| `--hold-until-arm` | Works | board stays at x = 88.000 m, wheel 0, until the first arm bit |
| PS5 DualSense mapping | Built | headless self-test: keyboard PASS, stick PASS, L2 PASS |
| Rumble (bits 5/6, fall jolt) | Built, not felt yet | Mac path uses Core Haptics directly (see below) |
| HUD cues (warning, fall prompt) | Built, interim | replaced by the shared HUD spec next |
| Camera cycle (Options / C) | Built | chase → close → high |
| OB_CityHill, default map | Works | built by `tools/city/build_city.sh` (verify PASS, worst tyre gap 7.8 mm); opens headless |
| Game elements | Built | 11 elements load and draw on OB_CityHill; scoring not yet ridden |
| Latency | Measured | see below |

## Controls (PS5 DualSense)

| Control | Action | Wire field |
|---|---|---|
| Left stick Y | Lean fore/aft: full stick moves the rider 10 cm (`--rider-reach 0.10`). Dead zone 0.10, curve 0.5x + 0.5x³, no filter | `weight_shift_fore_aft` |
| Right stick X | Carve intent (positive = right) | `steer` |
| L2 (analog) | Hard lean back (tail brake): `fore_aft = min(stick, −L2)` | `weight_shift_fore_aft` |
| Cross | Arm (releases the board) | input flag bit 0 |
| Circle | Reset | input flag bit 1 |
| Options | Camera cycle | — |

Keyboard: W/S lean, A/D steer, Left Shift tail brake, Space arm, R reset, C camera, Esc quit.
Only the keyboard path is ramped (`KeyboardRampSpeed` 3.0). The old code ramped the pad too,
which added about 0.33 s of lag.

## Launch

1. Connect the DualSense (USB-C, or Bluetooth: hold PS + Create, then pair).
2. `~/projects/overboard-game-play/tools/play/play.sh` — it starts sim-host, opens the editor on
   OB_CityHill, and stops sim-host when the editor closes. `--game` opens the game window only.
3. Press Play (editor), then Cross (or Space) to arm.

## Game elements (OB_CityHill)

Layout: `tools/play/elements/city_hill.json` (from `tools/play/gen_elements.py`). Read-only: the
actor reads the newest board sample and flags; it has no collision and no force.

| Element | Where (s from course start) | Rule | Points |
|---|---|---|---|
| START gate | 14 m | starts the timer | — |
| Slalom flags ×5 | 26–66 m, y = ±1.8 m | pass each flag on its outer side | 100 each |
| Stop box | 85–92 m | stop (< 0.3 m/s) inside; tail down (pitch > 0.25 rad) adds a bonus | 300 + 200 |
| Slow zone | 95–110 m | stay under 3 m/s | +200 clean, −100 too fast |
| SPLIT gate | 102 m | split time | — |
| No-buzz climb | 114–166 m | no rider warning (bits 5/6) on the 12 % climb | 300 |
| FINISH gate | 172 m | stops the timer | 500 |

## Latency (2026-10-04, this Mac)

With sim-host's real-time loop thread (controls 880a2b2):

| Segment | Value | How |
|---|---|---|
| Pad → packet sent | ≤ 1 frame (~17 ms at 60 fps, ~8 ms at 120 fps) | Enhanced Input and the send run in the same frame |
| Input → sim → state back | 4.5–9.8 ms (median 6.4 ms) | `tools/play/latency_probe.py`, kick bit |
| State packet gap | p99 5.6 ms, max 8.4 ms | same probe |
| Board actor render delay | 12 ms (was 50 ms) | `ABoardActor::RenderDelaySeconds` |
| Render + display | ~1–2 frames | estimate; not measured (needs a camera on the screen) |
| **Stick to screen** | **~60 ms at 60 fps, ~40 ms at 120 fps (estimate)** | the 50 ms target needs 120 fps |

Missed sim deadlines: 0 of 15 402 ticks (`--stats-path`, jitter p99 0). Before the real-time
fix: about 70 % missed, jitter p99 12 ms, packet gaps up to 21 ms.

## Demo video

`tools/play/make_demo_video.sh` records a live ride: the demo rider (`-ObDemoRider`) plays the pad,
and the game records its own viewport (`-ObRecordVideo`, 720p, 30 fps, VideoToolbox).
Take of 2026-10-04 (sim 400ee0f): 4/4 flags, 14.6 mph, tail stop, finish 82.5 s, 1 600 points,
96.5 % unique frames. Tailnet: https://mikes-macbook-pro.tail2cbb82.ts.net:8448/game/overboard-demo-full-2026-10-04.mp4

## Open faults and next

- Pull-away after a tail-brake stop (controls). `-ObDemoQuickRestart` 3-ride test:
  8add160: a nose strike in 3/3; e487142 (pad mode): 0 nose strikes. The remaining falls were
  (a) roll falls in the slalom from FULL steer at 3-4 m/s on 15 %: past the carve limit in every
  build (c4 reproduced it in 400ee0f), so the demo steers at most 0.6; and (b) a tilt back over
  the tail at a stop: the rigid rider model (an ankle model is a later controls job).
  The sim is pinned at e487142.
- A full L2 stop on the flat tips the board over the tail (rigid rider model; c4). Keep L2 <= 0.6.
- Only the handoff (bit 4) is a fall in the game; bit 2 is pitch past 20 deg (c4 8ef0a7d also
  stops setting it for a tail drag).

- OB_CityHill needs gitignored City Sample art in `Content/` (docs/city-level.md lists the
  folders). Known render-track state: plain facades, dress shoes on the rider.
- `--balance-comp` (grade compensation for the deployed law) is on in `run_sim.sh`. c4's 200-run
  test: 159 pass, 37 stall (mostly climbs of 15 % or more), 2 runaway, 2 nose strikes (0 with the
  warning). It is reversible: remove the flag if the hill feels wrong with it.
- Rumble is built but not yet felt on a real DualSense.
- Next: the shared HUD spec (in progress), the game HUD panel (time, score, toasts), ride check.
