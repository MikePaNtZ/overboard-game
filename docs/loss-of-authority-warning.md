# The loss-of-authority warning, on screen

**ADR-0011 second ratification, condition 3** — and `overboard-game#19`.

The ADR moved the kerb-strike and static-robustness exit criteria onto the hardware gate, and
stated that the move is honest only under three conditions. The third:

> The loss-of-authority warning ships as the in-game surfacing of the cliff. 2.868 s of lead is
> adequate.

`overboard#205` landed the **signal**. This is the half a player can see.

## What the warning is, and what it is not

It is a **diagnostic**, not a safety control. The board it warns about cannot be saved by the
warning; nothing in the loop reacts to it. What it buys is the difference between a player being
told *while there is still a run to change* and being told afterwards.

The numbers that make that concrete, all measured in `overboard#205`:

| event | sim time | relative to `FALLEN` |
|---|---|---|
| filtered authority utilisation > 0.85, below speed-cap onset | 3.000 s | **+2.868 s** |
| envelope saturated, 40 A pinned | 4.920 s | +0.948 s |
| `FALLEN` trips | 5.868 s | — |
| fully inverted | 6.470 s | −0.602 s |

Read the third row against the second. **`FALLEN` trails saturation by −0.948 s** — by the time it
fires, the board has already run out of authority and the outcome is decided. That is why
ADR-0011 asked for a different signal rather than an earlier `FALLEN` threshold.

The trigger is **saturation while below speed-cap onset**, not saturation generally. Every run
that saturated above 8.34 m/s survived, because the speed cap is already unloading the board when
it happens; a warning that fired on those would be noise, and noise is how a warning gets ignored.

## Where it comes from, and the gap that is still open

⚠️ **`sim-host` does not put this signal on the wire yet.** It computes `authority_warning` every
cycle (`host.rs:1607`) and sends it to **stderr and its trace CSV only** — those are its two
consumers. `StateOut.flags` carries armed, valid and fallen, and nothing else.

Issue #19 was filed on the understanding that the value *"arrives over the existing state
stream"*. It does not, and that is the reason the warning has not reached a player. Requested from
Senior Controls as `overboard#216` — one bit, no schema bump, on their own `INPUT_FLAG_KICK`
precedent.

**Everything on this side is built and tested against that bit today.** Until it lands,
`EStateFlags::AuthorityWarning` reads 0 on every real packet, which is the correct neutral
behaviour for a flag no sender sets, and the banner simply never fires.

## The treatment, and the two constraints that shaped it

### The lead has to survive the client

Issue #19 is explicit: *"it fires before the outcome is decided, so the lead time must survive
whatever filtering or debouncing the client adds."* So:

- **Zero debounce on the rising edge.** The banner is drawn on the first frame the bit is seen.
  No confirmation window, no n-of-m filter, no smoothing.
- **Newest raw sample, not the render-delayed pose.** `ABoardActor` reads the flag off the newest
  packet rather than the interpolated render pose — the same call `IsFallen()` already makes, and
  here it is load-bearing: reading it off the interpolated pose would hand `RenderDelaySeconds`
  (50 ms) of the ADR's lead back for no benefit. The banner therefore leads the drawn board by
  the render delay, which is the correct direction for it to be wrong in.
- **All hysteresis is on the clearing edge** (`MinimumHoldSeconds`, 0.75 s), where it cannot cost
  lead. It exists because the signal is a filtered value crossing a threshold and can dither
  across it near the boundary; a flickering warning is one a player learns to ignore.

### It must not become a behavioural claim

Under the `Playable Sim` provenance rules a game run may state facts about the machinery and
never a result. The banner reads:

```
LOSS OF PITCH AUTHORITY
commanded current at the envelope limit, below speed-cap onset
```

Both lines are facts about the machine. **"The board is about to flip" would be a result** — and
would be a false one on any run that recovers. Any future edit to this text has to keep that
property.

## Triggering it

The host does not send the bit yet, so the whole path is exercised with a replay of the ADR's own
measured timeline:

```
cd wire && make fake_sender && ./fake_sender --authority-cliff
```

It sends the warning bit at replay t = 3.000 s, `FALLEN` at 5.868 s, and the pitch profile
between ADR-0011's recorded points, at a real 50 Hz. It **computes nothing** — it replays
measured numbers, the same way a pose track is replayed rather than derived.

> **The pacing is not incidental.** The first version of this mode used `sleep_for(20 ms)` in a
> loop and delivered **~8 Hz**, not 50 — macOS sleep granularity compounding every iteration.
> That is exactly the defect `overboard#191` fixed, where `send-input` paced the same way,
> straddled the host's 100 ms staleness cutoff, and thereby **masked the very instability
> ADR-0011 was called over**. It is now paced against an absolute origin and reports the rate it
> achieved, with a warning if that rate approaches the staleness floor.

## The terrain tag rides on the same banner

`AOverboardHUD` also draws a permanent **TERRAIN UNVERIFIED** tag on any level whose drivable
surface has not been measured against the ADR-0011 authored-world envelope. That is the third of
the three things closing the `kind external` hole in `terraincheck` (see `terrain/README.md`), and
it is here rather than in its own element because it needs exactly the same thing: something
always on screen that a capture cannot omit.

Which levels are tagged is **generated** from `terrain/levels/*.terrain` into
`Source/OverboardGame/Public/TerrainVerification.g.h`, and CI regenerates and diffs it. A
hand-kept copy of "which levels are verified" would be a second source of truth, and the whole
point of `terrain/` is that a constraint kept in prose stops being true without anybody noticing.
