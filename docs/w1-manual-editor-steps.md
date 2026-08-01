# W1 — manual editor steps still needed

Everything up to "open the editor and press Play" is done and verified from the command line
(see the PR description for exact commands/output). What's left needs the editor GUI because
this pass had no display/interactive session available:

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
   plane and the placeholder board actor purely in C++ — you should see a low flat box sitting at
   the origin even with no host running.

5. **Prove the receive path visually.** In a terminal:
   ```
   cd /Users/mike/projects/overboard-game/wire
   make fake_sender
   ./fake_sender          # sweeps the board's X position over ~1s
   ```
   The board should visibly slide in Unreal while this runs. This is the fake host — the real
   Controls-side host doesn't exist yet, so this is the only available proof of the live receive
   path tonight.

6. **Prove the send path.** With PIE running and a gamepad connected, move the left stick /
   right stick / face buttons. There's no listener on 9602 to observe this against yet (that's
   the Controls session's host), so "proof" for tonight is: no errors in the Output Log, and
   optionally run `nc -ul 9602` in a terminal to confirm UDP datagrams are landing while you move
   the stick (payload will look like binary noise in `nc`, which is expected and fine — it's not
   a text protocol).

7. **The handedness sanity check the transform code asks for** (see
   `wire/CoordinateTransform.h` comment and `wire/README.md` "what is NOT proven here"): command
   a known pure pitch and a known pure yaw through the state feed (`fake_sender` currently only
   sweeps position — extending it to sweep a rotation is a small follow-up) and confirm the board
   turns the way you expect. Do this before trusting the transform for anything beyond "the
   formula ADR-0010 described is what got implemented."

None of this changes the wire contract or the transform math — it's purely "look at the running
editor and confirm what the standalone tests already proved numerically."
