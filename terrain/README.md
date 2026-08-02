# `terrain/` — the authored-world envelope

**ADR-0011 second ratification, condition 2.** The kerb-strike and static-robustness exit criteria
were moved off the game gate and onto the hardware gate. The ADR is explicit that the move is
honest only under three conditions, and that dropping any one of them turns it into the softening
manoeuvre the ADR forbids. Condition 2 is:

> The authored world is constrained to what the controller survives, and the constraint is
> **encoded as a checkable asset rule** — not a hope.

This directory is that rule. `TerrainLimits.h` is its numeric half; `terraincheck` is the gate.

```
cd terrain
make test     # do the RULES fire? (violating assets authored on purpose)
make check    # do the LEVELS pass? (the assets this repo ships)
make strict   # what a public build must run — also fails on unmeasured ground
```

Both `test` and `check` run in CI on every push. A validator nobody points at the assets is not
a gate.

## Why a renderer has an opinion about terrain

This repo computes no physics (ADR-0009). MuJoCo rides a single flat, level 20×20 m plane; the
board is *drawn* over whatever scenery this project supplies. Two reasons that still needs
constraining, neither of which requires computing anything here:

1. **A drawn surface that deviates from the ridden one makes the render false.** The board would
   be drawn riding a kerb it never rode. Under the `Playable Sim` provenance rules a game run may
   state facts about the machinery and never a result.
2. **The drawn world is the specification for the ridden one.** ADR-0010 cut terrain from the wire
   and explicitly did not cancel it. The day terrain reaches the physics, the world authored here
   becomes the world ridden there. A limit encoded now binds then; a limit written in prose now
   will not.

## The limits, and where they came from

Every number is measured in `overboard` and consumed here. Nothing is re-derived.

| limit | value | provenance |
|---|---|---|
| step discontinuity | **0.25 mm** | float32 ulp at 1 km from origin is 0.119 mm; this is 2× that floor and 4× *below* the ~1 mm best-case survived step (overboard#205) |
| authored slope | **5.2°** | 0.80 × the 6.5° grade the host self-arrests a released board on; outrun at 7.0° (overboard#207) |
| continuous descent | **5.07 m** | drop at which a free-rolling board reaches the 8.34 m/s speed-cap onset at 0.70·g·sin φ (overboard#207) |

**The step limit is a geometric noise floor, not a survivability allowance.** The controller rides
out ~1 mm at its calmest and *nothing* at its worst, so the only defensible authored rule is that
steps are zero. A tolerance is still needed because "zero" is not representable across a
kilometre-scale level, so it is set from representation rather than from survival. Deriving it
from what happened to survive would be deriving the acceptance number from what happened to pass —
the precise move ADR-0011's criterion (f) exists to forbid.

**The premise behind the slope limit changed, and the change is worth knowing.** Both ADR-0011 and
issue #208 predicted "under 0.5°", reasoning that a slope is an effective static pitch disturbance
and therefore eats the measured ±0.25° static-error band. overboard#207 measured it and the middle
step fails: a static pitch *estimate error* is a signal the controller cannot see, so it regulates
a lie forever; a *slope* is one it can see, because the IMU still measures true gravity. The
matrix inverts **nowhere** in ±12°. What binds instead is that **there is no speed loop** —
`MAX_GROUND_SPEED_M_S` shapes the stick, and a stick cap cannot brake against gravity. So the rule
needs an angle **and a length**: 0.25° is fine for a metre and not for a kilometre.

The run-out formula is cross-checked against controls' own number rather than invented here —
overboard#207 independently reports ~1159 m to speed-cap onset at 0.25°, and `MaxRunOutM(0.25)`
returns 1161.09 m, 0.18% apart. `test_terrain` asserts that agreement, so the two sides cannot
drift apart quietly.

## Declaring a level

Every `Content/Maps/*.umap` needs `terrain/levels/<Level>.terrain`, or the check fails. A `.umap`
is an opaque binary and `OB_City`'s scenery is a gitignored 1.5 GB Fab pack, so CI cannot open
either — the level declares its drivable surfaces as small, reviewable, diffable text instead.

```
level      Content/Maps/OB_Main.umap
level_hash b5e92fd43f779c3d

note       free text, printed on every check run

surface    placeholder_ground
  kind      plane | mesh | probe | external
  status    verified | unverified
  source    where these numbers came from        (required, never blank)
  slope_deg 0.0        # plane
  run_out_m 0.0        # plane
  path      Meshes/x.stl ; units mm|cm|m         # mesh — checked against the real file
  probe     x_m y_m z_m ; reach_m 100            # probe — measured points, repeatable
```

Parsing is **strict**: an unknown key, a blank `source`, a number with a unit suffix, a single
probe point, or a missing `level_hash` is a hard failure. A declaration that half-parses reads as
coverage it does not have.

**`level_hash` is what stops this becoming a comment that drifts.** Change the `.umap` without
re-declaring and the check goes red. Without it a declaration is documentation, and documentation
is a polling surface.

## What the mesh check actually looks for

The two ways a mesh breaks smoothness in practice, both invisible to the eye at the scale that
matters:

- **Wall facets.** A near-vertical facet's vertical extent is a step. This is a kerb, a brick
  edge, a root heave — the 20 mm case ADR-0011 measures at 201 °/s against ~76 °/s of KD
  authority.
- **Open seams.** Two boundary edges sitting at the same place in plan but at different heights:
  a tiling mistake or a collision-hull artifact. Boundary edges are found by counting triangle
  uses per welded edge; the weld tolerance is an order of magnitude below the step limit, so
  welding can never hide a step the check is looking for.

Normals are recomputed from winding and never read from the file, for the same reason `StlLoader`
gives: an exporter's normal is not evidence.

## The hole in the gate, and the three things that close it

`kind external` produces **no verdict** — it is for geometry this repo cannot open, which today is
only the City Park pack. It exists so that "we have not measured this" is something the check
*says*, rather than something it cannot represent; an undeclarable level would otherwise be
indistinguishable from an oversight. It is closed three ways:

1. the parser refuses to let an `external` surface call itself `verified`;
2. `terraincheck --strict` fails on it, and that is what a public build runs;
3. the runtime tags the level on screen, so no footage can be shot on an unverified level
   without the tag being visible. **This third one is not shipped yet** — it lands with the
   loss-of-authority HUD (ADR-0011 condition 3, overboard-game#19), which is the other thing
   that needs a persistent on-screen banner. Until then only (1) and (2) hold, and (2) is the
   load-bearing one.

**`OB_City` is unverified today and the check says so on every run.** That is the honest state.
Clearing it means tracing the park's road surface along the drivable path and replacing the
`external` surface with a measured `probe` series.

## Do not widen a limit to make a level pass

If `TerrainLimits.h` is ever edited to admit an asset, the criterion move onto the hardware gate
has become the softening manoeuvre ADR-0011 forbids by name, and the hold does not clear. Change
the asset.
