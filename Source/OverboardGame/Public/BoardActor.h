// BoardActor.h
//
// The rendered board. THE RULE: this actor computes no physics. Collision and gravity are
// disabled in the constructor, on every mesh component including the real board geometry below;
// its transform every tick comes entirely from state received over the wire, one render-delay
// behind the wall clock, interpolated -- never extrapolated.
//
// W3 (overboard#162): the real Openwheel geometry (see ../../mesh/README.md and
// ../../Meshes/openwheel/), built at runtime from the STL files via UProceduralMeshComponent --
// no .uasset import step, see mesh/README.md for why. BoxMesh, the W1 placeholder, stays: it is
// the fallback if the real mesh fails to load for any reason. An invisible board during capture
// is worse than an ugly one, so failure here must never mean "nothing renders".
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BoardStateClient.h"
#include "BoardActor.generated.h"

class UStaticMeshComponent;
class UProceduralMeshComponent;
class USkeletalMeshComponent;
class UAnimSequence;
class UBlendSpace;

UCLASS()
class OVERBOARDGAME_API ABoardActor : public AActor
{
	GENERATED_BODY()

public:
	ABoardActor();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	// True if the newest received sample had OverboardWire::EStateFlags::Fallen set. False
	// (never fallen) before anything has been received. For AOverboardPlayerController's
	// reset-on-fall (overboard#162 W3) -- reads the newest raw sample, not the interpolated
	// render pose, so it reacts on the tick data actually arrives, not one render-delay later.
	bool IsFallen() const { return bLatestSampleFallen; }

	// True if the newest received sample had OverboardWire::EStateFlags::AuthorityWarning set --
	// ADR-0011 exit criterion (c), surfaced by AOverboardHUD (condition 3 of the second
	// ratification, overboard-game#19).
	//
	// Newest RAW sample, deliberately, for the same reason as IsFallen() and one more: the ADR
	// is relying on 2.868 s of lead against FALLEN, and reading this off the render-delayed
	// interpolated pose would hand RenderDelaySeconds of that lead back for no benefit. A safety
	// annunciation has no reason to be smoothed into agreement with the drawn pose. It does mean
	// the banner leads the board on screen by the render delay, which is the correct direction
	// for it to be wrong in.
	//
	// **Reads false today on every packet from the real host**, which computes this signal every
	// cycle and sends it to stderr and its trace CSV rather than to the wire -- see
	// EStateFlags::AuthorityWarning for the gap and what has been requested to close it.
	bool IsAuthorityWarning() const { return bLatestSampleAuthorityWarning; }

	// FPlatformTime::Seconds() at which the packet that raised the CURRENT warning was received
	// off the socket. 0 if no warning is active. Exists so AOverboardHUD can measure and log the
	// receipt-to-draw latency on the frame it actually draws -- issue #19 AC2 asks for the
	// end-to-end lead as a measured number, not the host-side figure quoted forward.
	double GetAuthorityWarningArrivalTimeSeconds() const { return AuthorityWarningArrivalTimeSeconds; }

	// Where MuJoCo's world origin lands in UE world space, in centimetres.
	//
	// The wire carries an ABSOLUTE position and UpdatePoseFromHistory applies it with
	// SetActorLocation, so without this the board is pinned to UE (0,0,0) no matter where it is
	// spawned -- the first packet overwrites the spawn transform. That is fine over the C++
	// placeholder ground (a 100x100m plane centred on the origin) but useless in an imported
	// environment, where the origin is wherever the environment author happened to put it and is
	// typically not flat, not paved, and sometimes not even above ground.
	//
	// This is a RENDERING offset and nothing else. It is added after MuJoCoToUnreal and never
	// fed back anywhere: the wire, the controller and MuJoCo all keep working in MuJoCo's own
	// frame, exactly as before. Moving the board around a level cannot change a single physics
	// value, which is the property ADR-0009 is protecting.
	void SetWorldOriginOffsetCm(const FVector& InOffsetCm) { WorldOriginOffsetCm = InOffsetCm; }
	FVector GetWorldOriginOffsetCm() const { return WorldOriginOffsetCm; }

	// Which way MuJoCo's +X points in this level, in degrees of world yaw.
	//
	// Position offset alone is not enough once the board is somewhere real: a road runs whichever
	// way the environment author laid it, and MuJoCo's forward is a fixed axis. Without this the
	// board drives across the carriageway instead of along it, and no amount of moving the
	// PlayerStart fixes that.
	//
	// YAW ONLY, and that restriction is not fussiness. Pitch or roll here would rotate the frame
	// that gravity is expressed in: the board would render leaning, and a viewer would read it as
	// the controller failing to hold level when nothing of the sort had happened. Yaw about the
	// world vertical is the one rotation that cannot lie about attitude.
	void SetWorldOriginYawDeg(float InYawDeg) { WorldOriginYawDeg = InYawDeg; }
	float GetWorldOriginYawDeg() const { return WorldOriginYawDeg; }

protected:
	// The actor's true root: identity scale, always. BoxMesh and MeshAssemblyRoot are SIBLINGS
	// attached here, not parent/child of each other -- see the scale bug this fixed (overboard#162):
	// BoxMesh carries its own non-uniform SetRelativeScale3D to turn a 1m cube into a board-ish
	// placeholder shape, and a child component inherits its parent's scale by default. The real
	// mesh was attached under BoxMesh, so it silently inherited (0.7, 0.25, 0.08) on top of its
	// own correct mm->cm conversion -- exact match to the measured on-screen sliver. The
	// placeholder's cosmetic scale must never be able to reach anything else in the hierarchy.
	UPROPERTY(VisibleAnywhere, Category = "Board")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Board")
	TObjectPtr<UStaticMeshComponent> BoxMesh;

	// How far behind the wall clock we render, in seconds. Must always be able to find two real
	// samples to interpolate between; too small and we run out of history and hold the last
	// known pose (still not extrapolation, just a stall). Tune once the host's real send rate
	// (500 Hz control loop) is confirmed -- this default is a conservative placeholder.
	UPROPERTY(EditAnywhere, Category = "Board|Networking")
	float RenderDelaySeconds = 0.05f;

	// --- W3 real mesh -----------------------------------------------------------------------

	// Parent for every real-geometry part, attached at zero relative offset -- see mesh/README.md:
	// all seven STL parts and the wheel share one origin at the wheel axle, matching this actor's
	// own origin, so nothing here is individually hand-positioned.
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<USceneComponent> MeshAssemblyRoot;

	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UProceduralMeshComponent> FrontEnclosureMesh;
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UProceduralMeshComponent> RearEnclosureMesh;
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UProceduralMeshComponent> FrontBumperMesh;
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UProceduralMeshComponent> RearBumperMesh;
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UProceduralMeshComponent> FrontFootpadMesh;
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UProceduralMeshComponent> RearFootpadMesh;
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UProceduralMeshComponent> ElectronicsPlatformMesh;

	// Not an STL -- a MuJoCo primitive cylinder (radius 145.4mm, width 150mm; see mesh/README.md).
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UStaticMeshComponent> WheelMesh;

	// True once every part above has loaded and built successfully. While false, BoxMesh is the
	// visible fallback and the real-mesh components stay hidden -- see TryBuildRealMesh.
	bool bRealMeshLoaded = false;

	// --- Pint skin ---------------------------------------------------------------------------
	//
	// A COSMETIC SKIN, and the distinction is the whole point. MuJoCo simulates the Openwheel
	// geometry above and continues to; this only changes what is drawn. The two are not the same
	// vehicle -- the Pint deck is ~0.70 m against the 0.938 m Openwheel one -- so footage using
	// this skin may not be presented as a simulation result. See
	// Content/ThirdParty/PintPreview/NOTICE.md, which also covers the CC BY 4.0 attribution and
	// the trademark caveat that licence does not address.
	//
	// The cost of turning it on is real and worth stating: while the client renders exactly what
	// MuJoCo simulates, the render is a free visual check on the coordinate transform -- a board
	// that floats or sinks is a bug signal. With a differently-proportioned chassis that signal is
	// gone, because "wrong asset scale" and "pose stream offset" now look identical.
	UPROPERTY(EditAnywhere, Category = "Board|Skin")
	bool bUsePintSkin = true;

	UPROPERTY(VisibleAnywhere, Category = "Board|Skin")
	TObjectPtr<USceneComponent> PintAssemblyRoot;

	UPROPERTY(VisibleAnywhere, Category = "Board|Skin")
	TObjectPtr<UStaticMeshComponent> PintFrameMesh;
	UPROPERTY(VisibleAnywhere, Category = "Board|Skin")
	TObjectPtr<UStaticMeshComponent> PintWheelTireMesh;
	UPROPERTY(VisibleAnywhere, Category = "Board|Skin")
	TObjectPtr<UStaticMeshComponent> PintWheelHubMesh;

	// All three Pint parts resolved in the constructor. If false the skin is skipped entirely and
	// the Openwheel geometry stays visible -- same all-or-nothing rule as TryBuildRealMesh, for
	// the same reason: half a board reads as a rendering bug, not as graceful degradation.
	bool bPintSkinLoaded = false;

	// Zero by default, so OB_Main and every existing capture are bit-identical to before this
	// existed. Set by AOverboardGameMode from the level's PlayerStart, if it has one.
	UPROPERTY(EditAnywhere, Category = "Board|Level")
	FVector WorldOriginOffsetCm = FVector::ZeroVector;

	// Zero by default, so OB_Main and every existing capture are unchanged. See SetWorldOriginYawDeg.
	UPROPERTY(EditAnywhere, Category = "Board|Level")
	float WorldOriginYawDeg = 0.f;

	// --- Rider stand-in ------------------------------------------------------------------------
	//
	// CEO ask, after seeing an early capture (overboard#162): a board with nobody on it does not
	// read as being ridden, and a tall object shows lean/rotation far more legibly than a low
	// deck. Full honesty note in docs/mannequin-rider.md -- short version: the PHYSICS has a
	// rigid ballast on two slide joints, no legs, no arms, no articulation. Manny is a costume on
	// that lump. The pose (a stock idle animation, so he is not left T-posing -- the single most
	// damaging thing a placeholder rider could do in the launch footage) and the stance transform
	// (rotated to face across the board) are both INVENTED, not simulated. The fore/aft and
	// lateral OFFSET is real: wire v2's rider_fore_aft_m / rider_lateral_m, applied with NO
	// amplification -- a real ~2-4cm displacement renders as ~2-4cm, which is the honest thing to
	// do with it. If amplification is ever added for legibility, it is a new non-physical channel
	// and must be declared as such (overboard#163) -- this pass does not add one.
	UPROPERTY(EditAnywhere, Category = "Board|Rider")
	bool bShowRider = true;

	UPROPERTY(VisibleAnywhere, Category = "Board|Rider")
	TObjectPtr<USkeletalMeshComponent> RiderMesh;

	// True only if both the skeletal mesh AND the idle animation resolved in the constructor.
	// Same all-or-nothing rule as the Pint skin and the Openwheel mesh: a rider stuck in the
	// default bind/T-pose (no animation applied) is worse than no rider at all, so this is not a
	// graceful degradation -- see docs/mannequin-rider.md for the (very likely) cause, missing
	// Content/Characters/Mannequins/, and how to fix it.
	bool bRiderLoaded = false;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> RiderIdleAnim;

	// --- Riding animation (Fab pack "MonoWheel Board") -----------------------------------------
	//
	// Replaces the stock standing idle with an authored ASTRIDE RIDING STANCE, blended by two
	// values taken off the wire. This closes the "feet together, standing upright" gap
	// docs/mannequin-rider.md called out as unfixable without authoring a custom pose -- the pack
	// authored one, so nothing here is hand-animated.
	//
	// WHAT IS AND IS NOT SIMULATED, unchanged in spirit from the stock-idle case and restated
	// because a moving rider invites the assumption that the motion means something: the PHYSICS
	// still has a rigid ballast on two slide joints. No legs, no arms, no articulation, no balance
	// of its own. Every joint angle you see is the pack artist's invention. What is real is only
	// WHEN each pose is selected: the two blendspace axes are driven by wire values MuJoCo
	// actually computed (wheel rate, ballast lateral displacement), through two DECLARED gain
	// constants in BoardActor.cpp. The gains are a new non-physical channel and are declared as
	// such per overboard#163 -- see docs/rider-riding-animation.md.
	//
	// Default TRUE on this branch so a verification run needs no editor fiddling. Whether it stays
	// true on master is the merge decision, not this flag's.
	UPROPERTY(EditAnywhere, Category = "Board|Rider")
	bool bUseRidingAnim = true;

	UPROPERTY(Transient)
	TObjectPtr<UBlendSpace> RiderRidingBlendSpace;

	// True if the blendspace asset resolved in the constructor. NOT the same as bRidingAnimActive:
	// the asset can resolve and still fail to bind to the rider mesh (skeleton mismatch), which is
	// exactly the failure this design has to survive gracefully rather than T-pose through.
	bool bRidingAnimLoaded = false;

	// True only once the blendspace has been VERIFIED to be the mesh's live animation asset --
	// i.e. PlayAnimation was called AND the single-node instance actually took it. Set in
	// TryStartRidingAnim, read every tick before pushing blend parameters. If false, the rider is
	// on the stock idle (or absent) and no blend parameters are pushed at all.
	bool bRidingAnimActive = false;

	// --- Stance correction knobs: LIVE-EDITABLE EXPERIMENT, not settled values ------------------
	//
	// Measured 2026-08-04 and confirmed by eye: the pack's riding stance is an ELECTRIC UNICYCLE
	// stance, not a onewheel one. Its rider stands with both feet side by side on pedals either
	// side of a central wheel, facing along the direction of travel. A onewheel rider stands the
	// other way round -- feet fore and aft of the wheel, body square across the board. The
	// diagnostic put the feet at board-local Y = +24.4 / -23.4 with X identical, which is the
	// unicycle arrangement, hanging ~12 cm off each side of a 23.2 cm deck.
	//
	// Both knobs below are applied every tick, so they can be dragged in the Details panel during
	// PIE and judged immediately. NEITHER HAS BEEN CONFIRMED TO LOOK RIGHT -- the defaults are a
	// reasoned first guess and this may not converge at all (see the honest note in
	// docs/rider-riding-animation.md about the possibility that this pack simply does not fit a
	// onewheel).
	// +90. Both -90 and +90 put the feet on the footpads -- they are mirror images of each other,
	// the regular/goofy choice -- so only one of them faces the rider the way the board travels.
	//
	// -90 was chosen by eye on 2026-08-04 and that observation is VOID: it was made while
	// fake_sender --carve was driving the board tail-first, which put the chase camera ahead of
	// the travel direction and mirrored the horizontal axis. Under a mirrored view the wrong
	// member of a mirror-image pair is exactly what looks right. With the travel direction fixed,
	// -90 left the rider facing backwards down the road.
	//
	// The lesson is worth more than the constant: a value confirmed by eye is only as trustworthy
	// as the view it was confirmed in, and two of the three sign choices in this feature were
	// picked while looking through the same mirror.
	UPROPERTY(EditAnywhere, Category = "Board|Rider")
	float RiderRidingYawDeg = 90.f;

	// Yaw alone fixes WHERE THE FEET LAND but breaks WHICH WAY THE RIDER LEANS, because the
	// animation leans the body along its own facing. Turned 90 degrees, the pack's forward lean
	// points across our board instead of at its nose.
	//
	// Swapping which wire signal drives which blendspace axis restores the geometry: after the
	// yaw, the rider's body-forward axis is our board's lateral axis, and their body-lateral axis
	// is our board's direction of travel. So board SPEED should drive the animation's turn/lean
	// axis, and board LATERAL displacement should drive its forward/back axis.
	//
	// Geometrically coherent; whether it READS correctly is a different question, since the pack's
	// turn poses likely carry steering-specific upper-body twist that may look wrong standing in
	// for an acceleration lean. That is exactly what this toggle exists to let you find out.
	// FALSE, and this is an empirical correction to my own reasoning. I argued the swap was
	// geometrically necessary -- that a yawed rider would lean at right angles to the board. Play
	// testing said otherwise: unswapped reads correctly and swapped does not. The geometric
	// argument was about where the body's axes point; what actually governs how it reads is how
	// the pack authored the poses, which no amount of reasoning from bone positions was going to
	// tell me. Kept as a toggle because it costs nothing and the finding is worth being able to
	// re-check.
	UPROPERTY(EditAnywhere, Category = "Board|Rider")
	bool bSwapRidingAxes = false;

private:
	TUniquePtr<FBoardStateClient> StateClient;

	// Applies the interpolated, transformed pose to the actor. No-op if we don't yet have at
	// least two received samples.
	void UpdatePoseFromHistory();

	// Attempts to load and build all seven STL parts plus the wheel cylinder. Returns true only
	// if every single one succeeded; on any failure, tears down what it built and leaves BoxMesh
	// as the visible fallback -- partial real geometry (e.g. a board with no rear bumper) would
	// look like a rendering bug, not a graceful degradation, so this is all-or-nothing.
	bool TryBuildRealMesh();

	// Loads Meshes/openwheel/<StlBaseName>.stl and builds it into Component as one procedural
	// mesh section. ExtraYawDeg applies an additional local Z rotation on top of the shared
	// zero-offset placement (only front_footpad needs this -- see mesh/README.md). Returns false
	// and logs the parse error on failure. Every processed vertex also extends InOutLocalBounds,
	// in the actor's local space (Meshes/README.md's mm->cm + Y-mirror conversion already
	// applied), so TryBuildRealMesh can log and arithmetically check the whole assembly's size
	// instead of asking a human to eyeball it on screen.
	bool BuildPartFromStl(UProceduralMeshComponent* Component, const FString& StlBaseName, float ExtraYawDeg, FBox& InOutLocalBounds);

	// Binds the riding blendspace to the rider mesh and confirms it took. Returns false (leaving
	// the caller to fall back to the stock idle) on any of: no blendspace, no rider mesh, no
	// skeleton, or the single-node instance not actually holding the blendspace afterwards.
	//
	// The pack's sequences are bound to the pack's OWN copy of Epic's SK_Mannequin, while the
	// rider mesh uses the UE 5.7 template's copy -- two distinct assets with identical bone
	// hierarchies. USkeleton::AddCompatibleSkeleton (a runtime UFUNCTION, not editor-only) is what
	// lets one play on the other without an editor retarget step, and it is registered BOTH
	// directions because which side the engine's compatibility check interrogates is an engine
	// implementation detail this code should not be betting on.
	bool TryStartRidingAnim();

	// Pushes this tick's two blend parameters. No-op unless bRidingAnimActive. Separated from
	// UpdatePoseFromHistory so the "what drives the animation" mapping -- the one genuinely new
	// piece of physics-to-visual arithmetic in this actor -- reads as one function rather than as
	// four lines buried in the pose interpolator.
	void UpdateRidingAnimParams();

	// Manual trim on top of the measured root lift, cm. Positive raises the rider.
	//
	// Exists because kRidingAnimRootLiftCm was measured from ONE pose (the centre of the
	// blendspace) and the rider's lowest foot is not at the same height in every pose -- turning
	// drops a foot through the deck. Rather than re-measure by hand per pose, the placement
	// diagnostic now tracks the running MINIMUM foot height across a whole session, so a --carve
	// run reports the true worst case and this trim can be set from it once.
	UPROPERTY(EditAnywhere, Category = "Board|Rider")
	float RiderRidingHeightTrimCm = 0.f;

	// Uniform visual scale on the rider. 0.746 is DERIVED, not dialled in by eye:
	//
	//     0.746 = Pint deck length (0.70 m) / Openwheel deck length (0.938 m)
	//
	// Scaling the rider by the same ratio the SKIN is undersized by makes the rider-to-board
	// proportion on screen exactly match a real rider on the real board. A first pass used 0.85,
	// which was a guess and left the rider visibly too big; the derived figure is what the eye was
	// actually asking for. It also pulls the feet from ~40.6 cm apart to ~35.7 cm, which is what
	// stops the rear foot hanging off the end of a 70 cm deck at full lean.
	//
	// PURELY COSMETIC and it has to stay that way: the rider avatar contributes no mass, no
	// inertia and no dynamics, so its size cannot reach any physics value. But be clear about what
	// this is compensating for. A 170 cm rider on the 0.70 m Pint SKIN looks too big largely
	// because the skin is ~25% shorter than the 0.938 m Openwheel board MuJoCo actually simulates.
	// Shrinking the rider makes the pair look right by moving FURTHER from the simulated geometry,
	// not closer -- at 0.746 the rider is ~127 cm, a child's height against the real board.
	//
	// This was put side by side against bUsePintSkin=false with an unscaled rider, which is the
	// honest alternative and needs no compensation at all. The Pint was chosen on looks, knowingly:
	// footage from this client already carries its own provenance category, and nothing here can
	// reach a physics value. The cost is real and is the one bUsePintSkin's own comment names --
	// with a wrongly-proportioned chassis, "wrong asset scale" and "pose stream offset" become
	// indistinguishable, so the free floats/sinks bug signal is gone. See
	// docs/rider-riding-animation.md.
	UPROPERTY(EditAnywhere, Category = "Board|Rider", meta = (ClampMin = "0.5", ClampMax = "1.5"))
	float RiderScale = 0.746f;

	// Fraction of the pack's authored lean range the rider is allowed to reach at full command.
	//
	// The lean stays strictly PROPORTIONAL to the wire signal -- this scales the whole response,
	// it does not clip the top of it, so there is no dead band and no kink. It exists because the
	// pack authored its lean for a unicycle that banks hard, and against the modest yaw our carve
	// actually achieves the full range read as a rider throwing themselves over far more than the
	// turn justified.
	//
	// A THIRD DECLARED NON-PHYSICAL GAIN, on the same footing as the two in BoardActor.cpp: it
	// changes how much authored pose a given real displacement buys. Declared in
	// docs/rider-riding-animation.md.
	UPROPERTY(EditAnywhere, Category = "Board|Rider", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float RidingMaxLeanFraction = 0.45f;

	// Where the rider stands on the deck, cm, in the BOARD's local frame -- X fore/aft, Y across.
	// Added on top of the real ballast displacement, which is untouched and still un-amplified.
	//
	// Y = -3.0 is MEASURED, not chosen: the riding animation puts the ankles at board-local
	// Y = +3.4 / +2.5, so the authored stance sits ~3 cm off the deck's centreline. That bias
	// spends 3 cm of a deck only ~23 cm wide, and it is spent on the side the heel hangs over.
	// Cancelling it is the cheapest part of the overhang fix and needs no judgement call.
	//
	// It does NOT fully solve heel overhang and cannot: a ~25 cm foot on a ~23 cm deck overhangs
	// somewhere by construction. The rest is what the lean-dip compensation below is for.
	UPROPERTY(EditAnywhere, Category = "Board|Rider")
	FVector2D RiderRidingStanceOffsetCm = FVector2D(0.f, -3.0f);

	// Raises the rider in proportion to how hard they are leaning, cm at FULL commanded lean.
	//
	// This is the fix a static trim cannot be. The pack's turn poses drop a foot below where the
	// neutral pose puts it -- measured at -8.7 cm at 0.55 lean -- so a constant big enough to keep
	// the feet out of the deck through a turn would leave the rider floating that far above it at
	// rest. Scaling the lift with lean magnitude plants the feet at BOTH ends.
	//
	// A cheap approximation of the foot IK this pass deliberately skipped. It is cosmetic: it moves
	// the avatar only, and the avatar has no mass, inertia or dynamics.
	UPROPERTY(EditAnywhere, Category = "Board|Rider")
	float RiderRidingLeanDipCompCm = 7.0f;

	// Base height of the rider component above the actor origin, cm. Differs by tier: the riding
	// animation carries its own ~31 cm root lift that the stock standing idle does not, so one
	// shared constant cannot plant both poses. See kRidingAnimRootLiftCm.
	float GetRiderBaseHeightCm() const;

	// Newest raw wheel rate, rad/s, straight off the wire -- drives the blendspace's forward axis
	// via the wheel radius. Same newest-raw-sample rule as the ballast displacements below.
	float LatestWheelRateRadS = 0.f;

	// Blendspace axis ranges, read off the ASSET in TryStartRidingAnim rather than hardcoded.
	// The pack authored these; this code has never assumed what they are, and logs them on start
	// so the numbers appear in the Output Log instead of in a comment that could go stale.
	FVector2D RidingAxisMin = FVector2D::ZeroVector;
	FVector2D RidingAxisMax = FVector2D::ZeroVector;

	// Throttle for the once-a-second blend-parameter trace (checkpoint C1). Per-tick logging of
	// two floats at 60+ fps is how an Output Log becomes unreadable during the exact session it
	// is needed for.
	double LastRidingTraceLogTimeSeconds = 0.0;

	// One-shot guard for the rider placement measurement -- see UpdateRidingAnimParams. Fires on
	// the first tick the skeleton is actually posed, not in BeginPlay, because bone transforms
	// before the first animation evaluation describe the bind pose and would quietly lie.
	bool bLoggedRiderPlacementDiagnostic = false;

	// Yaw the placement diagnostic last reported against, so dragging RiderRidingYawDeg re-arms it.
	float LastDiagnosticYawDeg = 0.f;

	// Running minimum of the rider's lowest foot height, actor-local cm, across the whole PIE
	// session. A single-pose measurement cannot see the turn poses that clip; this can.
	float MinFootZObservedCm = TNumericLimits<float>::Max();

	// ...and the HIGHEST. A foot floating above the deck is what reads as "the back foot comes off
	// the board", and tracking only the minimum was blind to it.
	float MaxFootZObservedCm = TNumericLimits<float>::Lowest();

	// |commanded lean| as a 0..1 fraction of RidingMaxLeanFraction, updated each tick and read by
	// GetRiderBaseHeightCm to drive the lean-proportional lift.
	float LastLeanFractionOfMax = 0.f;

	bool bLatestSampleFallen = false;

	// See IsAuthorityWarning(). Reset to false (and the timestamp to 0) the moment the signal
	// clears, so a stale arrival time can never be reported against a later event -- the HUD's
	// own hold is what keeps the banner up past the signal, not a lingering value here.
	bool bLatestSampleAuthorityWarning = false;
	double AuthorityWarningArrivalTimeSeconds = 0.0;

	// Newest raw rider ballast displacement, metres, MuJoCo frame -- same "newest sample, not
	// the interpolated render pose" reasoning as bLatestSampleFallen. 0 on a v1 packet, which is
	// the documented v1 neutral-rider behaviour (wire/OverboardWire.h), not a sentinel.
	float LatestRiderForeAftM = 0.f;
	float LatestRiderLateralM = 0.f;
};
