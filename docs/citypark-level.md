# OB_City — the working level

`Content/Maps/OB_City.umap` is the level this project actually drives in: Epic's **City Park
Environment Collection**, the board on the road, the real control loop over the wire. It is the
default startup and game map.

It is small, because it contains almost nothing: a GameMode override, one PlayerStart, and a
streaming reference to the park.

**No physics value changes here.** The wire, the controller and MuJoCo all keep working in
MuJoCo's own frame. The only things that moved are where MuJoCo's origin is *drawn* and which way
it faces — see "World origin" below. ADR-0009's rule is intact: this repo computes no physics.

`OB_Main` is deliberately kept. It is the **only level that works without any third-party
content**, and it is still the right level for coordinate-transform and handedness checks, where a
plain ground plane and no scenery is a feature.

## You need the pack first — it is not in this repo

`Content/CityPark/` is **gitignored and will never be committed.** The pack is licensed to an Epic
account, not to this repo, and this repo is public: the Fab EULA permits use in projects, not
redistribution of the raw assets. It is also 1.5 GB, against this estate's "never check binaries
into git" rule.

1. Install **City Park Environment Collection** from Fab into any local project.
2. Copy its `Content/CityPark/` folder here:
   ```
   rsync -a "<that project>/Content/CityPark/" Content/CityPark/
   ```
3. Open `Content/Maps/OB_City`.

Until you do, `OB_City` opens with unresolved references and no scenery. That failure is
deliberate and honest. Open `OB_Main` instead — it needs nothing.

**Copy it, do not open the pack's own project and migrate.** The pack ships as a UE 5.0EA project;
opening it in 5.7 converts it in place. A plain file copy leaves your install pristine and lets
5.7 upgrade our copy on load, which it does cleanly (verified: 458 actors, no errors).

## What is in the level

| Piece | Why |
|---|---|
| **GameMode override → `AOverboardGameMode_NoGround`** | Suppresses the 100 x 100 m placeholder plane, which would slice through the park, and the motion-reference markers, which a real environment does not need. It is also the only GameMode that reads the PlayerStart — see below. |
| **`/Game/CityPark/Maps/Showcase` as an always-loaded streaming sublevel** | The persistent level stores only the *path string*, never their geometry. That is what keeps `OB_City.umap` tiny and keeps us clear of redistributing the pack. |
| **One PlayerStart, `OB_BoardOrigin`** | Where MuJoCo's origin sits, and which way it faces. |

## World origin — position and yaw

The wire carries an **absolute** pose and `ABoardActor::UpdatePoseFromHistory` applies it with
`SetActorLocation`, so without an offset the board is pinned to UE `(0,0,0)` — the first packet
overwrites the spawn transform. Two knobs on `ABoardActor` fix that, both fed from the level's
first PlayerStart and both fed back nowhere:

- **`WorldOriginOffsetCm`** — where MuJoCo's origin lands.
- **`WorldOriginYawDeg`** — which way MuJoCo's +X points. Without it the board drives *across* the
  carriageway instead of along it, and no amount of moving the PlayerStart fixes that.

**Yaw only.** Pitch or roll would rotate the frame gravity is expressed in: the board would render
leaning and read as the controller failing to hold level when nothing of the sort had happened. A
hand-placed PlayerStart is rarely perfectly level, so its pitch and roll are discarded.

Order of operations is rotate-then-translate. The other way round swings the board around the
level origin on a lever arm as long as its distance from it — several hundred metres here.

### `bUsePlayerStartAsWorldOrigin` — default false, and that matters

`OB_Main` has a PlayerStart at **(0, 0, 92)** — the ordinary "lift a pawn clear of the floor"
offset every default level ships with. An earlier revision read the first PlayerStart
*unconditionally*, which silently raised the board 92 cm above `OB_Main`'s ground while claiming to
change nothing there.

A level now has to **opt in**, and only `AOverboardGameMode_NoGround` does. A PlayerStart's Z means
"where a pawn stands", not "where the ground is", and the difference between those is exactly the
amount that makes a board look like it is hovering.

## Where the board sits

`OB_BoardOrigin` is at **(-3880, -7450, -275.0), yaw 90** — on the road by the crosswalk, Mike's
pick. The road surface there measures **-275.0**, carrying 14,316 cm² of `MergedRoad2`.

**Measure ground by dominant surface area, not by maximum height.** Taking the highest horizontal
surface in a window returns the *kerb*, which put an earlier attempt 30 cm out. Ranking horizontal
triangle area per height bucket returns the road.

Line-tracing does not work for this: a commandlet editor world has no physics scene, so every trace
misses. The heights above come from geometry directly — `ProceduralMeshLibrary.get_section_from_static_mesh`
per `StaticMeshActor`, transformed to world, near-horizontal triangles binned by area.

To move the board: drag `OB_BoardOrigin`, press **End** to snap it to the surface, and save. Its
yaw sets the board's heading. No C++, no Blueprint.

## The board's appearance — a skin, not the simulated vehicle

`ABoardActor` renders the **Pint** model (`Content/ThirdParty/PintPreview/`, CC BY 4.0 — see that
directory's `NOTICE.md`) when `bUsePintSkin` is true, which is the default. The Openwheel
components are hidden, never destroyed; turning the flag off restores exactly what was there.

**MuJoCo still simulates Openwheel geometry, unchanged.** The rendered chassis is a ~0.70 m Pint;
the simulated one is a 0.938 m Openwheel-class board. They are deliberately not the same vehicle,
and footage using this skin may not be presented as a simulation result.

The cost is worth stating plainly: while the client rendered exactly what MuJoCo simulates, the
render was a free visual check on the coordinate transform — a board that floated or sank was a bug
signal. With a differently-proportioned chassis that signal is gone, because "wrong asset scale"
and "pose stream offset" now look identical. `bUsePintSkin=false` gets it back.

### Known defect

The Pint tyre renders with a checkerboard artifact in Unreal that is absent in Blender. Material
slot binding, material graph contents, per-actor overrides, auto-exposure, texture streaming and
duplicate actors have each been ruled out by direct verification. Two renders with *different*
materials came out byte-identical, which indicates material edits are not reaching the renderer —
a caching problem in the `-game` capture path, not a problem with the model.

## Reproducing the level from scratch

Built by a headless Python commandlet, not by hand. The order matters:

1. `new_level`, set the GameMode override, spawn the PlayerStart, then **save**.
2. **Only then** `add_level_to_world` for the sublevel, and save the persistent map by explicit
   path with `save_map(world, "/Game/Maps/OB_City")`.

`add_level_to_world` makes the level it adds **current**. Doing it earlier, or calling
`save_current_level()` afterwards, writes your PlayerStart into *City Park's own* `Showcase.umap`
and re-saves 465 MB of someone else's content. That happened once; the fix was restoring
`Content/CityPark/Maps/` from the pristine install and redoing it in the order above.

Two more traps, both of which cost a render each:

- **`unreal.Rotator` in Python is `(roll, pitch, yaw)`**, not C++'s `FRotator(Pitch, Yaw, Roll)`.
  Passing positionally aims the camera at the sky.
- **`les.new_level()` returns `False` if the map already exists** and does not raise. The script
  then happily edits whatever world was loaded. Assert on it.
