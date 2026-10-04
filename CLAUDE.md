# overboard-game — project notes

Unreal 5.7 client for the Overboard sim: City Park level, rider, and the UDP wire to `sim-host`
(state out on 127.0.0.1:9601, input in on :9602). MuJoCo computes the board motion.

Earlier process rules and role files were archived on 2026-10-03 (git tag
`archive/pre-reset-2026-10-03`). They are historical and do not bind any work.

## Build
`"/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/Mac/Build.sh" OverboardGameEditor Mac Development -Project=<abs path>/OverboardGame.uproject`

## Content you must add yourself
`Content/CityPark/` and `Content/Mannequins/` are gitignored Fab packs. Never commit them.
See `docs/citypark-level.md`.

## Engine tips
- `UnrealEditor <project> <map> -game` needs the full package path (`/Game/Maps/OB_City`).
  The short name starts, writes no log, and exits after ~75 s.
- `FObjectFinder` asserts outside a constructor. Use `LoadObject` in `BeginPlay`.
- `GetMapName()` has a PIE prefix (`UEDPIE_0_`). Strip it with `UWorld::RemovePIEPrefix`.
- Asset loads can fail silently. Log loudly when a load returns null.
- `screencapture` needs Screen Recording permission. Use the in-engine shot (`-ObAuthorityShot`).
- Pace a UDP sender against an absolute clock, not `sleep_for()` in a loop (macOS drifts to ~8 Hz).
- Park geometry for MuJoCo: `tools/terrain_probe/dump_triangles.py` (editor python, read-only),
  then `rasterize_hfield.py`. Never save the level from those scripts.
