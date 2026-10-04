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
| Left stick Y | Lean fore/aft. Dead zone 0.10, curve 0.5x + 0.5x³, no filter | `weight_shift_fore_aft` |
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

| Segment | Value | How |
|---|---|---|
| Pad → packet sent | ≤ 1 frame (~17 ms at 60 fps) | Enhanced Input and the send run in the same frame |
| Input → sim → state back | 4–13 ms (median ~9 ms) | `tools/play/latency_probe.py`, kick bit |
| State packet gap | p99 15.3 ms, max 20.9 ms | same probe; sim-host sends in bursts |
| Board actor render delay | 25 ms (was 50 ms) | `ABoardActor::RenderDelaySeconds` |
| Render + display | ~1–2 frames | estimate |
| **Stick to screen** | **~75 ms estimate** | over the 50 ms target |

Missed sim deadlines: sim-host misses about 70 % of its 2 ms deadlines on this Mac under load
(`--stats-path`: 14 936 of 20 943 ticks in one run, jitter p99 12.2 ms). The mean rate holds at
500 Hz, so the physics is right, but the packets come in bursts. A real-time thread class for the
sim loop (request to c4) would let the render delay go toward 10 ms.

## Contract assumptions

- StateOut v3, 104 bytes, on 127.0.0.1:9601. InputIn v1, 28 bytes, to 127.0.0.1:9602.
- StateOut flags: bit 0 armed, 1 valid, 2 fallen, 3 legacy authority warning (never set),
  4 handoff latch (cleared only by the input reset bit), 5 rider warning pulsed, 6 rider warning
  solid (never both; set only with `--authority-margin warn|limit`).
- `weight_shift_lateral` is sent as 0. Under `--lean-steer` the rider model sets the lateral
  ballast from `steer` (controls track, c4).
- sim-host zeroes an input older than 100 ms. The game sends one packet per frame.
- After a strike, bit 4 latches and Unreal owns the board (ADR-0012 ragdoll). The game does not
  auto-reset during a handoff; the player presses Circle.
- The PlayerStart is the MuJoCo origin, not the spawn point (c5). The sim spawns the board at
  x = 88 m (course s = 2 m; s = 90 − x).
- sim-host command and flags: `tools/play/run_sim.sh` (confirmed with c4). It needs sim-host
  from `feat/controls/downhill-carve` at db8dfbc or later.

## Mac facts

- UE 5.7 reads a DualSense through Apple's GameController framework (`GCDualSenseGamepad`).
  Options = Apple `buttonMenu` = `Gamepad_Special_Right`.
- UE 5.7's Mac force feedback is an empty function (`AppleControllerInterface.h:87`). Unreal
  rumble never reaches a pad on a Mac. `FPadRumble` drives `GCController.haptics` directly.
- A wired Xbox 360 pad does not work on Apple Silicon (no driver).

## Open faults and next

- OB_CityHill needs gitignored City Sample art in `Content/` (docs/city-level.md lists the
  folders). Known render-track state: plain facades, dress shoes on the rider.
- c4: the deployed balance law cannot hold speed on grades above about 5 %. A fix is in work.
  Until it lands, the 15 % street feels wrong. Do not tune around it in the game.
- sim-host misses many 2 ms deadlines on this Mac under load (about 70 %); the mean rate holds
  at 500 Hz, but packets arrive in bursts (p99 gap about 15 ms).
- Rumble is built but not yet felt on a real DualSense.
- Next: the shared HUD spec (in progress), the game HUD panel (time, score, toasts), ride check.
