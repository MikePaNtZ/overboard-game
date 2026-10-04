# OB_Carve — offline render of a recorded run

`Content/Maps/OB_Carve.umap` renders a **recorded** MuJoCo run in City Park with Movie Render
Queue (MRQ). It is a blog render, not a live view. The board pose still comes only from MuJoCo:
this path computes no physics.

## How the replay works

`ABoardActor` has a file-replay mode. The command line turns it on:

```
-ObReplay=/tmp/ob-render/a03.bin -ObReplayOffset=3.0
```

- The file holds wire-v3 packets, 104 bytes each, concatenated. `tools/replay/npz_replay.py
  --bin` writes it from an overboard-viz npz track. The same script with `--udp` sends the run
  live to 127.0.0.1:9601 on an absolute clock, for a real-time check in the normal game.
- The clock is the playing Level Sequence's time plus the offset. MRQ renders much slower than
  real time, so a UDP replay would run ahead of the renderer. The sequence clock keeps the board
  and the camera keys in lock-step, through warm-up frames and temporal sub-samples.
- No socket is bound in this mode. The decoder, the MuJoCo-to-UE transform and the rider path are
  the live ones.

## The level

| Piece | Why |
|---|---|
| GameMode override `AOverboardGameMode_NoGround` | Reads the PlayerStart; no placeholder ground. |
| PlayerStart `OB_CarveOrigin` at UE (-47725, -30575, -6.2003) cm, yaw -37.6 | The MuJoCo origin of the a03 carve run. It is not the OB_City spawn. |
| Showcase, always-loaded streaming sublevel | The park. Never saved. |
| `ARenderLookOverride` | Switches off City Park's own sun, sky sphere, fogs and post-process volumes at runtime, in other levels only. Nothing on disk changes. |
| Sun, SkyAtmosphere, SkyLight (real-time capture), height fog (volumetric), clouds, unbound post-process volume | The late-afternoon look. Lumen GI and reflections. |
| One CineCamera per shot | Bound by `SEQ_Carve`. |

## Rebuild and render

```
PY=/Users/mike/projects/overboard/.venv/bin/python
NPZ=/Users/mike/projects/overboard-viz/out/carve-lab/data/a03_track.npz
$PY tools/replay/npz_replay.py $NPZ --bin /tmp/ob-render/a03.bin
$PY tools/render/plan_cameras.py $NPZ /tmp/ob-render/cameras.json
"/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor-Cmd" $PWD/OverboardGame.uproject \
  -run=pythonscript -script=$PWD/tools/render/build_carve_level.py -stdout -unattended -nosplash
tools/render/render.sh MRQ_Preview                              # 960x540, one sample, ~3 min
tools/render/render_all.sh A_wide B_low_track D_side C_chase     # 1080p, 8 temporal samples
```

`build_carve_level.py` deletes and rebuilds only `/Game/Maps/OB_Carve` and `/Game/Cinematics/`.
Its log goes to `/tmp/ob-render/build.txt`. `print()` from a commandlet does not reach the log.

## Traps found on the way

- **One MRQ job per camera cut.** With temporal samples, a single job over all shots rendered
  every shot after the first and wrote none of them ("Not all frames were fully submitted").
  `render_all.sh` runs one job per shot range. The preview config, with one sample, is not affected.
- **`-game` forks.** The launcher's own stdout is lost. `render.sh` passes `-abslog`.
- **Path tracing is not available on Mac.** `bSupportsRayTracingShaders = false` in the Mac
  platform info.
- **The project defaults have virtual shadow maps and Lumen mesh-SDF tracing off.** The MRQ
  configs switch both on for the render only.

## What is not faithful to the sim

- The board is drawn with the **Pint skin** (see citypark-level.md). MuJoCo simulates an
  Openwheel-class board. Do not present this footage as a simulation result of the Pint.
- The rider's joints are the Fab pack's authored animation. Only the pose selection and the
  ballast offset come from the sim.
- Lighting, camera moves and post-processing are artistic choices.
