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

## Launch (today)

1. `~/projects/overboard-game-play/tools/play/run_sim.sh`
2. Open `~/projects/overboard-game-play/OverboardGame.uproject` in UE 5.7.
3. Open the level and press Play. Press Cross to arm.

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

- The San Francisco street level is `OB_CityHill` (c5). It is not on master yet.
  `OB_City` on master is the older City Park level and does not match the city_hill ground.
- c4: the deployed balance law cannot hold speed on grades above about 5 %. A fix is in work.
  Until it lands, the 15 % street feels wrong. Do not tune around it in the game.
- sim-host misses many 2 ms deadlines on this Mac under load (about 70 %); the mean rate holds
  at 500 Hz, but packets arrive in bursts (p99 gap about 15 ms).
- Next: the shared HUD spec, the game elements on the street, latency measurement, ride check.
