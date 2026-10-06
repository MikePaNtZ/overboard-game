# Levels — status

Owner: levels track (session overboard-d5). Branch: `feat/game/levels`.
Updated at the end of each work block. The plan is in `docs/levels-plan.md`.

## State (2026-10-05)

| Item | State | Evidence |
|---|---|---|
| Peer agreements (c4, c5, overboard-14) | Done | `docs/levels-plan.md`, table "Agreements" |
| Pipeline design | Reviewed by the oracle | `docs/levels-plan.md`, section "Pipeline" |
| Level 1 layout | Final (ridden) | `tools/levels/parking_lot_layout.py`; lap 543 m; slalom cones at +-0.9 m |
| Level 2 route | Route B, 1.94 km (Mike's choice) | `tools/levels/embarcadero/route_stage.py` |
| sim-host loader (spawn, bounds, boxes) | Done by c4 | merged to controls master cd7962d (PR #298) |
| Kerb ride test | Done by c4 | 4 cm rides over; 8 and 15 cm: nose strike at 2 m/s. Heightfield drops of 0.10/0.12/0.15 m at 2-3 m/s: no fall (tail drag only). Box plank (0.12 m): off the end at 2-3 m/s, no fall; off a side at 10 deg: no fall at 2 m/s, nose-bumper fall at 3 m/s. Decision: keep the box plank (a real skill); the demo rides it centred at <= 2.5 m/s |
| Plan | Approved by Mike, 2026-10-05 | route B, Ramp B 15/20 %, kerb cut, box plank |
| Level 1 exporter | Done | `tools/levels/export_level.py parking_lot` -> course dir + `tools/play/elements/parking_lot.json` |
| Level 1 axes | Proved in MuJoCo | spawn yaw 180 = +X; the first hump acts at x = -22 m (row/col correct) |
| Level 1 ridden headless | 2 clean laps, 174.4 s / 172.2 s; earlier 3/3 clean (Ramp B 3/3) | `tools/levels/pilot_lap.sh parking_lot --laps 2` (controls cd7962d) |
| Launcher `--level` | Done | `tools/play/levels/<name>.env`; run_sim / play / make_demo_video |
| Game rules 2D + laps, HUD, DemoRider 2D | Merged (PR #47); gate labels face the rider, cruise toast "LOOP n m:ss" (this PR) | in-game demo on OB_ParkingLot, 2 runs x 2 laps: 172.4/168.8 s and 172.3/168.8 s, all CLEAN (sim cd7962d) |
| OB_ParkingLot Unreal level | Merged (PR #43); rebuilt for the moved cones (this PR) | worst ground gap 2 mm, pole NE correct; city surround, clear markings, grey asphalt, sunlit lot |
| Level 2 phase A data | Merged (PR #44) | 4601 x 8601 posts, 1302 kerb + 94 rail boxes, 2 kerb cuts, 2 crossings |
| Level 2 ridden headless | 1 clean loop, 443.9 s (7.4 min: route B is longer than the brief's 3-5 min, by Mike's choice) | `LEVEL_DIR=... tools/levels/pilot_lap.sh embarcadero` |
| Level 2 phase B (moving objects) | Works headless on c4's 7fc08dc (not pushed yet; c4 asks Mike) | 48 objects; the pilot gives way at both crossings; 1 clean loop 469.5 s |
| OB_Embarcadero Unreal level | Merged (PR #46); `--level embarcadero` merged (PR #51); game demo 1 loop 436.0 s CLEAN on PORT_BASE 19600 | corridor gap 3.1 mm; 1396 boxes exact (yaw sign fixed: UE yaw = -MuJoCo yaw); every building block covered. Facades interim procedural; City Sample building pass asked of c5 |
| Level 1 demo video | Published | https://mikes-macbook-pro.tail2cbb82.ts.net:8448/game/levels/overboard-parking-lot-demo-2026-10-06.mp4 (1 lap 172.3 s CLEAN; NoGround game mode, PR #50; full-frame recorder, PR #49) |
| Test ports | Agreed with overboard-14 (PR #49) | levels tests PORT_BASE=19600 (game -ObPortBase=19600); live play 9600; overboard-14 tests 29600 |

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

## Open: real facades for OB_Embarcadero (c5's suggested route, 2026-10-06, not yet proven)

1. Copy a City Sample SOURCE reference level (`/Game/Building/Library/Kit_Ref_Bldg/SFA_Ref_N1.umap`,
   `SFB_Ref_N1`, `SFJ_Ref`) and its closure (`tools/metahuman/copy_vault_closure.py`). There, every
   kit piece is a plain actor with an exact transform (packed BPP buildings cannot be measured
   headless).
2. Dump each actor (mesh path + transform). Cut ONE facade module: the pieces between two vertical
   grid lines of the street face, all floors, in a local frame at the street face.
3. Tile each OSM footprint edge with whole modules; fill the rest with a blank kit C wall; corner
   pieces at the vertices; ground each module at the lowest sidewalk height with a plinth.
Kit facts (c5): kit pieces are measurable static meshes; local -X is the street face; place with
yaw +-90. c5's measured table: `/tmp/ob-city/kit_bounds.json`. Who proves it first is Mike's call.
