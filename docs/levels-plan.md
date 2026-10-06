# Levels plan — parking-lot circuit and SF cruise loop

Owner: levels track (session overboard-d5). Branch: `feat/game/levels`.
Status: APPROVED by Mike, 2026-10-05. Decisions: Level 2 = route B (waterfront run to Bryant
Street and back); Ramp B = 15 % up / 20 % down; kerb lesson = use the kerb cut; plank = box
(levels recommendation).

## Rule

sim-host (MuJoCo) computes all board physics, and it owns the terrain and every object that the
board can hit. Unreal draws them. The coupling is the UDP wire and the shared files that sim-host
loads (heightfield + metadata.json, obstacles CSV). No wire change in this plan.

## Agreements with the peer tracks (2026-10-05)

| Peer | Agreement |
|---|---|
| c4 (controls) | sim-host adds `spawn {x,y,yaw_deg}`, `bounds` (enforced with `--terrain` too), rectangular grids (`half_extent_x_m`/`_y_m`), `--spawn-y`/`--spawn-yaw`, CSV `box` type with pitch, roll and `z_m`. Reset returns to the spawn pose. Grid limit about 50 M posts. Levels owns the exporter; c4 reviews the formats. |
| c4 (controls) | The tyre contacts fixed boxes directly (the wheel plate does not interfere). So kerbs, the plank and rumble bars are boxes. c4 sends a ride-over test result before the Level 2 kerb design is final. |
| c5 (render) | Levels writes `tools/levels/`. It imports only the pure `gen_city.py` helpers and `tools/trail/obm.py`. Big ground = Landscape + exact OBM meshes on the ridden surfaces (as OB_Trail). City Sample art is copied read-only from overboard-game-render. |
| overboard-14 (game) | Levels extends `RideCourseElements` + `gen_elements.py` (2D x,y,heading; checkpoints; laps; best lap; missed list), adds `--level` to `play.sh` with `tools/play/levels/<name>.env`, and makes `DemoRider` a 2D pure-pursuit follower on a `demo_path`. All by PR that overboard-14 reviews. city_hill stays unchanged. |

## Pipeline (reviewed by the oracle)

1. One source per level: a Python layout module writes `level.json` (grid spec, surfaces,
   boxes, elements, checkpoints, `demo_path`).
2. The exporter writes `course_hfield.bin` + `metadata.json` (spawn, bounds) + `obstacles.csv`
   + the elements json.
3. Surfaces with a slope under about 30° go in the heightfield (ramps, speed bumps, DEM ground).
   Steps and anything that the tyre must climb or fall off are boxes (kerbs, plank, rumble bars,
   rails, and every object to steer around).
4. Unreal draws the ground from the exported `.bin`, not from the layout primitives. Then the
   drawing equals what MuJoCo has, to the post, and an exporter bug is visible.
5. A non-symmetric marker in the first export proves the axes and the yaw sign in MuJoCo and in
   Unreal before any art work starts.
6. Level 1 proves the generic parts (2D spawn, bounds, laps, demo path). Level 2 adds an OSM
   front end. Phase B (moving objects) waits for a design that c4 agrees.

## Level 1 — parking-lot training circuit

Layout: https://mikes-macbook-pro.tail2cbb82.ts.net:8448/game/levels/level1_layout.png
Source: `tools/levels/parking_lot_layout.py`.

- Lot 124 x 84 m, kerb ring at the edge, grid 130 x 90 m at 0.05 m (2601 x 1801 posts).
- Lap 526 m, counter-clockwise, start/finish line on the south straight, 8 checkpoints.
- Estimated clean lap at 3-4 m/s: 2.2-2.9 min.

| # | Element | Size and grade | MuJoCo form |
|---|---|---|---|
| 1 | Speed bumps x3 | 0.075 m high, 0.9 m long, 10 m apart | heightfield (cosine) |
| 2 | Rumble bars | 20 bars, 15 mm high, 0.60 m pitch, 12 m | boxes |
| 3 | Ramp A | 5 % up 10 m (rise 0.50 m), deck 10 m, 10 % down 5 m | heightfield + rail boxes |
| 4 | Tight turn into the aisles | R 6 m | — |
| 5 | Cone slalom | 6 cones, 4 m apart, ±1.0 m | cone bodies |
| 6 | Hairpins x3 | R 6 m (at most 3 m/s, carve limit) | — |
| 7 | Plank | 12 m long, 0.60 m wide, 0.12 m high, 1.2 m entry ramp; ride off the end or the sides | box (gap under it) + heightfield entry. c4 test: a side drop at 3 m/s is a fall, at 2 m/s it is not |
| 8 | S-carves | painted line, ±1.5 m, 20 m wavelength | — |
| 9 | Kerb island | kerb up: a 4 cm rolled kerb, and a 10 cm kerb with a kerb-cut ramp; 10 m island; kerb down 0.10 m | boxes |
| 10 | Obstacles | cart, bin, pallet, ±1.2 m from the lane centre | boxes |
| 11 | Ramp B | 15 % up 4 m (rise 0.60 m), deck 8 m, 20 % down 3 m | heightfield + rail boxes |

Game: ordered checkpoints, lap counter, lap time, best lap, an optional target time (2:30),
missed checkpoints shown on the HUD. Read-only rules, as in `RideCourseElements`.

Acceptance:
- The demo rider rides a clean lap in 2-3 min on the pinned sim. Ramp B has its own number
  (clean in 3 of 3 runs), so one stall on Ramp B does not hide the rest of the lap.
- Each element is in MuJoCo and Unreal draws it where MuJoCo has it (worst tyre gap on the
  ground mesh at most 10 mm, as on OB_CityHill).
- A demo lap video, published to the lab server.

## Level 2 — SF Embarcadero cruise loop

Route map: https://mikes-macbook-pro.tail2cbb82.ts.net:8448/game/levels/level2_routes.png

Approved route: B, 1 932 m, about 6.4 min at 5 m/s (loop A + the waterfront run north to
Bryant Street and back, under the Bay Bridge). 2nd Street (protected bike lane) →
Brannan Street (bike lane) → The Embarcadero waterfront by South Beach Harbor → King Street
(bike lane, Oracle Park) → 2nd Street. Routes B and C add a waterfront run north to the Bay
Bridge and back; they are 1.9 km and 2.5 km, which is longer than the brief.

- Street layout from OpenStreetMap (ODbL; the level and the docs show the attribution).
  Elevation from the USGS 3DEP 1 m DEM (near flat in South Beach).
- Grid 440 x 880 m at 0.10 m (4401 x 8801 = 38.7 M posts, under the 50 M limit).
- The bay is visible: the promenade runs on the seawall. Unreal draws the water east of the
  seawall; MuJoCo has a rail box along the seawall edge, so the board cannot go into the bay.
- Kerbs (0.15 m) are boxes along the OSM kerb lines: a real step. c4 ride test (cecbd1d, 2 m/s):
  a 4 cm kerb rides over; at 8 cm and 15 cm the nose bumper hits the kerb face and the rider
  comes off. So a 0.15 m kerb is a real hazard, as on a Onewheel. Kerb ramps at the crossings
  let the rider get onto the sidewalk and off it.
- `bounds` is a crash guard only. The cruise has no score, so "off course" is not a rule.
- Unreal: a Landscape built from the `.bin`, exact OBM meshes on the streets, sidewalks and
  bike lanes, City Sample buildings along the OSM building footprints.
- Phase B: cars and pedestrians as MuJoCo bodies on scripted paths, sent on a new "objects"
  UDP channel (not StateOut). Design first, with c4's agreement.

## Open questions for Mike

0. Kerbs: the sim rider cannot ride up a kerb of 8 cm or more (no nose lift). Level 1 teaches
   "use the kerb cut" instead. Tell me if you want a different lesson.

1. (Answered: route B.)
2. (Answered: see the status line.)
