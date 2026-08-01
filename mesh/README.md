# mesh/ — the STL loader, standalone

Plain, engine-free C++17 (same reasoning as `../wire/`): prove it against the real files with
`clang++` before wiring it into Unreal. UE 5.7 has no built-in STL import factory — checked
directly against the installed engine (`Engine/Plugins`, `Engine/Source`): nothing registers an
STL importer. Rather than depend on a conversion tool (Blender/assimp) or fight that gap, this
project parses STL triangles directly and hands them to a `UProceduralMeshComponent` at runtime.
No `.uasset` import step, no editor GUI required to get geometry on screen.

## Layout
- `StlLoader.h` / `.cpp` — binary STL parser. Fails loudly (returns false + an error string) on
  a short/truncated file or a triangle count that doesn't match the file size (this is also what
  rejects an ASCII STL, without a separate format-sniffing heuristic — see the header comment).
- `tests/test_stl_loader.cpp` — runs against the real mesh files this project ships
  (`../Meshes/openwheel/*.stl`), not synthetic data. Checks every part parses, and cross-checks
  the parsed bounding boxes against specific facts recorded in `overboard`'s
  `sim/models/overboard_onewheel.xml` (front_enclosure spans x = -431.8..-145.4mm; front/rear
  enclosures mirror across X=0 with a 290.8mm gap = 145.4mm tire radius). If this ever fails, it
  means this copy of the STL has drifted from the one MuJoCo actually simulates.

## Build & run
```
cd mesh
make test
```

## The coordinate handling UE actually needs (not in StlLoader itself)
`overboard_onewheel.xml` states the meshes are authored in a shared assembly frame centred on the
wheel axle, mesh X/Y/Z already aligned with the body's fore-aft/lateral/up axes, mesh scale
0.001 (mm -> m). For Unreal (mm -> cm is x0.1) this means, in `ABoardActor`:
- All parts import at the actor's local origin, zero relative offset -- no per-part hand-placement.
- **Local vertices need the same Y-mirror as world positions/quaternions**
  (`wire/CoordinateTransform.h`) -- MuJoCo's local Y is "left", Unreal's local Y is "right", and
  the actor's local mesh space is effectively "world space at identity rotation", so the same
  mirror applies. Negating Y also reverses triangle winding, so vertex order gets swapped to
  keep faces front-facing -- see `ABoardActor`'s STL-to-ProceduralMesh conversion. (All seven of
  these particular parts are bilaterally symmetric, so getting this wrong would not have been
  visually detectable on this specific mesh set -- it's still done correctly rather than left as
  a landmine for the first asymmetric part someone adds.)
- `front_footpad.stl` ships authored at the *rear* location (confirmed by the loader's own
  bounding-box output, not just the XML comment) and needs an extra 180 degree yaw to reach the
  front -- matching the MJCF's `euler="0 0 180"` on that one geom.
- The wheel/tyre is **not** an STL -- it's a MuJoCo primitive cylinder, radius 145.4mm, width
  150mm. Built from `/Engine/BasicShapes/Cylinder.Cylinder`, scaled to those dimensions.

## Licensing
`../Meshes/openwheel/` carries `LICENSE` and `NOTICE.md` verbatim from
`overboard/sim/models/meshes/openwheel/` (byte-identical, verified via `shasum -a 256` at copy
time). Do not paraphrase or re-litigate `NOTICE.md`'s README/LICENSE discrepancy note -- it
travels as-is so the reasoning is intact in this second public repo. See the top-level README.
