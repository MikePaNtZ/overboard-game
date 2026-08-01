# overboard-game

The Unreal Engine client for [Overboard](https://github.com/MikePaNtZ/overboard), the DIY
self-balancing onewheel. A player drives the ballasted board with a gamepad while MuJoCo, in the
controls repo, remains the sole authority for physics and runs the real Rust control law at a
fixed 500 Hz.

Fourth repo in the set, alongside `overboard` (controls + sim), `overboard-web` (landing page)
and `overboard-viz` (cinematic renders). Ratified in
[ADR-0009](https://github.com/MikePaNtZ/overboard/blob/master/docs/decisions/ADR-0009-fourth-repo-and-game-engineer-seat.md).
Scope and design: **M3 Scope** and **D6–D9** in Notion.

## The rule that keeps this repo honest

**The renderer never computes physics.** Unreal renders and takes input. It does not integrate the
board, it does not own contact, and collision and gravity are disabled on the board actor — the
transform is driven entirely from state the controls repo computed. If this application ever
computes a board state, the boundary has failed.

The **rule** is `overboard-viz`'s, deliberately inherited. The **mechanism is not**, and the
difference is not cosmetic: viz reads one file, one direction, offline. This repo holds a live
two-way contract against a 500 Hz control loop, and the reverse channel — player input reaching a
real-time controller — has no precedent anywhere in this estate. It is the newest and riskiest
thing here, so it is versioned like the C ABI and a schema mismatch fails loudly.

Two data contracts cross, and **neither repo imports the other**:

| Direction | Carries |
|---|---|
| `overboard` → here | Board pose, wheel angle, rider weight-shift state, controller outputs, sim time, validity |
| here → `overboard` | Weight-shift targets, steer input, arm, reset |

Both are versioned. A schema mismatch fails loudly rather than misparsing a float.

## What is deliberately not physical

The board has one hub motor and cannot steer with it. Real onewheels carve on tyre profile, which
the collision model does not represent. **Steering here is a non-physical game channel**, labelled
as such in the log schema, and the rider avatar is visual only — its articulation contributes no
mass, no inertia and no dynamics.

That is fine, and it is why this repo exists separately: it is a **game first**. Footage from it
carries its own provenance category and may never be presented as a simulation result or used to
support a claim about the controller. The controls repo's claims discipline does not extend here,
because this application cannot meet it by construction.

## Status

Past the empty-scaffold phase: the wire contract (ADR-0010), the coordinate transform, a UE 5.7
C++ project with a board actor, gamepad input, and a chase camera are landed (see closed work on
[overboard#162](https://github.com/MikePaNtZ/overboard/issues/162)). The board actor's mesh is
the real Openwheel geometry MuJoCo simulates (see **Third-party assets** below), not a placeholder
box, though a box remains the fallback if a mesh fails to load at runtime.

## Third-party assets

`Meshes/openwheel/` carries the STL geometry MuJoCo's plant model (`overboard`'s
`sim/models/overboard_onewheel.xml`) actually simulates, copied byte-for-byte from
`overboard`'s `sim/models/meshes/openwheel/`. Source: **Openwheel** by Byte Sized Engineering
(https://github.com/bytesizedengineering/Openwheel), **MIT licensed**. `Meshes/openwheel/LICENSE`
and `Meshes/openwheel/NOTICE.md` are verbatim copies of the license text and the attribution
record (including a real README/LICENSE licensing discrepancy upstream and how this project
resolved it) — read `NOTICE.md` before touching these files; do not paraphrase it elsewhere.

## What this repo must not do

Consume the 500 Hz host across the wire contract. **Do not link MuJoCo or `control-core` into the
Unreal build.** The host — physics plus the real control law — is published as a versioned artifact
by `overboard`, and keeping it behind one version string is what makes "which control law was this
session run against?" answerable without reconstructing a build graph. See ADR-0009.
