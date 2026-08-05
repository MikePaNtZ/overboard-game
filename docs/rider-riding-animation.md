# The rider rides — an authored riding stance, driven by simulated values

## The story

> As someone watching Overboard footage, I want the rider to **look like they are riding** —
> astride the deck, leaning into what the board is actually doing — so that the board reads as a
> ridden vehicle rather than a mannequin balanced on a moving object.

The rider today stands upright with their feet together, playing a stock standing idle, because
`docs/mannequin-rider.md` correctly concluded that no stock UE animation offers a wide riding
stance and that authoring one was out of scope. The Fab pack **MonoWheel Board** authored one —
ten sequences and a 2-D blendspace, on Epic's standard UE5 mannequin skeleton. So the stance we
were told we'd have to author already exists, and this work is about wiring it to the wire.

**What this does not change:** the physics still has a rigid ballast on two slide joints. No legs,
no arms, no articulation, no balance of its own. Read the honesty section below before showing
anyone the footage.

## Milestones

- [x] **M0 — Import the asset subset, licensed and ignored.** 1.8 MB of a 439 MB pack; the
      `.gitignore` entry and the reasoning live with the CityPark/Mannequins precedent.
- [x] **M1 — Bind the pack's animations to our existing rider mesh.** Runtime skeleton
      compatibility registration, so no editor retarget step and no second Manny in the project.
- [x] **M2 — Drive the blendspace from the wire, in C++.** No AnimBP asset, no Blueprint
      interface, no `ACharacter`. Axis ranges read off the asset at runtime, never hardcoded.
- [x] **M3 — Three-tier graceful fallback.** Riding stance → stock idle → no rider, each logged.
- [x] **M4 — A stimulus that actually exercises it.** `fake_sender --carve`.
- [x] **M5 — Build green.** `wire` tests pass; the Unreal editor module compiles clean.
- [x] **M5a — Fix the mannequin import path.** *Unplanned.* C0's first run reported the rider
      mesh's skeleton as `null`: the template mannequin content was imported to
      `Content/Mannequins/` when its packages record themselves as `/Game/Characters/Mannequins/`,
      so every internal reference dangled and **the rider has been in bind pose, with default
      materials, since it was added** — while the log reported an idle animation playing. Content
      moved, both asset paths corrected, `docs/mannequin-rider.md` amended. See that document.
- [x] **C0 — Verify: the asset tier resolved.** Tier 1 confirmed 2026-08-04: blendspace bound, axes `Turn` and `Forward` both `[-1..1]`.
- [ ] **C1 — Verify: the numbers, before the picture.** *(Output Log, no PIE needed)*
- [ ] **C2 — Verify: the stance is right.** *(PIE, static)*
- [ ] **C3 — Verify: it moves, and moves correctly.** *(PIE, `--carve`)*
- [ ] **C4 — Verify: against the real host.**
- [ ] **M6 — Declaration line + `docs/mannequin-rider.md` update.** Launch-blocking; do it only
      once C2/C3 have said the thing is worth keeping.

Everything through M5 is done. **C0–C4 are yours** — they need a display, which this environment
does not have. Each should take about a minute.

---

## Verification runs

Two terminals. Build first if you haven't:

```
cd ~/projects/overboard-game/wire && make fake_sender
```

### C0 — did the riding tier resolve? (Output Log only)

Open the project and press Play. Filter the Output Log for `LogOverboardMesh`. Exactly one of:

| Log line | Meaning |
|---|---|
| `playing the AUTHORED RIDING STANCE` | **Tier 1.** What we want. |
| `Content/MonoWheel_Board/ is not imported locally` | Tier 2. The asset copy didn't land — re-run the import below. |
| `riding blendspace did not bind ... skeletons incompatible at runtime` | Tier 2, and the interesting failure. `AddCompatibleSkeleton` was rejected. Tell me — plan B is to import the pack's own `SKM_Manny_Simple` (+18.5 MB). |
| `playing a stock idle animation` alone | Tier 2 for another reason. |
| `rider requested ... did not resolve` | Tier 3. `Content/Characters/Mannequins/` is missing — predates this work. |

**A T-posing rider should be impossible.** If you see one, that's the highest-priority bug here,
because the whole tier design exists to prevent exactly it.

Tier 1 also logs the authored axis names and ranges, read off the asset rather than hardcoded.
Confirmed on 2026-08-04:

```
Axis0 'Turn' [-1.00..1.00], Axis1 'Forward' [-1.00..1.00]
```

**Both axes are signed**, which is the convenient case: `MapNormalisedToAxis(N, -1, 1)` is the
identity, so the normalised signal *is* the axis value and the mapping adds nothing to reason
about. It also means the pack's Idle sample sits at the centre of both axes rather than at an
end — which is why decelerating through zero into reverse (C3 phase 3) blends continuously
instead of jumping.

The code does not assume any of this: it reads the ranges at runtime and handles a signed or an
unsigned axis either way. If a future pack version re-authors these, the mapping follows.

### C1 — do the driving numbers make sense? (Output Log only)

With PIE running, in a second terminal:

```
cd ~/projects/overboard-game/wire && ./fake_sender --carve
```

Once a second the log prints the mapping:

```
ABoardActor riding: wheel 20.63 rad/s -> 3.00 m/s (fwd norm 0.60 -> axis1 ...) | lateral 0.055 m (turn norm -1.00 -> axis0 ...)
```

What to check, all arithmetic, no judgement needed:
- `wheel × 0.1454 = m/s`.
- `fwd norm` = m/s ÷ 5.0, clamped to ±1.
- `turn norm` = −lateral ÷ 0.04, clamped to ±1. **It should sit pinned at ±1.00 during the middle
  of the carve phase** — that's the deliberate over-sweep proving the clamp works.
- The axis values stay inside the ranges C0 printed.

If C1 is right and the picture is still wrong, the bug is in the asset or the stance, not the
mapping — which is the whole point of checking numbers first.

### C2 — is the stance right? (PIE, static)

```
./fake_sender --rider          # fixed 3 cm fore, 4 cm lateral
```

This holds one pose, so it's the clean check for the three things that were wrong on the first
mannequin pass and had to be corrected against real footage:

1. **Astride?** Feet apart, across the board, not together. This is the whole point.
2. **Facing across the board**, not down the road. (`RiderMesh` gets zero relative yaw — see the
   derivation in `ABoardActor`'s constructor.)
3. **Feet on the deck?** Not floating above or sunk into it. `kRiderDeckHeightCm` is 8.3 cm and
   was tuned for the *standing* idle — **I'd expect this one to be slightly off**, because the
   riding stance's root-to-foot distance is a different number. If the feet are floating or sunk,
   read off roughly how many centimetres and that's a one-constant fix, not a redesign.

### C3 — does it move correctly? (PIE, `--carve`)

```
./fake_sender --carve          # ~24 s, prints each phase as it starts
```

Four phases, each self-labelling in the terminal so you can match log to screen:

1. **Accelerate 0 → 6 m/s straight** — rider should progressively lean forward.
2. **Hold 3 m/s, lateral sweeps full left ↔ full right, twice** — the carve. This is the phase
   `--rider` cannot show you, and the one worth watching twice.
3. **Decelerate through zero to −2 m/s** — forward lean → neutral → backward lean.
4. **Back to rest.**

Bugs worth flagging: a **pop or snap** as it crosses the extremes (blend issue); lean going the
**wrong way** relative to the board's motion (sign error — cheap fix); the animation **continuing
to deform past the limit** (clamp not working, contradicting C1); or feet **sliding/skating**
against the deck (expected to some degree — foot IK is deliberately not in this pass).

### C4 — against the real host

Whatever scenario you'd normally run. Expect the startup impulse noted in
`docs/w1-manual-editor-steps.md` (host-side, known, not this repo's bug). The thing to judge here
is whether the lean **reads as responding to the board** rather than as idle motion playing over
it.

---

## Rollback

**The branch is the rollback.** `feat/game/rider-riding-animation` — if C2/C3 look bad, don't
merge it and `master` is untouched.

Within the branch there are two more levels, both without a rebuild:

- **`bUseRidingAnim`** (`EditAnywhere`, `Board|Rider`, default true on this branch). Uncheck it in
  the Details panel and press Play again for a same-session A/B against the stock idle.
- **`bShowRider`** — the pre-existing toggle, unchanged. Off means no rider at all.

Deleting `Content/MonoWheel_Board/` also cleanly reverts to tier 2, since the directory is
gitignored and the fallback is the documented path a fresh clone already takes.

**Nothing here touches the wire, the board transform, or the pose interpolator.** No schema
version bump, so no coordination with Controls and no compatibility matrix. The board renders
byte-identically with `bUseRidingAnim` off.

---

## What is and is not simulated — read before showing anyone the footage

`docs/mannequin-rider.md` states the standing rule: if an animation beyond a static idle is ever
added, saying so is a **launch-blocking documentation change, not a footnote**. This is that
change.

**Every joint angle you see is the pack artist's invention.** The physics has a rigid ballast on
two slide joints — no legs, no arms, no articulation, no balance of its own. A rider who now
*visibly leans into a carve* invites the reading that the lean is simulated. It is not. Nothing
about the rider's articulation contributes mass, inertia or dynamics, and the board would behave
identically with `bShowRider` off.

What *is* real is only **when each authored pose is selected**:

| Blendspace axis | Driven by | Status |
|---|---|---|
| Forward | `wheel_rate_rad_s` × 0.1454 m wheel radius | Real — MuJoCo computed it |
| Turn | `rider_lateral_m`, the simulated ballast displacement | Real — MuJoCo computed it |

Turn is deliberately **not** driven from the player's steer stick. Steering is already a declared
non-physical game channel, so driving the rider's lean from the stick would show *intent* rather
than what the board did — and those differ exactly when it matters, e.g. a steer command the
controller could not honour.

### The declared gains (overboard#163)

Mapping SI values onto the pack's authored axis units needs a reference point, and a mapping
without a declared one is an undeclared gain with extra steps. Both live in `BoardActor.cpp` as
named constants:

- `kRidingFullLeanSpeedMs = 5.0` — speed at which the rider reaches full authored forward lean.
- `kRidingFullLeanLateralM = 0.04` — ballast displacement at full authored carve. The COO's stated
  "full lateral" figure.

**Neither is measured. They are legibility choices, and this is a new non-physical channel** —
declared here and requiring a line in the `Playable Sim` channel declaration before any footage
using it is published.

Note the separation this preserves: the rider's **positional offset** on the deck is still applied
with **no amplification at all** — a real 3 cm displacement still renders as 3 cm, exactly as
before. These gains scale *pose selection only*, and the offset code is untouched.

### Still not simulated, still deliberately

- **Foot IK.** The pack ships a control rig pinning feet to *its* board's footpad targets. Ours is
  Openwheel (0.938 m deck) or the Pint skin (0.70 m) — neither matches. Out of scope; expect some
  foot sliding.
- **The additive idle layer.** Imported but unused — the single-node animation path this uses
  plays one asset. Would need an AnimBP.
- **Falls, dismounts, recoveries.** The rider rides or idles. `FALLEN` does not change the pose.

## Provenance

Footage from this client already carries its own provenance category and may never be presented as
a simulation result. An animated rider makes that **more** important, not less: the motion is more
convincing and no more real.

## The asset

**MonoWheel Board**, from Fab, licensed to Mike's Epic account. `Content/MonoWheel_Board/` is
**gitignored and will never be committed** — the Fab EULA grants use in projects, not
redistribution of raw assets, and this repo is public. Same reasoning as `Content/CityPark/` and
`Content/Characters/Mannequins/`.

Only 1.8 MB of the 439 MB pack is imported: the ten riding sequences, the `Riding_BS` blendspace,
and the pack's own `SK_Mannequin` skeleton asset (161 KB) those sequences are bound to.

Deliberately **not** imported: the pack's AnimBPs — they pull inputs through a Blueprint interface
(`MonoWheel_Board_BPI`) and call `ACharacter`/`UCharacterMovementComponent` APIs (`IsFalling`,
`GetCurrentAcceleration`) that this project's rider, a bare `USkeletalMeshComponent` on an
`AActor`, does not have. Driving the blendspace from C++ instead avoids that coupling entirely and
keeps the physics-to-visual mapping in a reviewable diff rather than inside a Blueprint graph.
Also skipped: the board mesh, materials, textures, audio, maps, and the 397 MB `Demo/` folder.

To re-import on another machine (adjust the source path to your own Fab install):

```
SRC="$HOME/Documents/Unreal Projects/UEIntroProject/Content/MonoWheel_Board"
DST="$HOME/projects/overboard-game/Content/MonoWheel_Board"
mkdir -p "$DST/Animations/UE5" "$DST/Demo/UE5/Mannequins/Meshes"
for f in Forward Backward Idle Idle_Additive Left_1 Left_2 Left_3 Right_1 Right_2 Right_3; do
  cp "$SRC/Animations/UE5/MonoWheel_Board_${f}_UE5_Anims.uasset" "$DST/Animations/UE5/"
done
cp "$SRC/Animations/UE5/MonoWheel_Board_Riding_BS.uasset" "$DST/Animations/UE5/"
cp "$SRC/Demo/UE5/Mannequins/Meshes/SK_Mannequin.uasset" "$DST/Demo/UE5/Mannequins/Meshes/"
```

### Why no editor retarget step

The pack's sequences bind to the pack's own copy of Epic's `SK_Mannequin`; our rider mesh uses the
UE 5.7 template's copy. Two distinct assets, identical bone hierarchies (the UE5 mannequin skeleton
is unchanged from 5.0 to 5.7). `USkeleton::AddCompatibleSkeleton` is a **runtime** `UFUNCTION`, not
editor-only, so `TryStartRidingAnim` registers compatibility on `BeginPlay` and the pack's
animations play on our already-textured Manny. No editor ritual, no second mannequin, no 18.5 MB
duplicate mesh, no 245 MB of duplicate textures.

It is registered in **both** directions because which side the engine's compatibility check
interrogates is an implementation detail this code should not bet on — and the result is
**verified** rather than assumed: `PlayAnimation` returns `void` and declines silently on a
skeleton it will not accept, and a silent decline is precisely a rider left in the bind pose. So
`TryStartRidingAnim` succeeds only if the single-node instance is actually holding the blendspace
afterwards.
