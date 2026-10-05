# OB_Trail: a wooded valley level from a MuJoCo course

`Content/Maps/OB_Trail.umap` is a blog-render level. The scripts in `tools/trail/` generate it from
an authored MuJoCo course: a bike path that goes down into a wooded valley, across a flat floor
and a small timber bridge over a creek, and up the other side. The look is golden hour. The level
does not use City Park. It computes no physics: the board pose comes only from MuJoCo.

## Build and render

```
tools/trail/build_trail.sh <course_dir> [track.npz]      # about 10 minutes
tools/trail/render_trail.sh Still                         # one 1080p still per shot
tools/trail/render_trail.sh Look T2_chase                 # a fast 960x540 look-dev frame
```

- `<course_dir>` is an overboard-viz carve-lab course directory, for example
  `.../overboard-viz/out/carve-lab/data/courses/valley_gentle`.
- Without `track.npz` the script makes a PLACEHOLDER track (`gen_track.py`): a straight run down
  the path centre at 3 m/s, with the poses on the course surface. It is for scene and camera checks
  only. Give the MuJoCo run on the same course as the second argument when it exists.
- Work files go to `/tmp/ob-trail/`. Stills go to `/tmp/ob-trail/frames/MRQ_Still_<shot>/`.
- `OB_LOOK='{"ev100": 11.8, "sun_yaw": 20}'` overrides the look values (see `LOOK_DEFAULTS` in
  `build_trail_level.py`). `OB_TRAIL_ONLY=look` with `ue.sh build_trail_level.py` re-lights the saved
  level without a full rebuild.
- `OB_CONTACT_CHECK=1` adds three wheel-contact close-ups (`C1_descent`, `C2_bridge`, `C3_climb`).
- `OB_EXTRA` sets the UE arguments for the rider. The default is `-ObRenderRider`. With the sun at
  60 000 lux, rider lights need `-ObRiderLights -ObKeyCd=20000 -ObRimCd=40000`.

## The steps

| Step | Script | Runs in | Does |
|---|---|---|---|
| 1 | `gen_course.py` | numpy | Landscape heights, paint layers, the asphalt, bridge and water meshes, the backdrop hills, and every scatter transform |
| 2 | `gen_foliage.py` | numpy | Grass, wildflower, fern and reed meshes (true blade geometry for Nanite) |
| 3 | `gen_track.py` | numpy | The placeholder track (npz and wire-v3 `.bin`) |
| 4 | `tools/render/plan_cameras.py --shots trail` | numpy | The camera keys from the track |
| 5 | `build_trail_level.py` | editor, offscreen (`ue.sh`) | Materials, meshes, landscapes, dressing, look, sequence, MRQ configs |
| 6 | `verify_trail.py` | editor, offscreen | The tyre against the surface along the track |

The editor steps run in the full editor with `-RenderOffscreen`, because a commandlet compiles no
material shaders and so cannot report a material compile error. `ue.sh` waits for the result file
and stops the editor if it hangs while it shuts down.

Two C++ helpers make this possible (`Source/OverboardGame/*/Trail*`):

- `UTrailBuildLibrary::ImportLandscapeFromRaw` creates a landscape from raw 16-bit heights and 8-bit
  layer weights, as the editor's import tool does. Python in 5.7 cannot do this.
- `UTrailBuildLibrary::CreateStaticMeshFromObm` makes a static mesh from an OBM1 file
  (`tools/trail/obm.py`). No importer is in the path, so no axis or unit conversion can move a
  vertex.
- `ATrailScatterActor` holds the dressing: one instanced component per mesh, static (HISM) or
  skinned (the Nanite foliage trees).

## The course is the shape

- Frames: ABoardActor maps MuJoCo `(x, y, z)` m to Unreal `(100x, -100y, 100z)` cm, then applies the
  PlayerStart yaw and location. OB_Trail puts the PlayerStart `OB_TrailOrigin` at the world origin
  with yaw 0. The landscape centre vertex is the MuJoCo origin.
- The main landscape is 2033 x 2033 vertices at 0.2 m (8 x 8 components, 2 x 2 sections of 127
  quads), +-203.2 m. Every vertex inside the course square is a course post (every fourth one), and
  the Z scale is 40 (a 3.1 mm step). The ridden surface is a profile in x plus a crown in y, so the
  0.2 m grid holds it to the 16-bit step. Measured: 1.6 mm worst on the path and the verge.
- The asphalt is a separate mesh, 5 m wide, on the same grid, 3 mm above the landscape. Its UVs
  carry the worn edge lines and the dashed centre line.
- The landscape differs from the course in two places only. Both are outside the ridden surface.
  1. The creek and its pond are cut into the valley floor and the +y wall. Under the path the cut
     is covered by the bridge deck. The deck is flat at the mean path height over the span, so the
     tyre is within 7.5 mm of the course there.
  2. Outside the course square the valley continues along x, and hills rise beyond the walls. A
     coarse backdrop landscape (10 m posts, +-5 km) sits under the main one and carries the far
     hills.
- `verify_trail.py` maps each track sample as ABoardActor does and traces down to the landscape.
  It writes `/tmp/ob-trail/verify.txt`. The generated meshes have no physics in the editor scene,
  so over the bridge the reference is the deck height in `meta.json`.

## Dressing and its sources

| Element | Source | Where it comes from |
|---|---|---|
| Canopy and young trees | European quaking aspen, 4 variants | PVE sample content, `Engine/Plugins/Experimental/ProceduralVegetationEditor` |
| Shrub layer | Common hazel, 4 variants | Same |
| Dead snags (2 %) | City Sample birch and alder (bare) | Fab vault, `Content/Prop/` (gitignored) |
| Rocks | Megascans mossy rock | City Sample, `Content/Megascans/` (gitignored) |
| Grass, flowers, ferns, reeds | Generated by `gen_foliage.py` | This repo |
| Ground | Megascans asphalt, gravel, sand, mossy rock; City Sample dirt | City Sample |
| Bridge | Generated deck, stringers, abutments, railing | Megascans wood planks and cast concrete |
| Water | Single-layer-water material, engine `water_n` normal map | Engine content |

- The aspen and hazel are Nanite skinned meshes whose leaves are Nanite assembly parts. They need
  `r.Nanite.Foliage=1` (in `Config/DefaultEngine.ini`) and the ProceduralVegetationEditor plugin.
  Without the setting they render as bare trunks.
- The scatter rules are in `gen_course.py`. No instance comes within the 1.5 m verge of the asphalt:
  the rule tests each instance's footprint radius times its scale. The canopy aspens are the only
  exception. Their crowns can hang over the path, as real crowns do, and their trunks keep 5 m.
- The placement is procedural (numpy), not PCG. It runs outside Unreal, so a new course
  regenerates the level from one command, and the verge rule can be checked on the numbers.

## Copy the third-party content in first

```
tools/metahuman/copy_vault_closure.py --copy \
  /Game/Prop/Kit_Tree_Birch/Mesh/SM_Tree_Birch_a /Game/Prop/Kit_Tree_Birch/Mesh/SM_Tree_Birch_c \
  /Game/Prop/Kit_Tree_Alder/Mesh/Tree_Alder_B \
  /Game/Megascans/3D_Assets/Mossy_Rock_ulldfii/S_Mossy_Rock_ulldfii_lod3_Var1 \
  /Game/Megascans/Surfaces/Wooden_Planks_tixjedrbw/Wooden_Planks_tixjedrbw_2x2_M_00_inst \
  /Game/Megascans/Surfaces/Cast_In_Situ_Concrete_Wall_vcfice0/Asphalt_Road_2x2_M_01/th5ldh0cw_8K_Albedo \
  /Game/Megascans/Surfaces/Cast_In_Situ_Concrete_Wall_vcfice0/Asphalt_Road_2x2_M_01/th5ldh0cw_8K_Normal \
  /Game/Megascans/Surfaces/Cast_In_Situ_Concrete_Wall_vcfice0/Asphalt_Road_2x2_M_01/th5ldh0cw_8K_Roughness \
  /Game/Megascans/Surfaces/Concrete_Castinsitu_uflnbcofw/uflnbcofw_8K_Albedo \
  /Game/Megascans/Surfaces/Concrete_Castinsitu_uflnbcofw/uflnbcofw_8K_Normal \
  /Game/Megascans/Surfaces/Concrete_Castinsitu_uflnbcofw/uflnbcofw_8K_Roughness \
  /Game/Megascans/Surfaces/sand_sand_pjuuP0/pjuuP_4K_Albedo /Game/Megascans/Surfaces/sand_sand_pjuuP0/pjuuP_4K_Normal \
  /Game/Megascans/Surfaces/sand_sand_pjuuP0/pjuuP_4K_Roughness \
  /Game/Megascans/Surfaces/Gravel_Pebbledash_ugzmbcrn_2K_surface_ms/ugzmbcrn_2K_Albedo \
  /Game/Megascans/Surfaces/Gravel_Pebbledash_ugzmbcrn_2K_surface_ms/ugzmbcrn_2K_Normal \
  /Game/Megascans/Surfaces/Gravel_Pebbledash_ugzmbcrn_2K_surface_ms/ugzmbcrn_2K_Roughness \
  /Game/Prop/Kit_Dirt_A/Texture/ve0hedi_2K_Albedo /Game/Prop/Kit_Dirt_A/Texture/ve0hedi_2K_Normal \
  /Game/Prop/Kit_Dirt_A/Texture/ve0hedi_2K_Roughness
```

The MetaHuman skater and the board need their own content (see `carve-render.md`).

## Packs to add for a better level

No nature pack is in the local Fab or Epic vault cache (City Sample, City Park LITE and MonoWheel
Board only). Electric Dreams and Quixel Megascans nature assets are not on this Mac. These packs
would replace the generated or improvised parts:

- Megascans grass and wildflower clumps (meadow grass, oxeye daisy, buttercup): replace the
  generated blades.
- Megascans forest-floor surfaces (leaf litter, moss, forest soil) and a European beech or oak
  set: replace the City Sample dirt texture and widen the species mix.
- Megascans fallen logs, stumps and riverbed stones: the creek has one rock mesh only.
- Electric Dreams Environment (Fab, free): PCG forest assemblies and its foliage.

## Traps

- Megascans textures from City Sample are virtual textures. A sampler must be the Virtual type,
  or the material falls back to the default material with no log line in a commandlet.
- An unconnected Noise node already uses the world position. Its Position input does not accept
  a WorldPosition link from python.
- Manual exposure: with no physical camera, `auto_exposure_bias` is minus the scene's EV100.
  Auto exposure did not converge in the MRQ warm-up and gave white frames.
- The editor cannot delete a map it may hold. `build_trail.sh` deletes the map and the generated
  folders on disk before it starts the editor.
