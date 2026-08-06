#include "BoardActor.h"

#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "ProceduralMeshComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "CoordinateTransform.h"
#include "StlLoader.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogOverboardMesh, Log, All);

namespace
{
	// Deck-top height above the actor origin, cm -- the COO's directly-stated measurement
	// (overboard#162, corrected from an earlier internally-computed 5.8cm after real footage
	// showed the rider hovering above/behind the deck). One named constant, used everywhere the
	// rider's height is set, specifically so the constructor's base pose and
	// UpdatePoseFromHistory's per-tick offset cannot drift apart the way they briefly did here.
	constexpr float kRiderDeckHeightCm = 8.3f;

	// Wheel radius, metres -- the MuJoCo primitive cylinder the plant model actually simulates
	// (145.4 mm; see mesh/README.md, same figure WheelMesh is built at). Turns the wire's
	// wheel_rate_rad_s into ground speed. This one is NOT a tuning knob: it is a property of the
	// simulated vehicle, and if it ever disagrees with the model it is a bug, not a preference.
	constexpr float kWheelRadiusM = 0.1454f;

	// --- DECLARED NON-PHYSICAL GAINS ------------------------------------------------------------
	//
	// These two constants are a new non-physical channel and are declared as one, per the standing
	// rule in docs/mannequin-rider.md and the channel declaration at overboard#163. Read that rule
	// before touching them: the fore/aft and lateral rider OFFSET is deliberately applied with no
	// amplification at all, and these gains do NOT change that -- the offset code below is
	// untouched. What they scale is only WHICH AUTHORED POSE the blendspace selects.
	//
	// Why a gain is unavoidable here: the blendspace's axes are in the pack author's units, chosen
	// for their vehicle, and our signals are in SI units from MuJoCo. Something has to map one onto
	// the other, and a mapping with no declared reference point is just an undeclared gain with
	// extra steps. So both are stated as "the value at which the rider reaches FULL authored lean",
	// which is a claim a reader can disagree with and re-tune, rather than a magic multiplier.
	//
	// Neither figure is measured -- they are legibility choices, and the honest thing is to say so.
	// kRidingFullLeanSpeedMs is set well above the ~1.1 m/s the host currently starts at so normal
	// riding does not sit pinned at the axis limit. kRidingFullLeanLateralM is the COO's stated
	// "full lateral" ballast displacement, the same 4 cm figure fake_sender --rider uses.
	constexpr float kRidingFullLeanSpeedMs = 5.0f;
	constexpr float kRidingFullLeanLateralM = 0.04f;

	// MEASURED, 2026-08-04, by the placement diagnostic in UpdateRidingAnimParams: with the rider
	// component at kRiderDeckHeightCm, the riding animation's lowest foot sat 39.5 cm above the
	// actor origin, i.e. 31.2 cm above the deck top it should be standing on.
	//
	// This is a property of the ANIMATION, not of our board, and that is why it cannot be folded
	// into kRiderDeckHeightCm: the stock standing idle puts the mannequin's feet at the mesh
	// origin, so sharing one constant would plant the riding pose correctly and bury the idle pose
	// 31 cm underground. The two poses genuinely have different root-to-foot distances and need
	// two numbers.
	//
	// The size of it is the tell. 31 cm is not a tuning discrepancy, it is the pedal height of a
	// self-balancing unicycle -- see the stance note in docs/rider-riding-animation.md.
	constexpr float kRidingAnimRootLiftCm = 31.2f;

	// Maps a normalised [-1, 1] signal onto one authored blendspace axis.
	//
	// Handles both axis conventions the pack could plausibly have used, because this code reads
	// the ranges off the asset at runtime rather than assuming them: a SIGNED axis (Min < 0, e.g.
	// turn: full-left .. centre .. full-right) maps symmetrically about the midpoint, and an
	// UNSIGNED axis (Min >= 0, e.g. speed: 0 .. max) maps the positive half only. Getting this
	// wrong the other way would park a signed axis at its minimum and look like a stuck animation.
	float MapNormalisedToAxis(float Normalised, float AxisMin, float AxisMax)
	{
		if (AxisMin < 0.f)
		{
			const float Mid = 0.5f * (AxisMin + AxisMax);
			const float Half = 0.5f * (AxisMax - AxisMin);
			return FMath::Clamp(Mid + Normalised * Half, AxisMin, AxisMax);
		}
		return FMath::Clamp(AxisMin + FMath::Clamp(Normalised, 0.f, 1.f) * (AxisMax - AxisMin), AxisMin, AxisMax);
	}
}

ABoardActor::ABoardActor()
{
	PrimaryActorTick.bCanEverTick = true;

	// Identity-scale actor root. BoxMesh and MeshAssemblyRoot attach here as SIBLINGS -- see the
	// header comment on SceneRoot for the scale bug this exists to prevent from recurring.
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	BoxMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BoxMesh"));
	BoxMesh->SetupAttachment(SceneRoot);

	// Placeholder for the real board model. As of W3 the real geometry (below) is the primary
	// visual; this stays as the fallback if it fails to load at runtime -- see class header.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		BoxMesh->SetStaticMesh(CubeFinder.Object);
	}
	// Board-ish proportions rather than a 1m cube: long, thin, low. This scale is BoxMesh's own
	// and must never propagate anywhere else -- it very nearly did once (see SceneRoot comment).
	BoxMesh->SetRelativeScale3D(FVector(0.7f, 0.25f, 0.08f));

	// THE RULE: this application computes no board physics. Collision and gravity are off --
	// the transform comes entirely from wire state.
	BoxMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BoxMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	BoxMesh->SetSimulatePhysics(false);
	BoxMesh->SetEnableGravity(false);
	BoxMesh->SetMobility(EComponentMobility::Movable);

	// --- W3 real mesh: built at runtime from STL, see mesh/README.md ------------------------
	// Sibling of BoxMesh, both attached directly to the identity-scale SceneRoot -- NOT to
	// BoxMesh or to each other, so the placeholder's cosmetic scale cannot reach this hierarchy.
	MeshAssemblyRoot = CreateDefaultSubobject<USceneComponent>(TEXT("MeshAssemblyRoot"));
	MeshAssemblyRoot->SetupAttachment(SceneRoot);
	MeshAssemblyRoot->SetVisibility(false, true); // hidden until TryBuildRealMesh succeeds

	auto MakePart = [this](const TCHAR* Name) -> UProceduralMeshComponent*
	{
		UProceduralMeshComponent* Part = CreateDefaultSubobject<UProceduralMeshComponent>(Name);
		Part->SetupAttachment(MeshAssemblyRoot);
		return Part;
	};
	FrontEnclosureMesh = MakePart(TEXT("FrontEnclosureMesh"));
	RearEnclosureMesh = MakePart(TEXT("RearEnclosureMesh"));
	FrontBumperMesh = MakePart(TEXT("FrontBumperMesh"));
	RearBumperMesh = MakePart(TEXT("RearBumperMesh"));
	FrontFootpadMesh = MakePart(TEXT("FrontFootpadMesh"));
	RearFootpadMesh = MakePart(TEXT("RearFootpadMesh"));
	ElectronicsPlatformMesh = MakePart(TEXT("ElectronicsPlatformMesh"));

	WheelMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WheelMesh"));
	WheelMesh->SetupAttachment(MeshAssemblyRoot);

	// --- Pint skin: a third sibling of SceneRoot, never a child of the other two -------------
	// Same isolation rule as MeshAssemblyRoot: nothing inherits BoxMesh's cosmetic scale.
	PintAssemblyRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PintAssemblyRoot"));
	PintAssemblyRoot->SetupAttachment(SceneRoot);
	PintAssemblyRoot->SetVisibility(false, true); // shown in BeginPlay only if all parts resolved

	// No scale conversion and no rotation, and both are measured facts rather than assumptions.
	// The prepared FBX is authored in metres with its origin ON THE AXLE, and Unreal's FBX import
	// lands it at centimetre scale directly: the tyre measures 29.08 cm across in world units at
	// scale 1.0, against the 2 x 14.54 cm the MJCF specifies -- a correction factor of 0.99992,
	// i.e. none. Blender's -Z-forward/Y-up export already applied the handedness conversion, so
	// unlike the raw Openwheel STLs (see mesh/README.md) this needs no manual Y-mirror.
	auto MakePintPart = [this](const TCHAR* Name, const TCHAR* AssetPath) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* Part = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Part->SetupAttachment(PintAssemblyRoot);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetCollisionResponseToAllChannels(ECR_Ignore);
		Part->SetSimulatePhysics(false);
		Part->SetEnableGravity(false);
		Part->SetMobility(EComponentMobility::Movable);
		return Part;
	};
	PintFrameMesh     = MakePintPart(TEXT("PintFrameMesh"),     TEXT(""));
	PintWheelTireMesh = MakePintPart(TEXT("PintWheelTireMesh"), TEXT(""));
	PintWheelHubMesh  = MakePintPart(TEXT("PintWheelHubMesh"),  TEXT(""));

	// FObjectFinder is legal HERE and only here -- this is a constructor. It asserts if used from
	// BeginPlay, which is exactly the crash the ground plane hit (see OverboardGameMode.cpp).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PintFrameFinder(TEXT("/Game/ThirdParty/PintPreview/OneWheelPint_prepared_OW_Frame.OneWheelPint_prepared_OW_Frame"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PintTireFinder(TEXT("/Game/ThirdParty/PintPreview/OneWheelPint_prepared_OW_Wheel_Tire.OneWheelPint_prepared_OW_Wheel_Tire"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PintHubFinder(TEXT("/Game/ThirdParty/PintPreview/OneWheelPint_prepared_OW_Wheel_Hub.OneWheelPint_prepared_OW_Wheel_Hub"));
	bPintSkinLoaded = PintFrameFinder.Succeeded() && PintTireFinder.Succeeded() && PintHubFinder.Succeeded();
	if (bPintSkinLoaded)
	{
		PintFrameMesh->SetStaticMesh(PintFrameFinder.Object);
		PintWheelTireMesh->SetStaticMesh(PintTireFinder.Object);
		PintWheelHubMesh->SetStaticMesh(PintHubFinder.Object);
	}

	// --- Rider stand-in: a fourth sibling of SceneRoot, same isolation rule as the others ------
	// See docs/mannequin-rider.md for the full honesty note; short version in the header comment.
	RiderMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("RiderMesh"));
	RiderMesh->SetupAttachment(SceneRoot);
	RiderMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RiderMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	RiderMesh->SetSimulatePhysics(false);
	RiderMesh->SetEnableGravity(false);
	RiderMesh->SetMobility(EComponentMobility::Movable);
	RiderMesh->SetVisibility(false, true); // shown in BeginPlay only if mesh AND animation resolved

	// Base stance transform, corrected once against real footage (overboard#162 -- first attempt
	// had him facing down the road, arms out like a snowboarder; second attempt below).
	//
	// FACING: the first attempt applied an extra +90 degree yaw on the assumption Manny's bind
	// pose faces the mesh's own local +X. That assumption was the bug. The UE mannequin's actual
	// bind-pose forward is +Y in its own component space -- this is *why* the standard
	// ThirdPerson-template convention gives the mesh component a -90 degree yaw relative to its
	// (capsule-forward = +X) owner: rotating a +Y-facing bind pose by -90 lands it on +X. So at
	// RELATIVE YAW 0 (no correction at all), Manny already faces this actor's local +Y -- which
	// is exactly "across the board" (lateral), since the board's direction of travel is its local
	// X axis (nose at -X). The earlier +90 rotated that already-correct +Y-facing bind pose onto
	// -X, i.e. facing the board's NOSE -- precisely "facing down the road", matching what was
	// seen. Fix: no additional yaw at all.
	//
	// HEIGHT: corrected from an internally-computed 5.8cm to the COO's directly-stated ~8.3cm
	// deck-top height, per real footage showing the board sitting below and behind his feet.
	//
	// Both of these were corrected by REASONING about the mannequin's known bind-pose convention
	// and the COO's stated measurement, not by seeing it -- still no display in this environment.
	// If footage says otherwise, that observation wins; see docs/mannequin-rider.md.
	RiderMesh->SetRelativeLocation(FVector(0.f, 0.f, kRiderDeckHeightCm));
	RiderMesh->SetRelativeRotation(FRotator::ZeroRotator);

	// /Game/Characters/Mannequins/, NOT /Game/Mannequins/, and the extra directory level is
	// load-bearing rather than cosmetic. Epic's template mannequin packages record their own
	// object paths as /Game/Characters/Mannequins/... internally, so importing them one level
	// higher leaves every INTERNAL reference dangling while the top-level asset still loads
	// happily by file path. The visible result is a mesh that resolves here, passes the
	// bRiderLoaded check, renders -- and has a NULL skeleton, because SKM_Manny_Simple's hard
	// reference to its own SK_Mannequin cannot be found.
	//
	// A skeletal mesh with a null skeleton silently ignores PlayAnimation. That is how the rider
	// stood in bind pose through every capture up to this point while the log cheerfully reported
	// an idle animation playing, and it is very likely what the first footage note in
	// docs/mannequin-rider.md was actually describing as "arms out like a snowboarder" -- an
	// A-pose, not a facing error. The materials were dangling for the same reason.
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> RiderMeshFinder(TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"));
	static ConstructorHelpers::FObjectFinder<UAnimSequence> RiderIdleAnimFinder(TEXT("/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle.MM_Idle"));
	bRiderLoaded = RiderMeshFinder.Succeeded() && RiderIdleAnimFinder.Succeeded();
	if (bRiderLoaded)
	{
		RiderMesh->SetSkeletalMesh(RiderMeshFinder.Object);
		RiderIdleAnim = RiderIdleAnimFinder.Object; // PlayAnimation happens in BeginPlay, same as the mesh visibility below
	}

	// Riding blendspace from the Fab pack -- optional by construction. Content/MonoWheel_Board/ is
	// gitignored (licence, same as Content/Characters/Mannequins/), so a fresh clone resolves nothing here and
	// must still run: bRidingAnimLoaded false simply leaves the rider on the stock idle. Note this
	// is a SOFT dependency in a way the rider mesh is not -- failing to find it is an expected
	// state, not an error, and is logged as such in BeginPlay.
	static ConstructorHelpers::FObjectFinder<UBlendSpace> RidingBlendSpaceFinder(
		TEXT("/Game/MonoWheel_Board/Animations/UE5/MonoWheel_Board_Riding_BS.MonoWheel_Board_Riding_BS"));
	bRidingAnimLoaded = RidingBlendSpaceFinder.Succeeded();
	if (bRidingAnimLoaded)
	{
		RiderRidingBlendSpace = RidingBlendSpaceFinder.Object;
	}
}

bool ABoardActor::TryStartRidingAnim()
{
	if (!RiderRidingBlendSpace || !RiderMesh)
	{
		return false;
	}

	USkeletalMesh* const MeshAsset = RiderMesh->GetSkeletalMeshAsset();
	USkeleton* const MeshSkeleton = MeshAsset ? MeshAsset->GetSkeleton() : nullptr;
	USkeleton* const AnimSkeleton = RiderRidingBlendSpace->GetSkeleton();
	if (!MeshSkeleton || !AnimSkeleton)
	{
		// Distinguish the three failures explicitly. Collapsing them into one "something was null"
		// line cost a diagnostic round-trip once already: a mesh that loads but whose SKELETON is
		// null is a dangling-internal-reference symptom (wrong import path -- see the finder above)
		// and looks nothing like a mesh that simply is not there, but they logged identically.
		if (!MeshAsset)
		{
			UE_LOG(LogOverboardMesh, Warning, TEXT("ABoardActor: riding animation skipped -- the rider component has NO skeletal mesh asset at all."));
		}
		else if (!MeshSkeleton)
		{
			UE_LOG(LogOverboardMesh, Error, TEXT("ABoardActor: riding animation skipped -- rider mesh '%s' loaded but its SKELETON IS NULL. That means its internal asset references are dangling, which almost always means the mannequin content was imported to the wrong path: it must be Content/Characters/Mannequins/ (the packages record themselves as /Game/Characters/Mannequins/...), NOT Content/Mannequins/. NOTE: this also means the stock idle is not playing either and the rider is in BIND POSE -- see docs/mannequin-rider.md."),
				*MeshAsset->GetName());
		}
		else
		{
			UE_LOG(LogOverboardMesh, Warning, TEXT("ABoardActor: riding animation skipped -- the riding blendspace has no skeleton."));
		}
		return false;
	}

	// Cross-register compatibility unless they are literally the same asset. See the header note:
	// both directions, deliberately, because the engine's runtime compatibility check is not a
	// contract this code should assume the direction of.
	if (MeshSkeleton != AnimSkeleton)
	{
		MeshSkeleton->AddCompatibleSkeleton(AnimSkeleton);
		AnimSkeleton->AddCompatibleSkeleton(MeshSkeleton);
		UE_LOG(LogOverboardMesh, Log, TEXT("ABoardActor: registered skeleton compatibility %s <-> %s for the riding animation."),
			*MeshSkeleton->GetName(), *AnimSkeleton->GetName());
	}

	RiderMesh->PlayAnimation(RiderRidingBlendSpace, /*bLooping=*/true);

	// VERIFY it took rather than assuming. PlayAnimation returns void and declines silently on a
	// skeleton it will not accept -- and the silent-decline case is precisely a rider left in the
	// bind pose, the one outcome docs/mannequin-rider.md calls the most damaging thing a
	// placeholder rider can do. So the success of this function is defined by what the component
	// is actually holding afterwards, not by having called the setter.
	UAnimSingleNodeInstance* const SingleNode = RiderMesh->GetSingleNodeInstance();
	if (!SingleNode || SingleNode->GetAnimationAsset() != RiderRidingBlendSpace)
	{
		UE_LOG(LogOverboardMesh, Warning, TEXT("ABoardActor: riding blendspace did not bind to the rider mesh (skeletons %s / %s incompatible at runtime); falling back to the stock idle."),
			*MeshSkeleton->GetName(), *AnimSkeleton->GetName());
		return false;
	}

	// Axis ranges come from the asset, never from a hardcoded guess -- and go straight to the log
	// so the authored numbers are visible in the session that needs them.
	const FBlendParameter& AxisX = RiderRidingBlendSpace->GetBlendParameter(0);
	const FBlendParameter& AxisY = RiderRidingBlendSpace->GetBlendParameter(1);
	RidingAxisMin = FVector2D(AxisX.Min, AxisY.Min);
	RidingAxisMax = FVector2D(AxisX.Max, AxisY.Max);
	UE_LOG(LogOverboardMesh, Log, TEXT("ABoardActor: riding blendspace bound. Axis0 '%s' [%.2f..%.2f], Axis1 '%s' [%.2f..%.2f]. Gains: full lean at %.2f m/s fore/aft, %.3f m lateral (DECLARED non-physical -- docs/rider-riding-animation.md)."),
		*AxisX.DisplayName, AxisX.Min, AxisX.Max,
		*AxisY.DisplayName, AxisY.Min, AxisY.Max,
		kRidingFullLeanSpeedMs, kRidingFullLeanLateralM);
	return true;
}

float ABoardActor::GetRiderBaseHeightCm() const
{
	return bRidingAnimActive
		? (kRiderDeckHeightCm - kRidingAnimRootLiftCm + RiderRidingHeightTrimCm)
		: kRiderDeckHeightCm;
}

void ABoardActor::UpdateRidingAnimParams()
{
	if (!bRidingAnimActive || !RiderMesh)
	{
		return;
	}

	// Applied every tick rather than once in BeginPlay, deliberately: it makes the stance knobs
	// draggable in the Details panel mid-PIE. This is an unsettled experiment and the cost of
	// re-setting a rotation each frame is nothing against the cost of a rebuild per attempt.
	RiderMesh->SetRelativeRotation(FRotator(0.f, RiderRidingYawDeg, 0.f));

	UAnimSingleNodeInstance* const SingleNode = RiderMesh->GetSingleNodeInstance();
	if (!SingleNode)
	{
		return;
	}

	// Ground speed from the wheel rate MuJoCo computed. Signed: reverse drives the backward pose,
	// which is the whole reason the pack ships a Backward sequence.
	const float SpeedMs = LatestWheelRateRadS * kWheelRadiusM;
	const float ForwardNormalised = FMath::Clamp(SpeedMs / kRidingFullLeanSpeedMs, -1.f, 1.f);

	// Turn comes from the REAL simulated ballast lateral displacement, not from the player's steer
	// stick. Steering is already declared a non-physical game channel, so feeding the rider's lean
	// from the stick would show intent rather than what the board did -- and the two differ exactly
	// when it matters, e.g. a steer command the controller could not honour. The sign carries the
	// same local-space Y-mirror convention as the offset code below and everything else attached to
	// this actor (mesh/README.md).
	//
	// SIGN: positive here selects the pack's LEFT poses. Which way the pack authored Left_1/2/3 to
	// lean is a property of the art, not something derivable from the wire convention -- so this
	// sign can only be settled by watching it, and it was settled on 2026-08-06 once the board was
	// finally travelling nose-first with the rider facing forward. Before that every observation
	// of it came through the tail-first mirror and was worthless.
	//
	// It is deliberately NOT the same sign as the rider's lateral OFFSET a few lines below, which
	// keeps the -1 because it maps a MuJoCo displacement onto this actor's mirrored local Y (see
	// mesh/README.md). Those two are different questions -- one is a coordinate convention, the
	// other is an art convention -- and making them agree for tidiness would break one of them.
	const float TurnNormalised = FMath::Clamp(LatestRiderLateralM / kRidingFullLeanLateralM, -1.f, 1.f);

	// Which axis is which is NOT assumed: the pack named them, and TryStartRidingAnim logged the
	// names. Axis0 is the horizontal (Turn) axis and Axis1 the vertical (Forward) axis, which is
	// the blendspace convention and matches the names dumped from the asset.
	//
	// bSwapRidingAxes exchanges which signal reaches which axis -- see the header note. With the
	// rider yawed 90 degrees to stand across the board, their body-forward axis IS our lateral
	// axis and their body-lateral axis IS our direction of travel, so the drivers have to swap
	// with them or the rider leans at right angles to what the board is doing.
	const float TurnAxisDriver = bSwapRidingAxes ? ForwardNormalised : TurnNormalised;
	const float ForwardAxisDriver = bSwapRidingAxes ? TurnNormalised : ForwardNormalised;

	const float AxisXValue = MapNormalisedToAxis(TurnAxisDriver, RidingAxisMin.X, RidingAxisMax.X);
	const float AxisYValue = MapNormalisedToAxis(ForwardAxisDriver, RidingAxisMin.Y, RidingAxisMax.Y);
	SingleNode->SetBlendSpacePosition(FVector(AxisXValue, AxisYValue, 0.f));

	// One-shot placement diagnostic. First real footage showed the rider and the board plainly not
	// belonging to each other, and "looks about a foot too high" is not a number anyone can fix a
	// constant with. Same convention TryBuildRealMesh already uses on the board geometry: measure
	// it, print it, check the arithmetic -- rather than asking a human to judge a distance by eye
	// in a perspective projection, which is exactly the judgement a screenshot is worst at.
	//
	// Deferred to the first tick with a posed skeleton rather than done in BeginPlay: bone
	// transforms are meaningless until the animation has evaluated at least once, and reading them
	// too early would report the bind pose while claiming to describe the riding pose.
	// Running minimum of the lowest foot, tracked EVERY tick rather than sampled once.
	//
	// kRidingAnimRootLiftCm was measured from a single pose, and that is exactly why feet dive
	// under the deck while turning: the blendspace's turn poses drop a foot lower than the centre
	// pose it was measured from. One sample cannot see that; a running minimum over a --carve
	// sweep can, and reports the trim needed to clear the worst case rather than the average one.
	if (RiderMesh->GetNumBones() > 0)
	{
		const int32 FootLIdx = RiderMesh->GetBoneIndex(TEXT("foot_l"));
		const int32 FootRIdx = RiderMesh->GetBoneIndex(TEXT("foot_r"));
		if (FootLIdx != INDEX_NONE && FootRIdx != INDEX_NONE)
		{
			const FTransform ToActor = GetActorTransform();
			const float LowestNow = FMath::Min(
				ToActor.InverseTransformPosition(RiderMesh->GetBoneLocation(TEXT("foot_l"), EBoneSpaces::WorldSpace)).Z,
				ToActor.InverseTransformPosition(RiderMesh->GetBoneLocation(TEXT("foot_r"), EBoneSpaces::WorldSpace)).Z);

			// Report only on a meaningful new low, so a carve sweep prints a short descending
			// series ending at the worst case instead of a line per frame.
			if (LowestNow < MinFootZObservedCm - 0.5f)
			{
				MinFootZObservedCm = LowestNow;
				const float ClearanceCm = LowestNow - kRiderDeckHeightCm;
				UE_LOG(LogOverboardMesh, Warning, TEXT("ABoardActor RIDER FEET: new lowest foot %.1f cm (deck top %.1f cm) -> clearance %+.1f cm.%s"),
					LowestNow, kRiderDeckHeightCm, ClearanceCm,
					ClearanceCm < 0.f
						? TEXT(" FOOT IS THROUGH THE DECK -- raise RiderRidingHeightTrimCm by at least this much.")
						: TEXT(""));
			}
		}
	}

	// Re-arms whenever the yaw knob moves, so dragging it in the Details panel produces a fresh
	// measurement per attempt instead of one stale reading from the first configuration tried.
	if (!FMath::IsNearlyEqual(RiderRidingYawDeg, LastDiagnosticYawDeg))
	{
		LastDiagnosticYawDeg = RiderRidingYawDeg;
		bLoggedRiderPlacementDiagnostic = false;
		MinFootZObservedCm = TNumericLimits<float>::Max(); // else the previous yaw's worst case leaks in
	}

	if (!bLoggedRiderPlacementDiagnostic && RiderMesh->GetNumBones() > 0)
	{
		const int32 FootLIndex = RiderMesh->GetBoneIndex(TEXT("foot_l"));
		const int32 FootRIndex = RiderMesh->GetBoneIndex(TEXT("foot_r"));
		if (FootLIndex != INDEX_NONE && FootRIndex != INDEX_NONE)
		{
			bLoggedRiderPlacementDiagnostic = true;

			// Everything in the ACTOR's local frame, because that is the frame kRiderDeckHeightCm
			// is expressed in and therefore the only frame in which the fix is a single number.
			const FTransform ActorToWorld = GetActorTransform();
			const FVector FootLLocal = ActorToWorld.InverseTransformPosition(RiderMesh->GetBoneLocation(TEXT("foot_l"), EBoneSpaces::WorldSpace));
			const FVector FootRLocal = ActorToWorld.InverseTransformPosition(RiderMesh->GetBoneLocation(TEXT("foot_r"), EBoneSpaces::WorldSpace));
			const float LowestFootZ = FMath::Min(FootLLocal.Z, FootRLocal.Z);
			const float FootSeparationY = FMath::Abs(FootLLocal.Y - FootRLocal.Y);

			const FBoxSphereBounds RiderBounds = RiderMesh->CalcBounds(RiderMesh->GetComponentTransform());
			const float RiderHeightCm = RiderBounds.BoxExtent.Z * 2.f;

			const float FootSeparationX = FMath::Abs(FootLLocal.X - FootRLocal.X);

			UE_LOG(LogOverboardMesh, Warning, TEXT("ABoardActor RIDER PLACEMENT (yaw %.0f deg): lowest foot sits %.1f cm above the actor origin; deck top is %.1f cm. RESIDUAL ERROR = %+.1f cm (should now be ~0 -- kRidingAnimRootLiftCm = %.1f cm is already applied)."),
				RiderRidingYawDeg, LowestFootZ, kRiderDeckHeightCm, LowestFootZ - kRiderDeckHeightCm, kRidingAnimRootLiftCm);
			UE_LOG(LogOverboardMesh, Warning, TEXT("ABoardActor RIDER PLACEMENT: rider height %.1f cm (expect ~180; a 2x error here would be a SCALE bug, not an offset bug). Board deck is 93.8 cm long, 23.2 cm wide."),
				RiderHeightCm);
			// CORRECTED TEST -- the earlier version of this line had the axes the wrong way round.
			// A standing person's feet separate along their own LEFT-RIGHT axis, which is
			// perpendicular to their facing. So feet spread in board-Y means the body faces along
			// board-X (down the road, unicycle stance); feet spread in board-X means the body
			// faces across the board (onewheel stance, what we want).
			UE_LOG(LogOverboardMesh, Warning, TEXT("ABoardActor RIDER PLACEMENT: foot spread X (fore/aft) = %.1f cm, Y (lateral) = %.1f cm. X-dominant = ONEWHEEL stance, feet on the footpads, body across the board -- WANTED. Y-dominant = UNICYCLE stance, feet side by side, body down the road -- WRONG for this vehicle. foot_l (%.1f, %.1f, %.1f), foot_r (%.1f, %.1f, %.1f)."),
				FootSeparationX, FootSeparationY,
				FootLLocal.X, FootLLocal.Y, FootLLocal.Z, FootRLocal.X, FootRLocal.Y, FootRLocal.Z);
		}
	}

	// Checkpoint C1: the numbers before the picture. Once a second, so the log stays readable.
	const double Now = FPlatformTime::Seconds();
	if (Now - LastRidingTraceLogTimeSeconds >= 1.0)
	{
		LastRidingTraceLogTimeSeconds = Now;
		UE_LOG(LogOverboardMesh, Log, TEXT("ABoardActor riding: wheel %.2f rad/s -> %.2f m/s (fwd norm %.2f -> axis1 %.2f) | lateral %.3f m (turn norm %.2f -> axis0 %.2f)"),
			LatestWheelRateRadS, SpeedMs, ForwardNormalised, AxisYValue,
			LatestRiderLateralM, TurnNormalised, AxisXValue);
	}
}

void ABoardActor::BeginPlay()
{
	Super::BeginPlay();

	bRealMeshLoaded = TryBuildRealMesh();
	if (bRealMeshLoaded)
	{
		BoxMesh->SetVisibility(false, true);
		MeshAssemblyRoot->SetVisibility(true, true);
		UE_LOG(LogOverboardMesh, Log, TEXT("ABoardActor: real board mesh loaded, placeholder box hidden."));
	}
	else
	{
		// Fallback stays visible -- an invisible board is worse than an ugly one (see class header).
		UE_LOG(LogOverboardMesh, Error, TEXT("ABoardActor: real board mesh failed to load (see prior errors); falling back to the placeholder box."));
	}

	// The Pint skin replaces whatever is visible above -- it is drawn INSTEAD OF the Openwheel
	// geometry, not alongside it, or the two chassis interpenetrate. The Openwheel components are
	// only hidden, never destroyed: they are still the geometry MuJoCo simulates, and flipping
	// bUsePintSkin off has to bring back exactly what was there before.
	if (bUsePintSkin && bPintSkinLoaded)
	{
		BoxMesh->SetVisibility(false, true);
		MeshAssemblyRoot->SetVisibility(false, true);
		PintAssemblyRoot->SetVisibility(true, true);
		UE_LOG(LogOverboardMesh, Log, TEXT("ABoardActor: Pint skin visible; Openwheel geometry hidden (cosmetic only -- MuJoCo still simulates Openwheel)."));
	}
	else if (bUsePintSkin)
	{
		UE_LOG(LogOverboardMesh, Warning, TEXT("ABoardActor: Pint skin requested but its meshes did not resolve; keeping Openwheel geometry."));
	}

	// Rider stand-in -- see docs/mannequin-rider.md. All-or-nothing: a rider stuck in the
	// default T-pose (mesh resolved, animation didn't, or vice versa) is worse than no rider.
	// Three tiers, most-wanted first, each falling through to the next: authored riding stance ->
	// stock standing idle -> no rider. Only the LAST tier is a degradation worth warning about;
	// tier 2 is the documented behaviour of a clone without the (gitignored, licensed) Fab pack.
	if (bShowRider && bRiderLoaded)
	{
		RiderMesh->SetVisibility(true, true);

		bRidingAnimActive = (bUseRidingAnim && bRidingAnimLoaded) ? TryStartRidingAnim() : false;

		// Apply the riding stance transform HERE, not only in the per-tick path.
		//
		// Everything that positions the rider used to live inside UpdatePoseFromHistory, which
		// returns early until the first wire packet arrives. That left the rider sitting in the
		// constructor's stock-idle transform -- yaw 0 and 31.2 cm too high -- for the whole
		// interval between pressing Play and the first packet, which with no sender running is
		// forever. Visible symptom: "the rider doesn't come up facing the right way until I run
		// carve." The height was wrong the entire time too, just less obviously.
		//
		// The rest state has to be correct on its own, because it is the state anyone sees who
		// opens the level without a host or a fake_sender attached.
		RiderMesh->SetRelativeLocation(FVector(0.f, 0.f, GetRiderBaseHeightCm()));
		RiderMesh->SetRelativeRotation(bRidingAnimActive ? FRotator(0.f, RiderRidingYawDeg, 0.f) : FRotator::ZeroRotator);

		if (bRidingAnimActive)
		{
			UE_LOG(LogOverboardMesh, Log, TEXT("ABoardActor: rider visible, playing the AUTHORED RIDING STANCE (Fab MonoWheel Board pack). Every joint angle is the pack artist's invention -- the physics is still a rigid ballast with no articulation. Only the POSE SELECTION is driven by simulated values. See docs/rider-riding-animation.md."));
		}
		else
		{
			RiderMesh->PlayAnimation(RiderIdleAnim, /*bLooping=*/true);
			if (bUseRidingAnim && !bRidingAnimLoaded)
			{
				UE_LOG(LogOverboardMesh, Log, TEXT("ABoardActor: riding animation requested but Content/MonoWheel_Board/ is not imported locally (expected on a fresh clone -- it is gitignored, see docs/rider-riding-animation.md). Falling back to the stock idle."));
			}
			UE_LOG(LogOverboardMesh, Log, TEXT("ABoardActor: rider visible, playing a stock idle animation (INVENTED pose, not simulated -- see docs/mannequin-rider.md)."));
		}
	}
	else if (bShowRider)
	{
		UE_LOG(LogOverboardMesh, Warning, TEXT("ABoardActor: rider requested (bShowRider) but the mannequin mesh/animation did not resolve -- Content/Characters/Mannequins/ is most likely not copied in locally (see docs/mannequin-rider.md). Board renders without a rider."));
	}

	StateClient = MakeUnique<FBoardStateClient>();
	if (!StateClient->StartListening())
	{
		UE_LOG(LogTemp, Error, TEXT("ABoardActor: BoardStateClient failed to start; board will not move."));
	}
}

void ABoardActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (StateClient.IsValid())
	{
		StateClient->Shutdown();
		StateClient.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void ABoardActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdatePoseFromHistory();
}

void ABoardActor::UpdatePoseFromHistory()
{
	if (!StateClient.IsValid())
	{
		return;
	}

	// Ask for the client's whole retained window (see FBoardStateClient::kHistoryRetentionSeconds,
	// currently 0.25s -- comfortably more than RenderDelaySeconds below, including margin for the
	// real host's bursty ~500Hz cadence rather than a smooth one). Requesting only a handful of
	// samples here would silently reintroduce the same "history shorter than the render delay"
	// gap that GetHistorySnapshot's own default already guards against.
	TArray<FTimestampedBoardState> History;
	StateClient->GetHistorySnapshot(History);
	if (History.Num() == 0)
	{
		return; // nothing received yet -- hold current pose, do not guess
	}

	// Newest raw sample, not the interpolated render pose -- see IsFallen()'s comment on why.
	bLatestSampleFallen = (History.Last().State.Flags & OverboardWire::EStateFlags::Fallen) != 0;

	// ADR-0011 exit criterion (c), condition 3 -- see IsAuthorityWarning(). Same newest-raw-
	// sample rule, and here it is load-bearing rather than merely consistent: the ADR is relying
	// on 2.868 s of lead over FALLEN, and reading this off the render-delayed pose would spend
	// RenderDelaySeconds of it for nothing.
	//
	// The arrival timestamp is captured on the RISING EDGE only, so it is the moment the warning
	// first arrived rather than the moment of the most recent packet still carrying it. That is
	// the instant the HUD's latency measurement has to be against.
	{
		const bool bWarningNow =
			(History.Last().State.Flags & OverboardWire::EStateFlags::AuthorityWarning) != 0;
		if (bWarningNow && !bLatestSampleAuthorityWarning)
		{
			AuthorityWarningArrivalTimeSeconds = History.Last().ArrivalTimeSeconds;
		}
		else if (!bWarningNow)
		{
			AuthorityWarningArrivalTimeSeconds = 0.0;
		}
		bLatestSampleAuthorityWarning = bWarningNow;
	}

	// Real simulated ballast displacement (wire v2; a v1 packet leaves these at 0, the documented
	// v1 "neutral rider" behaviour). Same newest-raw-sample reasoning as bLatestSampleFallen --
	// this is a local, board-relative offset, not something that benefits from render-delay
	// interpolation the way the world pose does.
	LatestRiderForeAftM = History.Last().State.RiderForeAftM;
	LatestRiderLateralM = History.Last().State.RiderLateralM;
	LatestWheelRateRadS = History.Last().State.WheelRateRadS;

	// Blend parameters BEFORE the offset below, so the two stay visibly independent: the offset is
	// the honest un-amplified ballast displacement and always has been, while the blend parameters
	// are the new declared-gain channel. They read the same source values and must not be confused
	// for each other.
	UpdateRidingAnimParams();

	if (bShowRider && bRiderLoaded)
	{
		// mm/m -> cm and the same local-space Y-mirror convention as everything else attached to
		// this actor (see mesh/README.md) -- these are LOCAL displacements in the board's own
		// frame, not world positions, so MuJoCoToUnreal's world-pose transform does not apply
		// here; the local-mirror rule does. NO amplification -- see docs/mannequin-rider.md.
		const float OffsetXCm = LatestRiderForeAftM * 100.f;
		const float OffsetYCm = -LatestRiderLateralM * 100.f;
		RiderMesh->SetRelativeLocation(FVector(OffsetXCm, OffsetYCm, GetRiderBaseHeightCm()));
	}

	const double RenderTime = FPlatformTime::Seconds() - static_cast<double>(RenderDelaySeconds);

	// Find the bracket [i, i+1] such that History[i].Arrival <= RenderTime <= History[i+1].Arrival.
	// One buffer behind, never extrapolate: if RenderTime is older than everything we have, hold
	// the oldest sample; if it's newer than everything we have (we're waiting on more data than
	// RenderDelaySeconds accounts for), hold the newest sample rather than projecting forward.
	int32 LowerIndex = -1;
	for (int32 i = 0; i < History.Num() - 1; ++i)
	{
		if (History[i].ArrivalTimeSeconds <= RenderTime && RenderTime <= History[i + 1].ArrivalTimeSeconds)
		{
			LowerIndex = i;
			break;
		}
	}

	OverboardWire::FUeTransform TransformToApply;

	if (LowerIndex >= 0)
	{
		const FTimestampedBoardState& A = History[LowerIndex];
		const FTimestampedBoardState& B = History[LowerIndex + 1];
		const double Span = B.ArrivalTimeSeconds - A.ArrivalTimeSeconds;
		const float Alpha = Span > 0.0 ? FMath::Clamp(static_cast<float>((RenderTime - A.ArrivalTimeSeconds) / Span), 0.f, 1.f) : 0.f;

		const OverboardWire::FUeTransform UeA = OverboardWire::MuJoCoToUnreal(A.State.Pos, A.State.Quat);
		const OverboardWire::FUeTransform UeB = OverboardWire::MuJoCoToUnreal(B.State.Pos, B.State.Quat);

		const FVector PosA(UeA.PosCm[0], UeA.PosCm[1], UeA.PosCm[2]);
		const FVector PosB(UeB.PosCm[0], UeB.PosCm[1], UeB.PosCm[2]);
		const FQuat QuatA(UeA.QuatWXYZ[1], UeA.QuatWXYZ[2], UeA.QuatWXYZ[3], UeA.QuatWXYZ[0]); // UE FQuat ctor is (X,Y,Z,W)
		const FQuat QuatB(UeB.QuatWXYZ[1], UeB.QuatWXYZ[2], UeB.QuatWXYZ[3], UeB.QuatWXYZ[0]);

		// Rotate MuJoCo's whole frame about the world vertical, THEN translate. Order matters:
		// translate-then-rotate would swing the board around the level origin on a lever arm the
		// length of its distance from it, which at this level's spawn is several hundred metres.
		const FQuat OriginYaw(FRotator(0.f, WorldOriginYawDeg, 0.f));
		SetActorLocation(OriginYaw.RotateVector(FMath::Lerp(PosA, PosB, Alpha)) + WorldOriginOffsetCm);
		SetActorRotation(OriginYaw * FQuat::Slerp(QuatA, QuatB, Alpha));
		return;
	}

	// No bracket found: RenderTime falls outside the whole history window. Clamp to whichever
	// end it's outside of instead of extrapolating.
	const FTimestampedBoardState& Clamp = (RenderTime < History[0].ArrivalTimeSeconds) ? History[0] : History.Last();
	TransformToApply = OverboardWire::MuJoCoToUnreal(Clamp.State.Pos, Clamp.State.Quat);
	const FQuat ClampOriginYaw(FRotator(0.f, WorldOriginYawDeg, 0.f));
	SetActorLocation(ClampOriginYaw.RotateVector(
		FVector(TransformToApply.PosCm[0], TransformToApply.PosCm[1], TransformToApply.PosCm[2])) + WorldOriginOffsetCm);
	SetActorRotation(ClampOriginYaw * FQuat(TransformToApply.QuatWXYZ[1], TransformToApply.QuatWXYZ[2], TransformToApply.QuatWXYZ[3], TransformToApply.QuatWXYZ[0]));
}

bool ABoardActor::TryBuildRealMesh()
{
	struct FPartSpec
	{
		UProceduralMeshComponent* Component;
		const TCHAR* StlBaseName;
		float ExtraYawDeg;
	};
	const FPartSpec Parts[] = {
		{ FrontEnclosureMesh, TEXT("front_enclosure"), 0.f },
		{ RearEnclosureMesh, TEXT("rear_enclosure"), 0.f },
		{ FrontBumperMesh, TEXT("front_bumper"), 0.f },
		{ RearBumperMesh, TEXT("rear_bumper"), 0.f },
		// Ships authored sitting at the REAR footpad location (confirmed by the loader's own
		// bounding-box output against mesh/tests/test_stl_loader.cpp, not just the XML comment)
		// -- needs the same extra 180 degree yaw the MJCF applies to move it to the front.
		{ FrontFootpadMesh, TEXT("front_footpad"), 180.f },
		{ RearFootpadMesh, TEXT("rear_footpad"), 0.f },
		{ ElectronicsPlatformMesh, TEXT("electronics_platform"), 0.f },
	};

	bool bAllOk = true;
	FBox BodyLocalBounds(EForceInit::ForceInit); // union of all 7 STL parts, actor-local space
	for (const FPartSpec& Part : Parts)
	{
		if (!BuildPartFromStl(Part.Component, Part.StlBaseName, Part.ExtraYawDeg, BodyLocalBounds))
		{
			bAllOk = false;
		}
	}

	// Arithmetic check, not eyeballing (overboard#162): the body (7 STL parts, not the wheel) is
	// 938 x 232 x 83mm per direct STL measurement, i.e. a (46.9, 11.6, 4.2)cm half-extent in
	// actor-local space. This is computed independently of the actor's current world transform
	// (spawn position, any received pose) specifically so it stays meaningful before any wire
	// state has arrived. A real fix here changed a silent scale bug (real mesh inheriting
	// BoxMesh's (0.7,0.25,0.08) placeholder-shaping scale) that was NOT visible in the wheel-only
	// bounds check below, because the wheel is a sibling of BoxMesh too, not a child of it --
	// only components actually nested under BoxMesh inherited the bug. Log this every time so
	// that stays caught if it ever regresses.
	if (BodyLocalBounds.IsValid)
	{
		const FVector BodyExtent = BodyLocalBounds.GetExtent(); // half-size, cm
		const FVector BodySize = BodyLocalBounds.GetSize();
		constexpr float kExpectedHalfX = 46.9f, kExpectedHalfY = 11.6f, kExpectedHalfZ = 4.2f;
		constexpr float kToleranceCm = 2.0f; // generous sanity-check tolerance, not a unit test
		const bool bXOk = FMath::IsNearlyEqual(BodyExtent.X, kExpectedHalfX, kToleranceCm);
		const bool bYOk = FMath::IsNearlyEqual(BodyExtent.Y, kExpectedHalfY, kToleranceCm);
		const bool bZOk = FMath::IsNearlyEqual(BodyExtent.Z, kExpectedHalfZ, kToleranceCm);
		UE_LOG(LogOverboardMesh, Log,
			TEXT("ABoardActor: body (7 STL parts) local bounds half-extent = (%.2f, %.2f, %.2f) cm, full size = (%.2f, %.2f, %.2f) cm ")
			TEXT("-- expected half-extent ~(46.9, 11.6, 4.2) cm / full ~(93.8, 23.2, 8.3) cm. %s"),
			BodyExtent.X, BodyExtent.Y, BodyExtent.Z, BodySize.X, BodySize.Y, BodySize.Z,
			(bXOk && bYOk && bZOk) ? TEXT("MATCH.") : TEXT("MISMATCH -- scale or placement bug, do not trust this render."));
	}
	else
	{
		UE_LOG(LogOverboardMesh, Error, TEXT("ABoardActor: body bounds empty -- no STL part contributed a single vertex."));
	}

	// Wheel: not an STL, a primitive cylinder. Radius 145.4mm / width 150mm (mesh/README.md,
	// straight from overboard_onewheel.xml's wheel_geom: size="0.1454 0.075" = radius, half-width).
	UStaticMesh* CylinderMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderMesh && WheelMesh)
	{
		WheelMesh->SetStaticMesh(CylinderMesh);

		// ASSUMPTION, unverified against this specific engine build (no display to check it):
		// /Engine/BasicShapes/Cylinder.Cylinder is the standard UE default bounds, radius 50uu,
		// height 100uu. If the board comes out ~2x or ~0.5x wrong on the wheel specifically
		// (everything else right), this assumption is the first thing to check -- see the log
		// line below, which prints the actual computed bounds for exactly that comparison.
		constexpr float kAssumedEngineCylinderRadiusUu = 50.f;
		constexpr float kAssumedEngineCylinderHeightUu = 100.f;
		constexpr float kTireRadiusCm = 14.54f; // 145.4mm
		constexpr float kTireWidthCm = 15.f;    // 150mm (2 x 75mm half-width)
		const float RadiusScale = kTireRadiusCm / kAssumedEngineCylinderRadiusUu;
		const float HeightScale = kTireWidthCm / kAssumedEngineCylinderHeightUu;
		WheelMesh->SetRelativeScale3D(FVector(RadiusScale, RadiusScale, HeightScale));

		// MuJoCo's cylinder default axis is Z; euler="90 0 0" in the MJCF tips it onto Y
		// (lateral) -- same operation here, a 90 degree Roll (UE FRotator rotates about X on
		// Roll), frame-labelling-agnostic since the tire is rotationally symmetric about its own
		// axis (see mesh/README.md).
		WheelMesh->SetRelativeRotation(FRotator(0.f, 0.f, 90.f)); // FRotator(Pitch, Yaw, Roll)

		WheelMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		WheelMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
		WheelMesh->SetSimulatePhysics(false);
		WheelMesh->SetEnableGravity(false);
		WheelMesh->SetMobility(EComponentMobility::Movable);

		const FBoxSphereBounds Bounds = WheelMesh->CalcBounds(WheelMesh->GetComponentTransform());
		UE_LOG(LogOverboardMesh, Log,
			TEXT("ABoardActor: wheel mesh world bounds extent = (%.2f, %.2f, %.2f) cm -- expect radius ~14.54cm, ")
			TEXT("width ~15cm if the assumed engine cylinder native bounds (radius 50uu, height 100uu) are correct. ")
			TEXT("VERIFY VISUALLY, not assumed correct."),
			Bounds.BoxExtent.X, Bounds.BoxExtent.Y, Bounds.BoxExtent.Z);
	}
	else
	{
		UE_LOG(LogOverboardMesh, Error, TEXT("ABoardActor: failed to load wheel cylinder mesh (/Engine/BasicShapes/Cylinder.Cylinder)"));
		bAllOk = false;
	}

	return bAllOk;
}

bool ABoardActor::BuildPartFromStl(UProceduralMeshComponent* Component, const FString& StlBaseName, float ExtraYawDeg, FBox& InOutLocalBounds)
{
	if (!Component)
	{
		return false;
	}

	const FString StlPath = FPaths::ProjectDir() / TEXT("Meshes/openwheel") / (StlBaseName + TEXT(".stl"));

	OverboardMesh::FStlMesh StlMesh;
	std::string Err;
	if (!OverboardMesh::LoadBinaryStl(TCHAR_TO_UTF8(*StlPath), StlMesh, Err))
	{
		UE_LOG(LogOverboardMesh, Error, TEXT("ABoardActor: failed to load %s: %s"), *StlBaseName, *FString(Err.c_str()));
		return false;
	}

	constexpr float kMmToCm = 0.1f; // mesh/README.md: mm -> cm is x0.1 (MuJoCo's 0.001 x UE's 100)
	const float YawRad = FMath::DegreesToRadians(ExtraYawDeg);
	const float CosYaw = FMath::Cos(YawRad);
	const float SinYaw = FMath::Sin(YawRad);

	// mm -> cm, then the same Y-mirror wire/CoordinateTransform.cpp applies to world positions --
	// see mesh/README.md: the actor's local mesh space is "world space at identity rotation", so
	// the same right-to-left-handed mirror applies to local vertices too. Front_footpad's extra
	// yaw (matching the MJCF's euler="0 0 180" on that one geom) is applied after the mirror, in
	// UE-local space, as a plain 2D rotation about Z.
	auto ToUeLocal = [kMmToCm, CosYaw, SinYaw](const OverboardMesh::FVec3& V) -> FVector
	{
		FVector P(V.X * kMmToCm, -V.Y * kMmToCm, V.Z * kMmToCm);
		const float X = P.X;
		const float Y = P.Y;
		P.X = X * CosYaw - Y * SinYaw;
		P.Y = X * SinYaw + Y * CosYaw;
		return P;
	};

	const int32 TriCount = static_cast<int32>(StlMesh.Triangles.size());
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> VertexColors;
	TArray<FProcMeshTangent> Tangents;
	Vertices.Reserve(TriCount * 3);
	Triangles.Reserve(TriCount * 3);
	Normals.Reserve(TriCount * 3);
	UVs.Reserve(TriCount * 3);
	VertexColors.Reserve(TriCount * 3);

	int32 Index = 0;
	for (const OverboardMesh::FStlTriangle& Tri : StlMesh.Triangles)
	{
		const FVector V0 = ToUeLocal(Tri.V0);
		const FVector V1 = ToUeLocal(Tri.V1);
		const FVector V2 = ToUeLocal(Tri.V2);

		// Negating Y reverses winding -- add in (V0, V2, V1) order, not (V0, V1, V2), to keep
		// faces front-facing. The normal below is computed from this same (already-swapped)
		// order so it stays consistent with the winding, rather than trusting the STL's own
		// facet normal (see StlLoader.h: deliberately ignored on load).
		Vertices.Add(V0);
		Vertices.Add(V2);
		Vertices.Add(V1);

		InOutLocalBounds += V0;
		InOutLocalBounds += V1;
		InOutLocalBounds += V2;

		const FVector FaceNormal = FVector::CrossProduct(V2 - V0, V1 - V0).GetSafeNormal();
		Normals.Add(FaceNormal);
		Normals.Add(FaceNormal);
		Normals.Add(FaceNormal);

		Triangles.Add(Index);
		Triangles.Add(Index + 1);
		Triangles.Add(Index + 2);
		Index += 3;

		UVs.Add(FVector2D(0.f, 0.f));
		UVs.Add(FVector2D(0.f, 0.f));
		UVs.Add(FVector2D(0.f, 0.f));

		// Not currently consumed by any material (see mesh/README.md -- per-part brand-palette
		// colour is a deliberately deferred follow-up, not attempted blind). Included anyway so
		// a future material can read it without another geometry pass.
		VertexColors.Add(FColor::White);
		VertexColors.Add(FColor::White);
		VertexColors.Add(FColor::White);
	}

	Component->CreateMeshSection(0, Vertices, Triangles, Normals, UVs, VertexColors, Tangents, /*bCreateCollision=*/false);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetCollisionResponseToAllChannels(ECR_Ignore);
	Component->SetSimulatePhysics(false);
	Component->SetEnableGravity(false);
	Component->SetMobility(EComponentMobility::Movable);

	return true;
}
