# OB_ParkingLot: Level 1, a parking-lot training circuit

`Content/Maps/OB_ParkingLot.umap` is the playable Level 1. The scripts in `tools/levels/` build it
from the exported `parking_lot` course. The level DRAWS only what MuJoCo owns: the ground, every
obstacle and every marking are exact meshes sampled from the exported data, so an exporter bug
shows on screen. Unreal computes no board physics (HARD RULE).

The lap is a counter-clockwise circuit on a flat lot: a start straight with speed bumps and a
rumble strip, ramp A on the east side, a serpentine of four aisles (cone slalom, the plank, the
S-carve line, the kerb island), obstacles to steer around, and ramp B on the west side.

## Build

```
tools/levels/build_parking_lot.sh [course_dir]      # default: the parking_lot course; ~10 minutes
```

The course directory is the exported course (`course_height.npy`, `metadata.json`, `level.json`,
`obstacles.csv`), default
`~/projects/overboard-viz/out/carve-lab/data/courses/parking_lot`.

Work files go to `/tmp/ob-levels-map/` (`OB_LEVELS_WORK` to move them). Stills land in
`/tmp/ob-levels-map/pl_*.png`.

| Step | Script | Runs in | Does |
|---|---|---|---|
| 1 | `gen_parking_lot.py` | numpy | The ground tiles, the obstacle boxes, the cones and the markings, from the course |
| 2 | `build_parking_lot.py` | editor, offscreen (`ue.sh`) | Materials, meshes, props, dressing, daylight, the map |
| 3 | `verify_parking_lot.py` | numpy | The drawn ground vs the course height along the demo path |
| 4 | `stills_parking_lot.py` + `tools/render/render.sh` | editor, offscreen | The review stills (a camera sequence + an MRQ still config) |
| 5 | `write_terrain_decl.py` | python + C++ | The terrain declaration and the regenerated verification header |

The C++ module must be compiled once per worktree before the editor runs (it provides
`UTrailBuildLibrary::CreateStaticMeshFromObm`):

```
"/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/Mac/Build.sh" \
  OverboardGameEditor Mac Development -project="$PWD/OverboardGame.uproject"
```

## The course is the shape

- Frames: ABoardActor maps MuJoCo `(x, y, z)` m to Unreal `(100x, -100y, 100z)` cm. The PlayerStart
  `OB_ParkingLotOrigin` sits at the world origin with yaw 0, so the MuJoCo origin is the world origin.
- The ground is exact meshes sampled from `course_height.npy` at 0.10 m, split into about 25 m
  Nanite tiles: the asphalt lot, the two ramps (concrete), and the grass verge outside the kerb ring.
- Every `obstacles.csv` row is a box at its exact pose and size (kerbs, the island and the plank as
  boxes; the rails as metal boxes; the cones as cone meshes), except `pole_NE` (a street light with
  a point light, the axis marker in the NE corner) and the bin (a trash-can prop).
- The markings sit 3 mm above the ground: the lane edge lines, the parking stall lines, the
  start/finish checker, the S-carve line, the turn arrows and the ramp edge paint.
- `verify_parking_lot.py` reads the ground OBM back and compares it with `course_height.npy` along
  the demo path. Target: worst gap under 10 mm (last run: 2.0 mm). It also checks that `pole_NE`
  stands at UE (6000, -4000) cm.

## Art folders (gitignored, licensed to Mike's Epic account; never commit)

The props, buildings and surfaces come from the City Sample Fab pack, copied into `Content/` by
`tools/metahuman/copy_vault_closure.py`:

```
tools/metahuman/copy_vault_closure.py --copy \
  /Game/Prop/Kit_StreetLamp_B/Mesh/SM_StreetLamp_B \
  /Game/Prop/Kit_Trashcan_A/Mesh/SM_Trashcan_A_01 \
  /Game/Building/Library/Kit_Hero_Bldg/LevelInstance/BPP_Bldg_Hero_Mid_SFC_A01 \
  /Game/Building/Library/Kit_Hero_Bldg/LevelInstance/BPP_Bldg_Hero_Mid_SFC_B01 \
  /Game/Building/Library/Kit_Hero_Bldg/LevelInstance/BPP_Bldg_Hero_Low_SFD_Long_01 \
  /Game/Megascans/Surfaces/Cast_In_Situ_Concrete_Wall_vcfice0/Asphalt_Road_2x2_M_01/th5ldh0cw_8K_Albedo \
  /Game/Megascans/Surfaces/Concrete_Castinsitu_uflnbcofw/uflnbcofw_8K_Albedo
```

The copied content (`Content/Prop/`, `Content/Building/`, `Content/Megascans/`, `Content/Material/`)
is gitignored; never commit it. The lot asphalt and the ramp/kerb concrete use the Megascans
surfaces (world-space UVs). The box, cone, marking, grass and tree-canopy materials are procedural.
The buildings ring the lot 55 m outside the verge; they are packed level actors, so they render only
in the `-game` MRQ still, not in the headless editor. The verge trees are a green canopy proxy,
because the City Sample street trees are a bare-branch winter variant.

## What is committed

`Content/Maps/OB_ParkingLot.umap`, the generated `Content/ParkingLot/` (meshes, materials, the still
sequence), `terrain/levels/OB_ParkingLot.terrain` and the regenerated
`Source/OverboardGame/Public/TerrainVerification.g.h`. Any re-save of the map changes the terrain
`level_hash`, so `write_terrain_decl.py` runs last.
