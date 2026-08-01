# W1/W2/W3 — manual editor steps

Mike's first PIE session (overboard#162) already confirmed the big two: the coordinate transform
is not mirrored (all four rotation phases checked out), and the real host drives the board
forward/coast/reverse correctly end-to-end. Since then (W3): the board actor's mesh is the real
Openwheel geometry MuJoCo simulates, built at runtime from STL, not the placeholder box — see
"real board geometry" under step 3. **None of the W3 geometry has been seen on screen yet** —
same "no display in this environment" constraint as every prior pass. This doc is "here's what's
already confirmed, here's what's new and unverified, and here's how a first-time reader tells
right from wrong within a minute."

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
there's nothing braking it. Mike already confirmed this feels right end-to-end.

**What genuinely would be a bug, worth flagging:** a crash, a black/blank viewport, a wall of
errors in the Output Log about the `OverboardGame` module, the board never appearing at all with
`fake_sender` running, or a rotation that comes out mirrored (see step 5 — already checked once
and passed, but worth re-confirming after any transform-adjacent change).

---

1. **Open the project.** Double-click `OverboardGame.uproject`, or run:
   ```
   "/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor" \
     "/Users/mike/projects/overboard-game/OverboardGame.uproject"
   ```
   Already built on this machine — first launch should not need to recompile anything.
   **`Content/Maps/OB_Main` is a committed level asset, and `Config/DefaultEngine.ini` already
   sets it as both Editor Startup Map and Game Default Map.** It should just open — no "new
   level, delete the template Floor, save as, set as default" ritual. If it opens into a blank
   untitled level instead, something's wrong with the map settings, not a step you're missing.

2. **Confirm the Output Log is clean** on load — no errors about the `OverboardGame` module, no
   `EnhancedInputComponent` cast failures (see `AOverboardPlayerController::SetupInputComponent`
   log warning if that binding path fails).

3. **Press Play (PIE).** `AOverboardGameMode::BeginPlay` spawns, all in C++ (nothing hand-placed
   in the level yet): a 100x100m flat ground plane, the board actor, and `AOverboardCameraPawn`,
   a chase camera that auto-possesses Player0 and finds the board on its own. Collision and
   gravity are off on every board mesh component — the board's position comes entirely from the
   wire, never from Unreal physics.

   **With nothing sending state (no host, no `fake_sender`) the board should sit still at the
   origin.** If the board is moving with nothing connected, that's a real bug (nothing should be
   able to move it without a wire packet telling it to).

   **Real board geometry (new, W3, completely unverified visually).** The board actor should be
   the real Openwheel STL geometry (`Meshes/openwheel/`), built at runtime — not the low flat
   placeholder box. Check the Output Log for one of two outcomes:
   - `ABoardActor: real board mesh loaded, placeholder box hidden.` — the real mesh built. Look
     at the board: seven distinct shell/bumper/footpad/platform parts plus a wheel cylinder,
     roughly matching a real onewheel's proportions. **If it looks roughly 10x too big or too
     small, or any single part is wildly out of place**, that's the first thing to report —
     `mesh/README.md` and the code comments in `ABoardActor::BuildPartFromStl` /
     `TryBuildRealMesh` explain the mm→cm scale and placement logic to check against. The wheel
     specifically depends on an *assumed* (not verified) native size for
     `/Engine/BasicShapes/Cylinder.Cylinder` — there's a log line printing its actual computed
     bounds in cm to check against the expected ~14.54cm radius / ~15cm width.
   - `ABoardActor: real board mesh failed to load ... falling back to the placeholder box.` — you
     see the low flat box instead. Not silent, not invisible — but means something in the STL
     load path failed; look a few lines up in the Output Log for which specific part
     (`ABoardActor: failed to load <name>: <error>`) and why.

   Materials/colour are **not** attempted this pass (default engine grey) — deliberately, to keep
   risk down; the exact brand-palette RGBA per part is in `overboard`'s
   `sim/models/overboard_onewheel.xml` `<asset>` block whenever someone wants to add it.

   **Two known, fixed-but-not-yet-re-verified issues from the first session, both yours to watch
   for:**
   - **Shadow acne on the ground plane** (dense black stipple in rectangular blocks) — near-
     certain cause was the ground's 100x-stretched-unit-plane UVs/normals confusing Virtual
     Shadow Maps. Fixed by turning off the ground's own shadow-casting
     (`AOverboardGameMode::BeginPlay`, `SetCastShadow(false)` on the ground's mesh component) --
     it's flat, has nothing worth casting, and still receives the board's shadow. **Unverified
     visually** (no display in the environment that made this fix) — confirm the stipple is
     actually gone, not just theoretically addressed.
   - **Camera slightly too far out** — Mike's read was "can see the board the whole time, minor,
     maybe zoom in a bit." `ArmLengthCm` pulled in from 650 to 480 (modest, deliberately not a
     close chase cam — the board covers real ground under lean and losing it off-frame is worse
     than reading a bit small). **Unverified visually** — confirm 480 actually reads better, not
     just numerically smaller. Tunable is `AOverboardCameraPawn` → `Board|Camera` →
     `ArmLengthCm` if it still wants adjusting.

4. **Prove the receive path visually** (no real host needed — this is a stand-in):
   ```
   cd /Users/mike/projects/overboard-game/wire
   make fake_sender
   ./fake_sender          # sweeps the board's X position over ~1s
   ```
   The board should visibly slide sideways in Unreal while this runs, then stop when it finishes.

5. **Check the handedness.** Already confirmed correct once (Mike, first PIE session, all four
   phases). Re-run it after any change anywhere near the transform, the board actor, or the
   camera, since it's cheap and it's the failure mode most likely to silently look "fine":
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

6. **Check the stick mapping and feel.** Plug in a gamepad:

   | Stick | Wire channel(s) | What it does |
   |---|---|---|
   | Left stick, Y (fore/aft) | `weight_shift_fore_aft` | **Lean command.** Forward = accelerate, back = decelerate then reverse. Centre = coast (no braking). |
   | Right stick, X (left/right) | `weight_shift_lateral` **and** `steer`, simultaneously | Lean-to-steer: one physical axis drives both wire channels. `steer` is explicitly non-physical — see `OverboardPlayerController.h`. |
   | Face button (bottom, e.g. A/Cross) | `arm` | |
   | Face button (right, e.g. B/Circle) | `reset` | |

   Deadzone (`StickDeadzone`, default 0.12) and a response curve (`ResponseCurveExponent`,
   default 2.0) are applied before every send — see `AOverboardPlayerController::ShapeAxis`.
   Mike drove the real host with this mapping end-to-end (forward/coast/reverse all worked); if
   it still feels twitchy or numb, those two named tunables (`Board|Input` category) are where
   to change it.

   With no host running: no errors in the Output Log is the available check, and optionally
   `nc -ul 9602` in a terminal to confirm UDP datagrams land while you move the stick (binary
   noise in `nc` is expected — it's not a text protocol).

7. **Reset-on-fall (new, W3, untested against a real host).** With the outer velocity loop off,
   a fallen board coasts on whatever velocity it had rather than stopping, so it can leave the
   play area if nobody reacts. `AOverboardPlayerController` watches `ABoardActor::IsFallen()`
   (the `Fallen` bit on the newest received state) and fires one `Reset` packet on the
   rising edge — not held down, exactly one packet per fall. Output Log line to look for:
   `AOverboardPlayerController: board fell, sending one Reset.` This has never run against a
   real host (nothing in this repo can trigger a `Fallen` flag standalone) — if the board falls
   and does *not* visibly reset, check that log line fired at all before assuming the host
   ignored the Reset flag.

None of the above changes the wire contract or the transform math — it's "look at the running
editor and confirm what the standalone tests already proved numerically," plus the two visual
fixes above that only a running session can actually confirm.
