# W1/W2 — manual editor steps still needed

Everything up to "open the editor and press Play" is done and verified from the command line
(see the PR descriptions for exact commands/output). What's left needs the editor GUI because no
pass so far has had a display/interactive session available — including W2's actual ask, "drive
it and say how it feels," which is unverified for the same reason. See PR #162 W2 for exactly
what that means.

1. **Open the project.** Double-click `OverboardGame.uproject`, or run:
   ```
   "/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor" \
     "/Users/mike/projects/overboard-game/OverboardGame.uproject"
   ```
   First launch will compile nothing new (already built — see PR), just loads. It should open
   into a blank/untitled level automatically, since no `.umap` exists in the project yet.

2. **Save a level** (so there's a real default map instead of an untitled one): File > Save
   Current Level As... > `Content/Maps/OB_Main`. Then Edit > Project Settings > Maps & Modes:
   set **Editor Startup Map** and **Game Default Map** to `OB_Main`.

3. **Confirm the Output Log is clean** on load — no errors about `OverboardGame` module, no
   `EnhancedInputComponent` cast failures (see `AOverboardPlayerController::SetupInputComponent`
   log warning if that binding path fails).

4. **Plug in a gamepad**, press Play (PIE). `AOverboardGameMode::BeginPlay` spawns a ground
   plane, the placeholder board actor, and (as of W2) an `AOverboardCameraPawn` — a spring-arm
   chase camera that auto-possesses Player0 and finds the board actor on its own — purely in
   C++. You should see a low flat box sitting at the origin, viewed from behind/above, even with
   no host running. **The camera's framing (arm length, pitch, follow speed) is a first guess,
   not tuned** — nobody has looked through it yet. If it's unusable (too close/far/fast), that's
   exactly the kind of feedback W2 asked for; the tunables are on `AOverboardCameraPawn` under
   `Board|Camera` and are named for exactly this.

5. **Prove the receive path visually.** In a terminal:
   ```
   cd /Users/mike/projects/overboard-game/wire
   make fake_sender
   ./fake_sender          # sweeps the board's X position over ~1s
   ```
   The board should visibly slide in Unreal while this runs. This is the fake host — the real
   Controls-side host doesn't exist yet, so this is the only available proof of the live receive
   path tonight.

6. **Prove the send path, and check the mapping/feel.** As of W2, the mapping is (see
   `AOverboardPlayerController.h` for the reasoning, especially why right-stick-X drives two
   channels):

   | Stick | Wire channel(s) |
   |---|---|
   | Left stick, Y axis (fore/aft) | `weight_shift_fore_aft` — a ground-speed command now that the outer velocity loop is on; centre stick means "hold position," not "no input" |
   | Right stick, X axis (left/right) | `weight_shift_lateral` **and** `steer` simultaneously (lean-to-steer: one physical axis, two wire channels) |
   | Face button (bottom, e.g. A/Cross) | `arm` |
   | Face button (right, e.g. B/Circle) | `reset` |

   Both axes have a deadzone (`StickDeadzone`, default 0.12) and a response curve
   (`ResponseCurveExponent`, default 2.0 — finer control near centre) applied before the packet
   is sent — see `AOverboardPlayerController::ShapeAxis`. **These are untuned first guesses**,
   same caveat as the camera: centre the stick and confirm it reads as a clean, unwavering zero
   (no creep), then check that small deflections feel controllable rather than twitchy. If it
   feels wrong, the two named tunables are the place to change it, not the mapping.

   There's no listener on 9602 to observe this against yet (that's the Controls session's host),
   so "proof" for tonight is: no errors in the Output Log, and optionally run `nc -ul 9602` in a
   terminal to confirm UDP datagrams are landing while you move the stick (payload will look like
   binary noise in `nc`, which is expected and fine — it's not a text protocol).

7. **The handedness sanity check the transform code asks for.** `wire/tests/test_wire.cpp`
   proves the *numbers* `MuJoCoToUnreal()` produces are what the ADR-0010 mirror formula demands
   (see the derivation comments in `Test_TransformPureNoseUpPitch` / `Test_TransformPureYawTowardLeft`
   / `Test_TransformPureRollRight`). It cannot prove those numbers *look* right on screen — that
   needs this step, with PIE running and the board visible:

   ```
   cd /Users/mike/projects/overboard-game/wire
   make fake_sender
   ./fake_sender --rotate
   ```

   This runs a ~20s scripted sequence, printing a label to the terminal for each phase as it
   starts. Watch the board in the viewport and confirm each phase matches:

   | Phase (printed label) | Expected on screen |
   |---|---|
   | `level (baseline)` | Board sits flat, no tilt. |
   | `NOSE UP (+25 deg)` | The end of the board pointing along +X (world forward, the direction the camera/pawn faces at spawn) **lifts**; the board tips backward like a skateboard nose-lifting. |
   | `NOSE DOWN (-25 deg)` | The same end **drops** toward the ground — the opposite of the phase above. |
   | `YAW LEFT (+25 deg)` | The nose swings toward **screen-left** (toward -Y in the viewport, since UE's +Y is world-right — see `wire/tests/test_wire.cpp` comment on `Test_TransformPureYawTowardLeft`). |
   | `ROLL RIGHT (+25 deg)` | The **right-hand edge** of the board (as seen facing the same way the nose points) dips toward the ground; the left edge lifts. |
   | `slow continuous pitch sweep` | The board rocks nose-up/nose-down smoothly and periodically, roughly one full cycle every 3s, with no snapping, jittering, or sudden reversal. |

   **What a mirrored (handedness-bug) failure looks like, so you know it when you see it:** any
   phase where the board moves the *opposite* way from the table above — nose dropping during
   `NOSE UP`, the board turning right during `YAW LEFT`, the left edge dipping during
   `ROLL RIGHT`. That "looks like correct motion, just backwards" is exactly the failure mode
   this check exists to catch; it will not look broken at a glance, only wrong once you check it
   against a specific commanded phase. If any phase comes out mirrored, the bug is in
   `wire/CoordinateTransform.cpp` (`MuJoCoToUnreal`) or in how `ABoardActor` applies the result
   (`Source/OverboardGame/Private/BoardActor.cpp`, `UpdatePoseFromHistory` — check the `FQuat`
   component order passed to its constructor, `(X,Y,Z,W)`, against `QuatWXYZ`), not in
   `fake_sender` — it emits exactly the quaternions `test_wire.cpp` already asserts on.

None of this changes the wire contract or the transform math — it's purely "look at the running
editor and confirm what the standalone tests already proved numerically."
