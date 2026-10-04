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

## Cameras come from the track

`plan_cameras.py` derives every camera from the npz, so any run on this road renders without
hand keys. An anchor follows the board through a critically damped spring, and a heading follows
the smoothed direction of travel. Each shot sits behind the anchor and looks forward past the
rider at the road ahead. The anchor lags in each carve, so the board swings across the frame.
A shot can run in slow motion: its replay rate and offset are in the plan, and
`render_shots.sh` passes them to `ABoardActor` (`-ObReplayRate`, `-ObReplayOffset`).

## The dressed, animated rider (`-ObRenderRider`)

With `-ObRenderRider`, `ABoardActor` draws City Sample's player character (body and head, from
the local Fab vault) instead of the mannequin. `URiderAnimInstance` plays the same riding
blendspace and adds a procedural layer in C++:

| Motion | Source |
|---|---|
| Body lean over the planted feet | **Sim:** the ballast displacement (`rider_fore_aft_m`, `rider_lateral_m`). Lean = asin(shift / 0.9 m) x 1.5 (declared gain), with a 0.12 s lag |
| Deck tilt under the rider | **Sim:** the board quaternion (bank), through the attachment |
| Crouch depth (knee and hip flex by leg IK), arm balance, upper body that leads the turn | **Derived from sim:** centripetal acceleration = speed x yaw rate. The mapping is a choice |
| Breathing, arm flutter | **Invented:** slow sines on the replay clock |
| Head looks along the direction of travel | **Derived:** the board heading |
| The base riding stance | **Authored:** the Fab MonoWheel blendspace |

`-ObRiderLights` adds a key and a rim spot light that follow the board on lighting channel 2.
They light the rider and the board and nothing else. `-ObKeyCd=` and `-ObRimCd=` set them.

The City Sample subset (about 2.9 GB, 309 packages) is gitignored. It is the dependency closure of
`/Game/Character/Player/Female/Meshes/SKM_PlayerFemale_Body` and `SKM_PlayerFemale_Head`, copied
from `/Users/Shared/UnrealEngine/Launcher/VaultCache/CitySample_5.7/data/Content/` to the same
paths. An import table stores a name with a numeric suffix (`T_ShirtPattern_10`) split in two, so a
resolver must also try the base name plus every numbered sibling. The crowd clothing material
needs the `AnimToTexture` plugin.

## Rebuild and render

```
PY=/Users/mike/projects/overboard/.venv/bin/python
NPZ=/Users/mike/projects/overboard-viz/out/carve-lab/data/a05_track.npz
$PY tools/replay/npz_replay.py $NPZ --bin /tmp/ob-render/a05.bin
$PY tools/render/plan_cameras.py $NPZ /tmp/ob-render/cameras.json
"/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor-Cmd" $PWD/OverboardGame.uproject \
  -run=pythonscript -script=$PWD/tools/render/build_carve_level.py -stdout -unattended -nosplash
export OB_EXTRA="-ObRenderRider -ObRiderLights -ObKeyCd=90 -ObRimCd=160"
tools/render/render_shots.sh Preview     # 960x540, one sample per frame
tools/render/render_shots.sh Final       # 1080p, 8 temporal samples, about 30 min
python3 tools/render/title_card.py /tmp/ob-render/title.png "OVERBOARD" "subtitle"
```

`build_carve_level.py` deletes and rebuilds only `/Game/Maps/OB_Carve` and `/Game/Cinematics/`.
Its log goes to `/tmp/ob-render/build.txt`. `print()` from a commandlet does not reach the log.

## Traps found on the way

- **One MRQ job per camera cut.** With temporal samples, a single job over all shots rendered
  every shot after the first and wrote none of them ("Not all frames were fully submitted").
  `render_shots.sh` runs one job per shot range, with that shot's replay offset and rate.
- **`-game` forks.** The launcher's own stdout is lost. `render.sh` passes `-abslog`.
- **Path tracing is not available on Mac.** `bSupportsRayTracingShaders = false` in the Mac
  platform info.
- **The project defaults have virtual shadow maps and Lumen mesh-SDF tracing off.** The MRQ
  configs switch both on for the render only.

## What is not faithful to the sim

- The board is drawn with the **Pint skin** (see citypark-level.md). MuJoCo simulates an
  Openwheel-class board. Do not present this footage as a simulation result of the Pint.
- The rider's base stance is the Fab pack's authored animation. The layer above it mixes
  sim-driven, sim-derived and invented motion (see the table). The sim has a rigid ballast on
  two slide joints, not a body.
- Lighting, camera moves and post-processing are artistic choices.
