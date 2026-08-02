# Attribution — Onewheel Pint preview model

## The work

**"OneWheel Pint" by maxime.montegnies (@aimix)** —
https://sketchfab.com/3d-models/onewheel-pint-cb822b5e535641f7ba23893a8f61b16e

Licensed **Creative Commons Attribution 4.0 International (CC BY 4.0)** —
https://creativecommons.org/licenses/by/4.0/

Published November 2019 on Sketchfab; the file in hand is a Blender 2.81 FBX export dated
2019-12-07, consistent with that.

The attribution above was not recoverable from the download itself — the FBX's `Author` and
`Creator` fields are empty and the only filesystem metadata was a Chrome quarantine flag with no
source URL. It came from Mike and was confirmed against the model page, which states the licence
as "CC Attribution" linking to the CC BY 4.0 deed.

## What is redistributed here

`.uasset` files derived from the original FBX and its texture set: three static meshes
(`OneWheelPint_prepared_OW_Frame`, `_OW_Wheel_Hub`, `_OW_Wheel_Tire`), their materials
(`Low_PlastickBlack`, `Material_001`, `Material_002`) and the textures those reference.
These are a **derivative work**, which CC BY 4.0 permits, with attribution and an indication
of changes — hence this file.

The raw Sketchfab download is **not** in this repo. `tools/pint/prepare_onewheel_fbx.py`
regenerates the prepared FBX from it in one Blender run.

## Changes made to the original (CC BY 4.0 §3(a)(1)(B))

The model was modified before import. Nothing was added to or removed from the vehicle geometry;
the changes are placement, scale and material wiring.

1. **Deleted the artist's display floor** — a 21 x 21 m ground plane (`Plane`, material `Floor`)
   included for presentation renders.
2. **Renamed** the three objects to `OW_Frame`, `OW_Wheel_Tire`, `OW_Wheel_Hub`.
3. **Moved the origin to the wheel axle**, which is where this project's pose data is anchored.
4. **Corrected a 23.54 deg rest-pose tilt.** The model shipped *posed*, leaning on one bumper the
   way a real onewheel sits when powered off. Measured from the deck's up-facing surface normals
   (68% of up-facing area at exactly -23.5 deg). Left uncorrected, the board renders permanently
   leaning and reads as a coordinate-transform bug.
5. **Uniformly scaled by 0.140973** so the tyre radius is 0.1454 m, matching the tyre radius in
   `overboard`'s `sim/models/overboard_onewheel.xml`. Anchored on tyre radius rather than deck
   length, because tyre-ground tangency is the most visible physical relationship in every frame.
   The original carries no real-world unit: it was 4.58 units nose-to-tail.
6. **Yawed 180 deg** so forward is -X, matching MuJoCo's convention for this vehicle.
7. **Bound two albedo maps that shipped unconnected.** `Material.001` (tyre) and `Material.002`
   (hub) had Base Color unlinked, sitting on Blender's Principled default of 0.8 grey — which is
   why both rendered white. `Tire-a.png` (solid black) and `WheelPlate-a.png` exist in the pack
   and were simply never wired. No colour was invented; the artist's own textures were connected.
8. **Marked the normal/roughness/metallic maps Non-Color**, and re-exported with smoothing groups
   and tangent space for Unreal.

## Trademark — NOT covered by CC BY

CC BY 4.0 **§2(b)(2) explicitly does not license trademark rights**, and the artist could not have
granted them regardless. **"Onewheel" and "Pint" are trademarks of Future Motion, Inc.**, and this
model reproduces both the `pint` wordmark on the deck and the Future Motion leaf logo on the hub.

A CC BY licence settles copyright. It does not settle trade dress or trademark. Using this model
to depict *this project's own board* — especially anywhere promotional, such as `overboard-web` —
is a materially different risk from using it as a development preview asset, and is not something
this NOTICE clears.

## Scope — this is a visual asset only

**Nothing here is used as physics.** `overboard`'s MuJoCo plant model continues to simulate the
MIT-licensed Openwheel geometry (`Meshes/openwheel/`, see that directory's `NOTICE.md`), unchanged.
This model has no metrological authority: it is an artist's approximation, not a measurement, and
is never a source for a mass, an inertia, a contact geometry or a control decision.

Consequently the rendered chassis is a Pint (~0.70 m deck) while the simulated one is
Openwheel-class (0.938 m). They are deliberately not the same vehicle, and footage using this
model may not be presented as a simulation result — see the "provenance category" rule in the
top-level `README.md`.

## Licensing posture

This repo's code is MIT. This directory is **CC BY 4.0** and is the exception, in the same way
`Meshes/openwheel/` carries its own MIT attribution. "100% MIT" is no longer accurate for assets
once this lands.

## Known defect

The tyre renders with a checkerboard artifact in Unreal that is not present in Blender. Material
slot binding, material graph contents, per-actor overrides, auto-exposure, texture streaming and
duplicate actors have each been ruled out by direct verification. Two renders with *different*
materials came out byte-identical, which indicates material edits are not reaching the renderer —
a caching problem in the `-game` capture path, not a problem with this model.
