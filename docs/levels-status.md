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
| sim-host loader (spawn, bounds, boxes) | c4 builds it | follow-up commit on feat/controls/downhill-carve |
| Build work | Not started | waits for Mike's approval of the plan |

## Worktrees

- Game: `~/projects/overboard-game-levels` (feat/game/levels from overboard-game master).
- Controls, read-only build: `~/projects/overboard-levels-controls` (detached).

## Next

1. Mike approves the plan or changes it.
2. Exporter + first `metadata.json` / `obstacles.csv` to c4 for review; axis marker test.
3. Game PRs to overboard-14 (after PR #38 merges): 2D elements + laps, `--level`, 2D demo rider.
