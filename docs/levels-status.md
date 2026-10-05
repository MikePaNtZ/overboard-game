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
| Build work | Level 1 exporter in progress | — |

## Worktrees

- Game: `~/projects/overboard-game-levels` (feat/game/levels from overboard-game master).
- Controls, read-only build: `~/projects/overboard-levels-controls` (detached at origin/master cd7962d, which has the levels loader).

## Next

2. Exporter + first `metadata.json` / `obstacles.csv` to c4 for review; axis marker test.
3. Game PRs to overboard-14 (after PR #38 merges): 2D elements + laps, `--level`, 2D demo rider.
