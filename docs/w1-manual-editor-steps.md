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

   **Real board geometry.** The board actor should be the real Openwheel STL geometry
   (`Meshes/openwheel/`), built at runtime — not the low flat placeholder box. Check the Output
   Log for one of two outcomes:
   - `ABoardActor: real board mesh loaded, placeholder box hidden.` — the real mesh built. Look
     at the board: seven distinct shell/bumper/footpad/platform parts plus a wheel cylinder,
     roughly matching a real onewheel's proportions.
   - `ABoardActor: real board mesh failed to load ... falling back to the placeholder box.` — you
     see the low flat box instead. Not silent, not invisible — but means something in the STL
     load path failed; look a few lines up in the Output Log for which specific part
     (`ABoardActor: failed to load <name>: <error>`) and why.

   **Scale is now arithmetic, not eyeballed — check these two log lines, not the screen:**
   ```
   ABoardActor: body (7 STL parts) local bounds half-extent = (X, Y, Z) cm, full size = (..) cm
     -- expected half-extent ~(46.9, 11.6, 4.2) cm / full ~(93.8, 23.2, 8.3) cm. MATCH.
   ABoardActor: wheel mesh world bounds extent = (X, Y, Z) cm -- expect radius ~14.54cm, width ~15cm ...
   ```
   The first line must say **MATCH**, not **MISMATCH**. (Mike's second session caught a real bug
   here: the real mesh was parented under the placeholder box, so it silently inherited the
   box's own `(0.7, 0.25, 0.08)` shaping scale on top of its own correct conversion — exact
   numeric match to the measured on-screen sliver. Fixed by giving the real mesh and the
   placeholder box separate, identity-scale parents; see `SceneRoot` in `BoardActor.h`. If MATCH
   ever regresses back to MISMATCH, this exact bug is the first thing to check.) The wheel line
   still depends on an *assumed* (not verified) native size for
   `/Engine/BasicShapes/Cylinder.Cylinder` — compare its printed bounds against ~14.54cm radius.

   Materials/colour are **not** attempted this pass (default engine grey) — deliberately, to keep
   risk down; the exact brand-palette RGBA per part is in `overboard`'s
   `sim/models/overboard_onewheel.xml` `<asset>` block whenever someone wants to add it.

   **Turn direction / "the board doesn't look like it's moving":** two contributing causes, both
   now addressed the same pass. (1) The scale bug above — a 6cm-wide, 7mm-tall sliver has no
   visible heading, so a correct carve read as sideways drift. (2) **The scene had no motion
   reference at all**: the chase camera follows the board's position and yaw over a completely
   featureless plane, so the board stays pinned to the centre of frame with nothing else in view
   to show it travelling 15-20m per run. The COO diagnosed this from real captured frames (board
   in nearly the same screen position and orientation at *settle* and mid-*turn*, despite ~16m of
   travel and 96° of yaw) — heading and path direction were independently verified to already
   converge correctly in the wire data; nothing about steering/transform logic changed for this.
   **Fix, CONFIRMED WORKING by the COO's own headless captures (not eyeballed, not code-reasoning
   this time):** `AOverboardGameMode::SpawnMotionReferenceMarkers` scatters ~36 primitive
   cube/cylinder/cone markers (collision off, scenery only) on an 8m grid over a ~40m field, so
   there's something in the scene the board visibly moves past. "The scene now has depth,
   landmarks and readable shadows, and the board reads as a board at correct scale." Toggle:
   `bSpawnMotionReferenceMarkers` on `AOverboardGameMode` — same per-level pattern as the ground
   plane, also suppressed by `AOverboardGameMode_NoGround` (a real environment brings its own
   landmarks).

   **Ground shadow acne — root cause found, four passes in.** History, because each ruled-out
   hypothesis is worth knowing so nobody re-tries it:
   1. `SetCastShadow(false)` alone — no change.
   2. Swap to `WorldGridMaterial` — no change (this is where it turned out the *real* bug was
      hiding, but wasn't noticed yet — see point 4).
   3. Disabling Lumen diffuse indirect + its denoiser, disabling Virtual Shadow Maps, `viewmode
      unlit` (control test), `r.ScreenPercentage 200` (rules out ordinary mip aliasing) — **all
      pixel-identical, speckle still present.** `viewmode unlit` still showing it is decisive:
      that strips lighting, shadow and GI entirely, so this was never a lighting artifact.
   4. **The actual bug:** `LoadObject<UMaterialInterface>("/Engine/EngineMaterials/
      DefaultMaterial.DefaultMaterial")` was silently returning null, at **both** the ground and
      the motion-reference-marker call sites, and both silently skipped `SetMaterial` when it
      did. Caught because the COO noticed markers visibly checkerboarded in captures despite
      "using" the same flat material as the ground — two independent call sites failing
      identically pointed at one shared cause. Neither call site logged the failure, so two
      passes were spent chasing lighting settings that were never the problem.

   **Fix:** `MakeFlatMaterial()` (shared by both call sites now, not duplicated) uses
   `UMaterial::GetDefaultMaterial(MD_Surface)` — documented to always return a valid material, no
   string asset path to get wrong — wrapped in a `UMaterialInstanceDynamic` with a best-effort
   colour tint (ground and markers get slightly different tones; harmless no-op if the engine
   default material doesn't expose that parameter). Both call sites now log loudly if this ever
   fails. `SetReceivesDecals(false)` and `SetCastShadow(false)` both stay (still individually
   correct, just never the actual cause). Still never touched `r.Lumen.DiffuseIndirect.Allow` or
   any project-wide VSM/Lumen setting.

   **Camera framing: Mike confirmed good, left alone.** No change this pass.

   **This pass was verified differently:** the COO now has headless UE rendering/screen-capture
   working directly, independent of Mike, and verified the aliasing diagnosis with real captured
   frames (A/B testing shadow/GI settings) before this fix was written — see the PR for what was
   actually checked versus what's still a code-reasoning argument (the motion-reference fix).

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

6. **Check the mapping and feel.** Keyboard works with no gamepad plugged in — the CEO's explicit
   ask, so a keyboard-first driving session doesn't need an Xbox controller at all:

   | Gamepad | Keyboard (both work) | Wire channel(s) | What it does |
   |---|---|---|---|
   | Left stick, Y (fore/aft) | **W / Up Arrow** = positive, **S / Down Arrow** = negative | `weight_shift_fore_aft` | **Lean command.** Forward = accelerate, back = decelerate then reverse. Centre/no key = coast (no braking). |
   | Right stick, X (left/right) | **D / Right Arrow** = positive, **A / Left Arrow** = negative | `weight_shift_lateral` **and** `steer`, simultaneously | Lean-to-steer: one physical input drives both wire channels. `steer` is explicitly non-physical — see `OverboardPlayerController.h`. |
   | Face button (bottom, e.g. A/Cross) | **Space** | `arm` | |
   | Face button (right, e.g. B/Circle) | **R** | `reset` | |

   The gamepad bindings are byte-for-byte unchanged from before keyboard support was added — same
   keys, same code path, still the launch configuration. Keyboard is additive, in the same
   mapping context, not a replacement.

   Deadzone (`StickDeadzone`, default 0.12) and a response curve (`ResponseCurveExponent`,
   default 2.0) are applied before every send — see `AOverboardPlayerController::ShapeAxis`.
   Mike drove the real host with the gamepad mapping end-to-end (forward/coast/reverse all
   worked); if it still feels twitchy or numb, those two named tunables (`Board|Input` category)
   are where to change it.

   **Keyboard-specific:** a key is digital (0 or full deflection the instant it's pressed), unlike
   a stick's gradual sweep, so `KeyboardRampSpeed` (`Board|Input`, default 3.0) ramps the sent
   value toward whatever's held rather than snapping to full lean in one frame — a raw digital
   slam to ±1 would feel bad and might not even be drivable. Unverified by feel (no display in
   this environment); tune it if a keypress still feels like a jolt rather than a lean.

   **Press Escape to quit.** The CEO's first session trapped him in the window with no way out
   short of switching virtual desktops to force-quit. Escape is now mapped to `IA_Quit` in the
   same mapping context as everything else.

   **If a keypress or stick move does nothing at all** (not "feels wrong", genuinely zero effect):
   this exact symptom already happened once — both `SetupInputComponent()`'s `InputComponent` and
   `InitInputSystem()`'s `PlayerInput` silently fell back to plain (non-Enhanced) base classes in
   `-game` mode, because `UInputSettings::GetDefault{InputComponent,PlayerInput}Class()` resolves
   a `TSoftClassPtr` that's only valid if the class happens to already be loaded in memory at that
   exact moment — `Config/DefaultEngine.ini`'s `DefaultPlayerInputClass`/`DefaultInputComponentClass`
   settings were correct and still didn't help. Both are now pinned explicitly in code
   (`AOverboardPlayerController`'s constructor and `SetupInputComponent`), not left to that
   resolution. If input goes dark again: run with `-OverboardInputSelfTest` on the command line
   (headless-safe, no display needed) — it injects a simulated keyboard AND gamepad axis press
   through the real Enhanced Input pipeline via `UPlayerInput::InputKey`/`FInputKeyEventArgs::
   CreateSimulated` and logs `OverboardInputSelfTest KEYBOARD: PASS/FAIL` and `...GAMEPAD:
   PASS/FAIL`. Raising `LogOverboardInput` to `Verbose` (`-LogCmds="LogOverboardInput Verbose"`)
   also logs the actual value arriving at every handler, every frame — if the handler never
   fires, it's the mapping; if it fires with zero, it's the action/trigger, not the mapping.

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

## Bringing in a real environment (e.g. a Fab download)

Written for someone who has never done this in this project. `OB_Main` (the flat checkered
plane) is the fallback — ship it ugly rather than ship nothing — so nothing below risks that.

1. **Import the asset.** From the Fab library / the downloaded package, add it to
   `OverboardGame` the normal way (Epic Games Launcher "Add to Project", or drag the content
   folder into the Content Browser). This puts a new folder under `Content/` (e.g.
   `Content/DestroyedWarehouseKaarina/...`) alongside `Content/Maps/OB_Main.umap` — nothing here
   touches or replaces `OB_Main`.

2. **Open the environment's own demo/sample map** (most Fab environments ship one, usually named
   something like `Demo_Map` or `Showcase` inside the asset's own folder) so you're looking at it
   already lit and dressed, rather than an empty level with the assets floating in a content
   browser.

3. **Save it as a new map under `Content/Maps/`** — File > Save Current Level As... >
   `Content/Maps/<something>` (e.g. `OB_Warehouse`). Don't overwrite `OB_Main` and don't leave it
   living only inside the downloaded asset's own folder — keep every playable map in
   `Content/Maps/` so they're easy to find later.

4. **Set the GameMode override so the placeholder ground doesn't spawn.** Window > World Settings
   (if the panel isn't already open) > **GameMode Override** > pick **OverboardGameMode_NoGround**
   from the dropdown. This is a plain C++ class already built into the project — nothing to
   create, no Blueprint to author, just a selection in that one dropdown. (Leaving GameMode
   Override unset, or picking plain `OverboardGameMode`, spawns the 100x100m placeholder plane,
   which will slice through the warehouse's floor/walls/props — that's the bug this step avoids.)
   The board, camera, and everything else spawn exactly the same either way; only the ground
   plane is affected.

5. **Check the floor height is Z = 0.** MuJoCo's own ground plane is at Z = 0 in world space, and
   the board's wire-driven position assumes that same ground height — it does not know anything
   about this level's geometry (see the constraint below). If the warehouse's floor sits at some
   other Z in the level, the board will appear to float above it or sink into it, and **that will
   look exactly like a physics bug, not a level-placement issue** — it's the first thing to check
   if the board doesn't sit on the visible floor. Select the floor/foundation piece(s) and read
   their Z location in the Details panel; move the whole environment (or just double-check its
   import origin) so the walkable floor is at Z = 0.

6. **Press Play.** Stage the board in a clear area of floor for anything you're capturing. This
   is a *destroyed* warehouse — expect rubble and debris on the floor. **That's cosmetically
   fine.** The board will pass straight through props and rubble: collision stays off on every
   board mesh component (see the constraint below), and the environment is scenery only, so
   nothing here is a physics interaction to report as broken. If the board is sliding around on
   its own with no host connected, that's still the same "not a bug" note from the top of this
   doc (real host's unconditional startup impulse) — it applies here too, the environment doesn't
   change it.

**Constraint that doesn't move:** the environment is scenery only. It must never become collision
the board interacts with — MuJoCo runs on a plain ground plane and knows nothing about the
warehouse, so the moment Unreal geometry starts affecting the board's motion, the boundary rule
(this repo computes no physics) has failed. If anything about the environment ever needs to
affect the board, that has to happen by changing what MuJoCo simulates, not by wiring UE collision
into `ABoardActor`.
