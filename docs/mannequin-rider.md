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

## Corrected once against real capture, and the height figure changed as a result

First pass shipped with a height derived from `TryBuildRealMesh`'s own logged body bounds
(footpad Z max, ~5.8cm above the actor origin) and an extra +90 degree facing rotation. Real
footage showed both wrong: he was facing down the road (see the facing note below) and the board
sat visibly below and behind his feet. Height is now the CEO's directly-measured **~8.3cm** deck
top (a single named constant, `kRiderDeckHeightCm` in `BoardActor.cpp`, used everywhere the
rider's height is set so the two call sites cannot drift apart the way an internal estimate and a
real measurement briefly did). Fore/aft placement otherwise still centres him at the board's
local origin (roughly the axle) before the real ballast offset is added — reasonable by
construction, not yet independently re-confirmed against footage.

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
- **The facing is a fixed, hand-picked transform**, not something the physics chose — a real
  onewheel rider stands roughly perpendicular to the direction of travel (skateboard/snowboard
  stance), not facing down it. As shipped, `RiderMesh` is given **no additional relative yaw at
  all** (`FRotator::ZeroRotator`): the UE mannequin's bind pose already faces its own local +Y,
  and this actor's board faces along local X (nose at -X) — so an unrotated Manny already faces
  across the board. (An earlier attempt added +90 degrees on the wrong assumption that his bind
  pose faces +X; real footage showed that turned him to face the board's nose, "down the road,
  arms out like a snowboarder." See the code comment in `ABoardActor`'s constructor for the full
  derivation.) This is not a fully authored stance — it rotates the whole body; it does **not**
  spread his feet apart into an astride stance. The idle animation's own leg pose (feet together,
  standing upright) is used as-is; no stock animation in `Content/Mannequins/Anims/` offers a
  wider standing stance (checked: idle/attack/walk/jog/jump only, and walk/jog are directional
  cycles, not a static wide pose), and a true wide, planted riding stance needs a custom pose
  authored in the editor — deliberately out of scope this pass, called out as low-priority
  ("minor cosmetic issue") relative to the facing/placement fixes.
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
