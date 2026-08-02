# OB_City — the board in the City Park environment

`Content/Maps/OB_City.umap` puts the board in Epic's **City Park Environment Collection** instead
of the C++ placeholder plane. It is 9.7 KB, because it contains almost nothing: a GameMode
override, one PlayerStart, and a streaming reference to the park.

**None of this changes a single physics value.** The wire, the controller and MuJoCo all keep
working in MuJoCo's own frame. The only thing that moved is where MuJoCo's origin is *drawn* —
see "World origin offset" below. ADR-0009's rule is intact: this repo still computes no physics.

## You need the pack first — it is not in this repo

`Content/CityPark/` is **gitignored and will never be committed.** The pack is licensed to an Epic
account, not to this repo, and this repo is public: the Fab EULA permits use in projects, not
redistribution of the raw assets. It is also 1.5 GB, against this estate's "never check binaries
into git" rule.

So `OB_City` ships as a level that *references* content you supply. To get it:

1. Install **City Park Environment Collection** from Fab into any local project.
2. Copy its `Content/CityPark/` folder to `Content/CityPark/` here:
   ```
   rsync -a "<that project>/Content/CityPark/" Content/CityPark/
   ```
3. Open `Content/Maps/OB_City`.

Until you do, `OB_City` opens with unresolved references and no scenery. That failure is
deliberate and honest — better than a level that silently looks empty.

**Copy it, do not open the pack's own project and migrate.** The pack ships as a UE 5.0EA project;
opening it in 5.7 converts it in place. A plain file copy leaves your installed pack pristine and
lets 5.7 upgrade our copy on load, which it does cleanly (verified: `SM_MergedParkSquare01`,
`SM_Statue02` and the rest all build, 458 actors load, no errors).

## What is in the level

| Piece | Why |
|---|---|
| **GameMode override → `AOverboardGameMode_NoGround`** | Suppresses the 100 x 100 m placeholder plane, which would otherwise slice straight through the park, and the motion-reference markers, which a real environment does not need. The class already existed for exactly this. |
| **`/Game/CityPark/Maps/Showcase` as an always-loaded streaming sublevel** | The persistent level stores only the *path string*, never their geometry. That is what keeps `OB_City.umap` at 9.7 KB and keeps us clear of redistributing the pack. |
| **One PlayerStart, labelled `OB_BoardOrigin`** | Where MuJoCo's origin lands. See below. |

`OB_Main` is untouched and remains the startup map. Switch levels in the editor; do not change
`EditorStartupMap`, or the project stops opening for anyone without the pack.

## World origin offset — the part that actually needed code

The wire carries an **absolute** position and `ABoardActor::UpdatePoseFromHistory` applies it with
`SetActorLocation`. The board was therefore pinned to UE `(0,0,0)` no matter where it spawned —
the first packet overwrote the spawn transform. Fine over a placeholder plane centred on the
origin; useless in an imported environment, where the origin is wherever the environment author
happened to put it. In City Park's case it is not flat, not paved, and not somewhere you would
want to ride.

`ABoardActor::WorldOriginOffsetCm` is added after `MuJoCoToUnreal` and fed back nowhere.
`AOverboardGameMode::BeginPlay` sets it from the first `PlayerStart` in the level, if there is one,
and defaults to zero — so `OB_Main` and every previous capture are bit-identical to before.

Only the **position** is offset. Rotation is untouched: moving a level's origin must not be able
to roll the horizon.

To move the board somewhere else in the park, drag `OB_BoardOrigin` and save. No C++, no
Blueprint. If a level has several PlayerStarts the first wins and the Output Log names it.

## Why the board sits where it does

`OB_BoardOrigin` is at **(-66750, -11000, -168.3) cm**. That is a measured flat spot, not a guess.

Line-tracing the level headless finds nothing — a commandlet editor world has no physics scene, so
every trace misses. Instead the flat spot came from the geometry directly: read every
`StaticMeshActor`'s triangles via `ProceduralMeshLibrary.get_section_from_static_mesh`, transform
to world space, keep the near-horizontal ones (|n·z| > 0.999), bin their area into 5 m cells, and
keep cells that are >75% covered and span < 40 cm vertically. Then take the largest inscribed
rectangle of adjacent good cells.

The answer: **15 x 20 m, flat to 14.4 cm** (about 0.4°, i.e. ordinary road camber). Ground height
at that exact point is -168.3 cm.

Worth knowing: **there is no large flat plaza in this park.** Nothing anywhere in the level is a
20 x 20 m open flat square. The paved surface is all paths and roads, so the best available spot is
path-width. The runner-up is 15 x 15 m at (-40250, -35750), ground z = 113.4, flat to 12.2 cm —
marginally flatter, smaller, and elsewhere in the park.

Both numbers are **measured, not seen** — this pass had no display, same constraint as every other
geometry change in this repo. First person to press Play should confirm the board lands on the
path rather than in a hedge, and move `OB_BoardOrigin` if it looks wrong.

## Reproducing the level from scratch

The level was built by a headless Python commandlet, not by hand in the editor. If it is ever lost,
the order matters:

1. `new_level`, then set the GameMode override and spawn the PlayerStart, then **save**.
2. **Only then** `add_level_to_world` for the sublevel, and save the persistent map by explicit
   path with `save_map(world, "/Game/Maps/OB_City")`.

`add_level_to_world` makes the level it adds **current**. Doing it earlier, or calling
`save_current_level()` afterwards, writes your PlayerStart into *City Park's own* `Showcase.umap`
and re-saves 465 MB of someone else's content in 5.7 format. That happened once here; the fix was
to restore `Content/CityPark/Maps/` from the pristine install and redo it in the order above.
