# The rider — a stand-in, not a person

`ABoardActor` can draw the default UE mannequin ("Manny") standing on the deck when
`bShowRider` is true (the default). It rides along with the board's position and rotation for
free, because it is a child of the board's transform, and is nudged fore/aft and laterally by the
real simulated ballast displacement carried in wire v2's `rider_fore_aft_m` / `rider_lateral_m`.

**This is the CEO's explicit ask**, made after seeing an early capture: a board with nobody on it
does not read as being ridden, and a tall object shows lean and rotation far more legibly than a
low deck does.

## You need the asset first — it is not in this repo, and it ships with the engine

`Content/Mannequins/` is **gitignored and will never be committed.** It is Epic's UE 5.7 template
mannequin content — the mesh, skeleton, materials, textures and animations — licensed under the
UE EULA as template content, not licensed for redistribution in an arbitrary public repo, and this
repo is public. Same reasoning as `Content/CityPark/` (see `docs/citypark-level.md`), not
re-litigated here. It is also ~126 MB, well past this estate's "never check binaries into git"
rule on size alone.

It ships with every UE 5.7 install, so there is nothing to download:

```
rsync -a "/Users/Shared/Epic Games/UE_5.7/Templates/TemplateResources/High/Characters/Content/Mannequins/" Content/Mannequins/
```

Without it, `bShowRider` fails gracefully: no rider is drawn, the board renders exactly as before,
and the Output Log says so loudly (`ABoardActor: rider mesh/animation did not resolve...`). That
is the intended and honest failure mode — never a hard failure, per the standing rule that an
invisible/broken board is worse than a plain one.

## What is actually simulated, and what is not — read this before trusting the footage

**The physics has a rigid ballast on two slide joints, not a person.** It has no legs, no arms, no
articulation, no balance of its own. Manny is a costume on that lump, and this is already line 3
of the `Playable Sim` channel declaration: *"the rider is a rigid ballast, not a person."* Putting
a visible human on screen makes that line MORE important to hold onto, not less.

Consequences, stated plainly rather than left implicit:

- **The pose is invented.** `ABoardActor` plays a stock idle animation
  (`/Game/Mannequins/Anims/Unarmed/MM_Idle`) so Manny is not left in the default bind/T-pose,
  which would be the single most damaging thing in the launch footage — a T-posing rider reads as
  a bug, not a placeholder. That idle motion (subtle breathing/shifting) is **not simulated by
  MuJoCo in any way.** No articulated riding stance was authored — that needs the editor's
  animation tools, out of scope for this pass, and any custom pose would be exactly as invented as
  the stock idle, just more expensive to produce.
- **The facing (rotated 90 degrees to face across the board) is a fixed, hand-picked transform**,
  not something the physics chose — a real onewheel rider stands roughly perpendicular to the
  direction of travel. This rotates the whole body; it does **not** spread his feet apart into an
  astride stance. The idle animation's own leg pose (feet together, standing upright) is used
  as-is. A true wide, planted riding stance needs a custom pose authored in the editor — out of
  scope for this pass, and the first thing worth revisiting once someone can see the result.
- **The fore/aft and lateral offset IS real physics** — those two numbers are the actual simulated
  ballast displacement (wire v2, `rider_fore_aft_m` / `rider_lateral_m`), applied with no
  amplification: small values (a few centimetres) render as a few centimetres of shift, not
  exaggerated for legibility. If a future pass amplifies this offset for visibility, that
  amplification is a new non-physical channel and must be declared as such in code and in the PR
  that adds it — it would need its own line in the `Playable Sim` channel declaration
  (overboard#163). This pass does not do that.

If an animation beyond a static idle is ever added, say so explicitly wherever this doc and the
declaration live — per the standing instruction, that is a launch-blocking documentation change,
not a footnote.

## Toggle

`ABoardActor::bShowRider` (`EditAnywhere`, `Board|Rider` category), default true. Off skips
loading/spawning the rider entirely -- useful for isolating transform/handedness checks from
"is there also a mannequin bug" (see `docs/w1-manual-editor-steps.md`'s handedness check, which
predates the rider and should not need it).
