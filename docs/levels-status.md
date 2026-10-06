# Levels — status

Owner: levels track (session overboard-d5). Branch: `feat/game/levels`.
Updated at the end of each work block. The plan is in `docs/levels-plan.md`.

## State (2026-10-05)

| Item | State | Evidence |
|---|---|---|
| Peer agreements (c4, c5, overboard-14) | Done | `docs/levels-plan.md`, table "Agreements" |
| Pipeline design | Reviewed by the oracle | `docs/levels-plan.md`, section "Pipeline" |
| Level 1 layout | Proposed | `tools/levels/parking_lot_layout.py`; lap 526 m closes to 0.00 m |
| Level 2 route | Proposed (loop A, 1 413 m) | OSM data, own route tool; map on the lab server |
| sim-host loader (spawn, bounds, boxes) | Done by c4 | merged to controls master cd7962d (PR #298) |
| Kerb ride test | Done by c4 | 4 cm rides over; 8 and 15 cm: nose strike at 2 m/s. Heightfield drops of 0.10/0.12/0.15 m at 2-3 m/s: no fall (tail drag only). Box plank (0.12 m): off the end at 2-3 m/s, no fall; off a side at 10 deg: no fall at 2 m/s, nose-bumper fall at 3 m/s. Decision: keep the box plank (a real skill); the demo rides it centred at <= 2.5 m/s |
| Plan | Approved by Mike, 2026-10-05 | route B, Ramp B 15/20 %, kerb cut, box plank |
| Level 1 exporter | Done | `tools/levels/export_level.py parking_lot` -> course dir + `tools/play/elements/parking_lot.json` |
| Level 1 axes | Proved in MuJoCo | spawn yaw 180 = +X; the first hump acts at x = -22 m (row/col correct) |
| Level 1 ridden headless | 2 clean laps, 174.4 s / 172.2 s; earlier 3/3 clean (Ramp B 3/3) | `tools/levels/pilot_lap.sh parking_lot --laps 2` (controls cd7962d) |
| Launcher `--level` | Done | `tools/play/levels/<name>.env`; run_sim / play / make_demo_video |
| Game rules 2D + laps, DemoRider 2D | In progress | branch feat/game/laps-2d (engineer); PR to overboard-14 |
| OB_ParkingLot Unreal level | Merged (PR #43) | worst ground gap 2 mm, pole NE correct; city surround, clear markings, grey asphalt, sunlit lot (3 visual passes) |
| Level 2 phase A data | Done | branch feat/game/embarcadero, cfc023a: route B 1.94 km, 4601 x 8601 posts, 1302 kerb + 94 rail boxes, 2 kerb cuts |
| Level 2 ridden headless | 1 clean loop, 443.9 s (7.4 min: route B is longer than the brief's 3-5 min, by Mike's choice) | `LEVEL_DIR=... tools/levels/pilot_lap.sh embarcadero` |
| Level 2 phase B (moving objects) | Works headless on c4's 7fc08dc (not pushed yet; c4 asks Mike) | 48 objects; the pilot gives way at both crossings; 1 clean loop 469.5 s |
| OB_Embarcadero Unreal level | Next | Landscape from the .bin + OBM on the ridden surfaces + City Sample buildings on OSM footprints |

## Worktrees

- Game: `~/projects/overboard-game-levels` (feat/game/levels from overboard-game master).
- Controls, read-only build: `~/projects/overboard-levels-controls` (detached at origin/master cd7962d, which has the levels loader).

## Facts found (controls cd7962d)

- Turn law (c4): kappa = stick * min(0.25, 0.6 g/v^2) * clamp((v - 0.8)/2.2, 0, 1). Below 3 m/s
  the turn fades: R 8.6 / 6.0 / 4.7 m at 1.8 / 2.2 / 2.6 m/s (measured, `turn_test.py`).
  Slower is NOT tighter. Level 1 turns are R 7 m at <= 3.6 m/s.
- A 7.5 cm x 0.9 m bump: pitch +-11 deg, 90 A. A 5 cm x 1.8 m hump: +-1.9 deg (kept).
- Box edges overstate small features (rigid tyre): a 15 mm box bar row caused a fall. Rule
  (c4): features under 3 cm in the heightfield; boxes for kerbs of 4 cm and up, and objects.
- Kerbs at 2 m/s: 4 cm rides over; 8 and 15 cm: nose strike (c4).
- Box plank 0.12 m: off the end OK at 2-3 m/s; off a side at 3 m/s = fall (c4). Kept.
- A balancing board integrates the lean (lean = acceleration), so a demo speed loop must be P +
  damping; an integral term overshoots by 1.5-2 m/s. Brake with a higher gain (0.30) than you
  accelerate (0.20): a late corner entry cuts the corner.
- Level 2 kerbs (OSM-derived): the bike line must be 2 m from the kerb, and kerb corners need a
  radius (6 m closing) or a right turn hits the inner corner. Kerb cuts need flared sides.
- Each ride on any level overwrites `/tmp/overboard-ride.csv` (the game's ride log).

## Next

1. Game PR to overboard-14: 2D elements + laps in RideCourseElements and the HUD; 2D DemoRider
   (the headless pilot's laws, with the full turn law).
2. OB_ParkingLot: ground from `course_height.npy`, boxes, markings, lights.
3. Demo lap video in the game; publish to the lab server.
