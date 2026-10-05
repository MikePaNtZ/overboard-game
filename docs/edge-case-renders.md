# Edge-case renders on city_hill (rider warning)

Status: planned. They render in OB_CityHill after the level is finished.

Source: the controls track, commit 1a78aed on feat/controls/downhill-carve: the X7 build plant
(`--plant x7`, 18.4 kg board) with the build's own pads and meshes in MuJoCo, end boxes kicked 4.2 deg with the deck (strike at 20.4 deg),
the 40 A cap fix (c28e529) and the pad-contact fix. This is the set to render. Tracks are at `/Users/mike/projects/overboard-carve/sim/out/city_runs/<name>/track.npz`.
Each run also has `track_full.npz` (the whole record) and `trace.csv` (nose_strike_n,
tail_strike_n, margin, margin_level). The runs are straight (|y| < 0.3 m), with a passive rider on a
speed hold. Times are sim time (the npz `t` column).

Flags: bit 4 (0x10) = handoff (Unreal owns the board), bit 5 (0x20) = pulsed buzz,
bit 6 (0x40) = solid buzz.

| # | Run | Track t (s) | Events |
|---|---|---|---|
| 1 | mike_95kg_4ms | 0-50.7 | 95 kg, 90 A motor, 4 m/s, full street, no warning (peak 39 A) |
| 2 | fast_descent_70kg | 0.03-33.3 | 7 m/s, full street, no warning |
| 3 | tail_brake_descent | 0.13-66 (cut about 15) | 100 kg, 32 A. Warning 7.15. Tail pad touches 7.75 at s = 16.3 m. At rest from 11.4 at s = 22.0 m, upright on the tail |
| 4 | climb_heavy_no_warning | 1.05-6.38 | 110 kg, Kt 0.88x, 35 A. Starts at 4 m/s on the flat. Nose strike 6.38 at s = 116.6 m. End at the strike frame: the pose after it is a MuJoCo tumble, not for the render |
| 5 | climb_heavy_rider_reacts | 1.05-6.2 | Same start. Pulsed 5.28, solid about 5.5. The rider steps off 6.00 at about s = 115 m, before run 4's strike |

- heavy_weak_no_warning and heavy_weak_rider_reacts tell the same story as run 3 with 110 kg
  (at rest 11.4-11.5 s at s = 22 m). Use one at most. heavy_weak_no_warning starts at t = 0.07 s, heavy_weak_rider_reacts at 0.69 s. The rider_reacts one pulses once at 1.2 s from the
  start-up current: a known false alarm.
- The tail TAPS the road at about 16 Hz (contact about 7 % of the time); it does not drag. Show a pad
  scrape effect only on the contact frames (trace.csv tail_strike_n), not a continuous drag.
- Do not use the first second of the full track in runs 4 and 5: the sim sets the speed at 1.0 s.
- The key clip: runs 4 and 5 back to back.

## Render work still needed

- A fall for the MetaHuman rider at the nose strike (the ADR-0012 ragdoll is for the old
  mannequin only). Until then, run 4 ends at the strike frame.
- A step-off for run 5 at 6.01 s. It is an invented motion; the card says so.
- The warning cue is the board's own lights (Mike: one board look for the Unreal renders, the game
  and the MuJoCo renders; the X7 dark theme, -ObBoardSkin=x7). Driven from the replayed state, no
  wire change: L1 front bar white, always on; L2 rear bar red, a brake flash while `current` < 0;
  L3 teal underglow and L4 teal hub rings, always on; L5 amber segments at the ends of the bars,
  pulsed (about 2 Hz) on flags bit 5 (0x20) and solid on bit 6 (0x40). The material slots exist in
  the imported X7 skin: th_lf, th_lr, led_teal, led_ring, led_amber. A buzz sound is added in the
  encode step (MRQ renders no audio). A look.json from the hardware track may follow.

## Card notes

- Runs 3-5 use deliberately SMALL motors (32-35 A) to show the edge cases. On Mike's build
  (90 A) the same climbs and descents pass.

- The rider is a passive rider on a speed hold. The reaction (ease off after 0.5 s of pulsed
  buzz, step off after 0.5 s of solid buzz while the board drives) is a model, not a measured
  human.
- The step-off motion and the fall are artistic. The board pose up to the strike or the step-off
  comes from MuJoCo.
