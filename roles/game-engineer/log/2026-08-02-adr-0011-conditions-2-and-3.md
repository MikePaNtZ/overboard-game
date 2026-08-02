# 2026-08-02 — ADR-0011 conditions 2 and 3

Two of the three conditions the launch hold's criterion move depends on. PRs
[#20](https://github.com/MikePaNtZ/overboard-game/pull/20) and
[#21](https://github.com/MikePaNtZ/overboard-game/pull/21), both merged green.

## Condition 2 — the authored-world envelope, as a check (`overboard#208`)

`terrain/`: engine-free C++17, same pattern and CI slot as `wire/` and `mesh/`. `make test` proves
the rules fire; `make check` points them at the levels this repo ships.

**The issue's premise was wrong and the measurement inverted it.** #208 and ADR-0011 both predicted
"under 0.5°", reasoning that a slope is an effective static pitch disturbance eating the ±0.25°
static band. `overboard#207` measured it: a static pitch *estimate error* is a signal the
controller cannot see, so it regulates a lie forever; a *slope* is one it **can** see, because the
IMU still measures true gravity. The matrix inverts nowhere in ±12°. What binds is that there is
**no speed loop** — so the rule needs an angle **and** a run-out length.

| limit | value | from |
|---|---|---|
| step | 0.25 mm | float32 ulp at 1 km = 0.119 mm; 2× that, 4× under the ~1 mm best-case survived step |
| slope | 5.2° | 0.80 × the 6.5° self-arrest grade (outrun at 7.0°) |
| descent | 5.07 m | drop at which free-roll reaches speed-cap onset at 0.70·g·sin φ |

**The run-out formula is validated against controls' own number, not asserted.** #207 independently
reports ~1159 m to onset at 0.25°; the validator computes 1161.09 m from three separate constants
— 0.18% apart. The suite asserts that agreement, so the two sides cannot drift apart quietly.
Worth reusing as a pattern: a cross-repo constant is only trustworthy if something fails when the
two ends disagree.

## Condition 3 — the warning, on screen (`overboard-game#19`)

**The finding that mattered was that the signal is not on the wire at all.** #19 was filed on the
understanding that the value "arrives over the existing state stream". It does not: `host.rs:1607`
computes it, and its only consumers are an `eprintln!` and the trace CSV. Filed as
`overboard#216`. Going and looking at the host before building saved building the wrong thing.

Measured receipt-to-draw latency, arrival stamp to `DrawHUD`: **12.5 ms** (OB_Main), **10.1 ms**
(OB_City). End-to-end lead 2.868 s − ~12 ms = **2.856 s**.

Zero debounce on the rising edge, all hysteresis on the clearing edge, newest raw sample rather
than the render-delayed pose — three separate places the client could have quietly spent the lead
the ADR is relying on.

## What I got wrong, and how it was caught

1. **FNV-1a offset basis was one digit short.** Every level read as stale. Caught because the C++
   and Python hashes of the same file disagreed — not by review. There is now a known-answer test.
   Lesson: a hash function with no KAT is a hash function nobody has checked.
2. **The cliff replay delivered ~8 Hz, not 50.** `sleep_for(20ms)` in a loop. This is the same
   defect `overboard#191` fixed, and the reason it matters is that the identical bug **masked** the
   instability ADR-0011 was called over. Caught only because the tool prints its achieved rate.
   Anything that paces a socket should report the rate it managed, not the rate it intended.
3. **Pushed the terrain tag from `GameMode::BeginPlay` first.** The HUD is spawned by the
   PlayerController and the ordering is not guaranteed; a tag that depended on winning that race
   would go missing exactly as silently as #12's null material. Now resolved lazily by the HUD.

## Deliberately not delivered, flagged rather than fudged

- **`OB_City`'s road surface is not traced**, so the level is `unverified` and the check says so
  every run. UE 5.7 and the pack are both present locally, so this is doable — it is a line-trace
  pass along the drivable path, not a blocked item. It was cut for time, not for difficulty.
- **`OB_Main`'s marker field** — 1 m × 1 m posts, 2.5–4.0 m tall, 8 m grid, inside the corridor —
  is drawn-only obstacle geometry the drivable-surface rule does not reach. Printed as a `note` on
  every check run rather than buried in a comment. If terrain ever reaches the wire, that field
  becomes ridden geometry.
- **No screenshot committed as evidence.** The repo rule is never check binaries into git; the log
  lines and the reproduction command are the artefact instead.

## For whoever picks this up

The hold does not clear on this repo's work alone. Condition 3's first acceptance criterion — *a
player holding full forward stick gets the warning* — needs `overboard#216`, which is one line in
`host.rs`. Everything downstream of that bit is done, tested and measured.
