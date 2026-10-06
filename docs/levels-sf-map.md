# OB_Embarcadero: Level 2, the San Francisco Embarcadero cruise loop

`Content/Maps/OB_Embarcadero.umap` is Level 2. The scripts in `tools/levels/embarcadero/` build it
from the exported `embarcadero` course (OSM + USGS DEM, staged to the grid frame). The level DRAWS
only what MuJoCo owns: the ridden corridor, the kerbs, the rails and the buildings are meshes
sampled from the exported data, so an exporter bug shows on screen. Unreal computes no board physics.

The route is a 1.94 km loop: Brannan Street (with a green bike lane), The Embarcadero promenade
along the bay, a racket turnaround at Bryant Street, and King Street past the ballpark corner.

## Build

```
tools/levels/embarcadero/build_embarcadero.sh [course_dir]      # ~15 minutes
```

The course directory is the exported course (`course_height.npy`, `metadata.json`, `level.json`,
`obstacles.csv`), default `~/projects/overboard-viz/out/carve-lab/data/courses/embarcadero`.
The build needs the OSM + DEM sources in
`~/projects/overboard-viz/out/carve-lab/data/sources/embarcadero/`.

Work files go to `/tmp/ob-levels-sf/`. Stills land in `/tmp/ob-levels-sf/sf_*.png`.

| Step | Script | Runs in | Does |
|---|---|---|---|
| 1 | `gen_embarcadero.py` | numpy + OSM (`~/.venvs/ob-levels`) | The ground mesh, the ridden corridor, the kerb/rail boxes, the water, the buildings, the markings and the dressing |
| 2 | `build_embarcadero.py` | editor, offscreen (`ue.sh`) | Materials, meshes, props, water, daylight, the map |
| 3 | `verify_embarcadero.py` | numpy | The drawn corridor vs course_height along the demo path |
| 4 | `stills_embarcadero.py` + `tools/render/render.sh` | editor, offscreen | Six review stills (a camera sequence + an MRQ still config) |
| 5 | `write_terrain.py` | python + C++ | The terrain declaration and the regenerated verification header |

Compile the C++ module once per worktree before the editor runs:

```
"/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/Mac/Build.sh" \
  OverboardGameEditor Mac Development -project="$PWD/OverboardGame.uproject"
```

## The course is the shape

- Frames: ABoardActor maps MuJoCo `(x, y, z)` m to Unreal `(100x, -100y, 100z)` cm. The PlayerStart
  sits at the world origin, yaw 0, so the MuJoCo origin is the world origin.
- The ground is `course_height.npy` downsampled to 2 m, 5 cm below the exact surface (a non-ridden
  backdrop; course_height raises building footprints by 3 m, so the ground mesh lowers those blocks
  back down). The ridden corridor (within 12 m of the demo path, clamped to the local curve radius
  so it never folds) is an exact mesh at course_height + 4 mm.
- Every `obstacles.csv` kerb is a concrete kerb box; every rail is a low railing box (z_m = box
  bottom). The buildings are the OSM footprints, extruded and grounded. The water plane covers the
  bay at -0.4 m.
- `verify_embarcadero.py` reads the corridor back and compares it with `course_height.npy` along the
  demo path (triangle interpolation). Target: worst gap under 10 mm (last run: 6.78 mm, 0 points over).

## Art folders (gitignored, licensed to Mike's Epic account; never commit)

The street lamp and the bench come from the City Sample Fab pack:

```
tools/metahuman/copy_vault_closure.py --copy \
  /Game/Prop/Kit_StreetLamp_B/Mesh/SM_StreetLamp_B \
  /Game/Prop/Kit_bench_RR/Mesh/SM_street_bench
```

The copied content (`Content/Prop/`, `Content/Material/`, `Content/Megascans/`) is gitignored. All
surface, building, water and marking materials are procedural, so the build needs no texture
content. The street trees and the Embarcadero palms are a green canopy proxy.

## Attribution

Map data (c) OpenStreetMap contributors, ODbL 1.0 (https://www.openstreetmap.org/copyright).
Elevation: USGS 3DEP 1 m DEM (CA_SanFrancisco_B23), public domain.

## What is committed

`Content/Maps/OB_Embarcadero.umap`, the generated `Content/Embarcadero/` (meshes, materials, the
still sequence; about 16 MB), `terrain/levels/OB_Embarcadero.terrain` and the regenerated
`Source/OverboardGame/Public/TerrainVerification.g.h`.
