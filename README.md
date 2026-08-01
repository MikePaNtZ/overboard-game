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

This is `overboard-viz`'s rule, deliberately reused. The difference is only that viz replays a
batched pose track and this replays a live stream — and sends setpoints back.

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

Empty scaffold. Nothing is built yet. Phase P1 in the **M3 Implementation Plan** is the first
increment: render an existing, CI-gated scenario live, with no player input at all, to prove the
transport, the frame transform and the rate handling before anything else is added.
