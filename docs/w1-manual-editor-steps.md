# W1/W2 — manual editor steps

Nobody has looked at a rendered frame of this project yet. This doc is written for that first
look: what you should expect to see, and how to tell within a minute whether it's right or wrong.

## Read this before you press Play

**If a real Controls host is connected (not `fake_sender`), expect the board to already be
moving — roughly 1.1 m/s, sliding off on its own the instant you connect.** Senior Controls has
an unconditional startup impulse in the host that hasn't been gated off yet. **This is a known
current state of the host, not a bug in this client and not a handedness error.** Don't chase it,
don't file it against this repo — it's already tracked on the Controls side.

**Control model, so the stick makes sense:** the outer velocity loop is OFF. The board does NOT
station-keep — there's no term pulling it back to rest. **Fore/aft is a lean command, not a
speed command:** push the stick forward to lean and accelerate, pull back to decelerate and then
reverse. Centre stick means "coast at whatever speed the board already has," not "stop." If you
centre the stick and the board keeps drifting at a constant speed, that's expected, not a bug —
there's nothing braking it.

**What genuinely would be a bug, worth flagging:** a crash, a black/blank viewport, a wall of
errors in the Output Log about the `OverboardGame` module, the board never appearing at all with
`fake_sender` running, or (see step 7) a rotation that comes out mirrored.

---

1. **Open the project.** Double-click `OverboardGame.uproject`, or run:
   ```
   "/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor" \
     "/Users/mike/projects/overboard-game/OverboardGame.uproject"
   ```
   Already built on this machine (see the PRs for the exact `Build.sh` runs) — first launch
   should not need to recompile anything. It should open into a blank/untitled level
   automatically, since no `.umap` exists in the project yet.

2. **Save a level** (so there's a real default map instead of an untitled one): File > Save
   Current Level As... > `Content/Maps/OB_Main`. Then Edit > Project Settings > Maps & Modes:
   set **Editor Startup Map** and **Game Default Map** to `OB_Main`.

3. **Confirm the Output Log is clean** on load — no errors about the `OverboardGame` module, no
   `EnhancedInputComponent` cast failures (see `AOverboardPlayerController::SetupInputComponent`
   log warning if that binding path fails).

4. **Press Play (PIE).** `AOverboardGameMode::BeginPlay` spawns, all in C++ (nothing hand-placed
   in the level yet): a 100x100m flat ground plane, the placeholder board actor (a low flat box,
   not the real model — collision and gravity are off on it; its position comes entirely from
   the wire, never from Unreal physics), and `AOverboardCameraPawn`, a chase camera that
   auto-possesses Player0 and finds the board on its own.

   **With nothing sending state (no host, no `fake_sender`) the board should sit still at the
   origin.** If the board is moving with nothing connected, that's a real bug (nothing should be
   able to move it without a wire packet telling it to).

   **The camera's framing (distance, angle, follow speed) is a first guess, never looked
   through before now.** If the board isn't in view at all, or the framing is unusable, that's
   expected to need adjustment, not a sign something is broken — the tunables are on
   `AOverboardCameraPawn` under `Board|Camera`.

5. **Prove the receive path visually** (no real host needed — this is a stand-in):
   ```
   cd /Users/mike/projects/overboard-game/wire
   make fake_sender
   ./fake_sender          # sweeps the board's X position over ~1s
   ```
   The board should visibly slide sideways in Unreal while this runs, then stop when it finishes.

6. **Check the handedness** — the single most important visual check, and the one most likely to
   silently look "fine" while actually being wrong (see the warning at the end of this step):
   ```
   ./fake_sender --rotate
   ```
   ~20s scripted sequence, printing a label to the terminal as each phase starts. Watch the
   board and confirm each phase matches:

   | Phase (printed label) | Expected on screen |
   |---|---|
   | `level (baseline)` | Board sits flat, no tilt. |
   | `NOSE UP (+25 deg)` | The end of the board along +X (world forward, the direction the camera/board face at spawn) **lifts** — tips backward, like a skateboard nose-lifting. |
   | `NOSE DOWN (-25 deg)` | The same end **drops** toward the ground — opposite of the phase above. |
   | `YAW LEFT (+25 deg)` | The nose swings toward **screen-left**. |
   | `ROLL RIGHT (+25 deg)` | The **right-hand edge** of the board (facing the way the nose points) dips toward the ground; the left edge lifts. |
   | `slow continuous pitch sweep` | The board rocks nose-up/nose-down smoothly, about one cycle every 3s — no snapping or sudden reversal. |

   **What a mirrored (handedness-bug) failure looks like:** any phase moving the *opposite* way
   from the table — nose dropping during `NOSE UP`, turning right during `YAW LEFT`, the left
   edge dipping during `ROLL RIGHT`. This will *not* look broken at a glance — it looks like
   correct motion, just backwards, and only shows up once you check a phase against its label.
   If something's mirrored: the bug is in `wire/CoordinateTransform.cpp` (`MuJoCoToUnreal`) or in
   how `ABoardActor` applies the result (`BoardActor.cpp`, `UpdatePoseFromHistory` — check the
   `FQuat` constructor's `(X,Y,Z,W)` argument order against `QuatWXYZ`), not in `fake_sender` —
   it emits exactly the quaternions `wire/tests/test_wire.cpp` already asserts on in CI.

7. **Check the stick mapping and feel.** Plug in a gamepad:

   | Stick | Wire channel(s) | What it does |
   |---|---|---|
   | Left stick, Y (fore/aft) | `weight_shift_fore_aft` | **Lean command.** Forward = accelerate, back = decelerate then reverse. Centre = coast (no braking). |
   | Right stick, X (left/right) | `weight_shift_lateral` **and** `steer`, simultaneously | Lean-to-steer: one physical axis drives both wire channels. `steer` is explicitly non-physical — see `OverboardPlayerController.h`. |
   | Face button (bottom, e.g. A/Cross) | `arm` | |
   | Face button (right, e.g. B/Circle) | `reset` | |

   Deadzone (`StickDeadzone`, default 0.12) and a response curve (`ResponseCurveExponent`,
   default 2.0) are applied before every send — see `AOverboardPlayerController::ShapeAxis`.
   **Both are first-guess defaults, not tuned** — nobody has felt this yet. If it feels twitchy
   or numb, those two named tunables (`Board|Input` category) are where to change it.

   There's no listener on 9602 to check this against unless a real host is running, so with no
   host: no errors in the Output Log is the available check, and optionally `nc -ul 9602` in a
   terminal to confirm UDP datagrams land while you move the stick (binary noise in `nc` is
   expected — it's not a text protocol).

8. **The actual point of W2: does it feel drivable?** That's a judgement call nobody has been
   able to make yet (no display in the environment that built this). If you get this far with a
   host attached and a controller in hand — that's the first real answer to "does someone who
   isn't us pick this up and not put it down."

None of the above changes the wire contract or the transform math — it's "look at the running
editor and confirm what the standalone tests already proved numerically," plus the one thing
tests can't check: how it feels.
