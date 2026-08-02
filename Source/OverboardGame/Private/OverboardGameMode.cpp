#include "OverboardGameMode.h"

#include "BoardActor.h"
#include "OverboardPlayerController.h"
#include "OverboardCameraPawn.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"
#include "Logging/LogMacros.h"
#include "GameFramework/PlayerStart.h"
#include "EngineUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogOverboardLevel, Log, All);

namespace
{
	// Guaranteed-non-null flat material (overboard#162, fourth pass on the ground acne). The
	// previous fix, LoadObject<UMaterialInterface>("/Engine/EngineMaterials/DefaultMaterial.
	// DefaultMaterial"), was silently returning null at BOTH call sites that used it (ground and
	// markers), and both silently skipped SetMaterial on null -- confirmed by the COO: markers
	// were visibly still checkerboarded in captures despite "using" this same flat material, and
	// two passes were spent chasing lighting settings (SetCastShadow, Lumen diffuse indirect,
	// VSM) before that silent failure was noticed. UMaterial::GetDefaultMaterial is documented to
	// always return a valid material -- no string asset path to get wrong, no null to silently
	// swallow. Logs loudly on the (should-be-impossible) case where it still fails, per the same
	// "every asset load in this file says so when it fails" rule the FObjectFinder crash and the
	// mesh-bounds MATCH/MISMATCH log already established.
	UMaterialInterface* MakeFlatMaterial(UObject* Outer, const FLinearColor& Color, const TCHAR* DebugName)
	{
		UMaterial* BaseMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		if (!BaseMaterial)
		{
			UE_LOG(LogOverboardLevel, Error, TEXT("MakeFlatMaterial(%s): UMaterial::GetDefaultMaterial(MD_Surface) returned null -- should not be possible."), DebugName);
			return nullptr;
		}

		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMaterial, Outer);
		if (!MID)
		{
			UE_LOG(LogOverboardLevel, Error, TEXT("MakeFlatMaterial(%s): UMaterialInstanceDynamic::Create failed, falling back to the base material with no tint."), DebugName);
			return BaseMaterial;
		}

		// Best-effort tint so the ground and markers can read as different tones -- silently a
		// no-op if the engine default material doesn't expose a "Color" parameter (it may not).
		// Not load-bearing: the aliasing fix is "flat, zero spatial frequency", which
		// GetDefaultMaterial guarantees regardless of whether this tint takes.
		MID->SetVectorParameterValue(TEXT("Color"), Color);
		return MID;
	}
}

AOverboardGameMode::AOverboardGameMode()
{
	PlayerControllerClass = AOverboardPlayerController::StaticClass();
	// Still nullptr: the camera pawn (spawned in BeginPlay below) possesses itself via
	// AutoPossessPlayer rather than going through GameMode's default-pawn/PlayerStart flow --
	// simpler for a single-player prototype with no PlayerStart authored yet. The BOARD remains
	// wire-driven, not a pawn -- that has not changed since W1.
	DefaultPawnClass = nullptr;
}

void AOverboardGameMode::BeginPlay()
{
	Super::BeginPlay();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Flat ground plane. Placeholder scale of a 100x100m plane (Engine's unit plane is 1x1m).
	// Conditional on bSpawnPlaceholderGround (overboard#162): a level that supplies its own
	// floor -- an imported environment -- must be able to opt out, or this unconditionally
	// spawned plane slices through its geometry with no way to remove it in the editor (it isn't
	// a level asset). See OverboardGameMode.h and AOverboardGameMode_NoGround below.
	//
	// LoadObject, NOT ConstructorHelpers::FObjectFinder. FObjectFinder asserts if it is
	// constructed outside a UObject constructor -- and BeginPlay is not one, so the previous
	// version here fatal-errored the moment anyone pressed Play:
	//
	//   Fatal error: FObjectFinders can't be used outside of constructors to find
	//   /Engine/BasicShapes/Plane.Plane
	//
	// Marking it `static` did not save it: the assert fires on first construction regardless,
	// which is the first PIE session. This compiled clean and passed CI, because CI gates the
	// engine-free wire/ tests and nothing else -- the defect was only ever reachable by
	// pressing Play, which no automated pass in this repo can do. Keep runtime asset loads on
	// LoadObject; reserve FObjectFinder for constructors (see ABoardActor, where it is correct).
	if (bSpawnPlaceholderGround)
	{
		UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
		AStaticMeshActor* Ground = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator);
		if (Ground && PlaneMesh)
		{
			Ground->GetStaticMeshComponent()->SetStaticMesh(PlaneMesh);
			Ground->GetStaticMeshComponent()->SetWorldScale3D(FVector(100.f, 100.f, 1.f));
			Ground->SetMobility(EComponentMobility::Static);
			// Root cause (overboard#162, confirmed by the COO's headless A/B captures, including
			// `viewmode unlit` still showing the speckle -- decisive that this was never a
			// lighting/shadow/GI artifact, and r.ScreenPercentage 200 not fixing it ruling out
			// ordinary texture mip aliasing too: it's WorldGridMaterial's procedural, per-pixel,
			// world-space pattern, evaluated at native resolution with no mipmaps to fall back
			// to). Fix: MakeFlatMaterial above -- zero spatial frequency, cannot alias.
			UMaterialInterface* FlatMaterial = MakeFlatMaterial(Ground, FLinearColor(0.55f, 0.56f, 0.60f), TEXT("Ground"));
			if (FlatMaterial)
			{
				Ground->GetStaticMeshComponent()->SetMaterial(0, FlatMaterial);
			}
			else
			{
				UE_LOG(LogOverboardLevel, Error, TEXT("AOverboardGameMode: ground has no flat material to apply; it will show whatever /Engine/BasicShapes/Plane.Plane's own default material is."));
			}
			// Cheap, COO-suggested try alongside the material swap -- decal projection is an
			// unlikely but free-to-rule-out contributor to badly-sampled ground shading.
			Ground->GetStaticMeshComponent()->SetReceivesDecals(false);
			Ground->GetStaticMeshComponent()->SetCastShadow(false); // kept: still correct even if not sufficient alone
		}
	}

	if (bSpawnMotionReferenceMarkers)
	{
		SpawnMotionReferenceMarkers(World);
	}

	// Where MuJoCo's origin lands in this level. The C++ placeholder ground is centred on the
	// world origin, so zero is right for OB_Main and every level that came before this; an
	// imported environment almost never is, so it says where it wants the board by placing a
	// single PlayerStart. First one found wins -- if a level has several, that is a level-
	// authoring mistake and the log below says which one was taken, rather than picking silently.
	//
	// PlayerStart is deliberately reused rather than a bespoke marker class: it already exists in
	// the editor's Place Actors panel, so putting the board somewhere needs no C++, no Blueprint
	// and nothing this repo has to ship. DefaultPawnClass is still nullptr and the camera pawn
	// still auto-possesses, so nothing actually spawns AT the PlayerStart through the normal
	// GameMode flow -- it is being used purely as a level-authored coordinate.
	FVector OriginOffsetCm = FVector::ZeroVector;
	float OriginYawDeg = 0.f;
	const APlayerStart* Start = nullptr;
	if (bUsePlayerStartAsWorldOrigin)
	{
		for (TActorIterator<APlayerStart> It(World); It; ++It)
		{
			Start = *It;
			break;
		}
	}

	if (Start)
	{
		OriginOffsetCm = Start->GetActorLocation();
		// Yaw only -- see ABoardActor::SetWorldOriginYawDeg for why pitch/roll are discarded
		// rather than forwarded. A PlayerStart dropped by hand is rarely perfectly level, and
		// inheriting that tilt would render as the board failing to balance.
		OriginYawDeg = Start->GetActorRotation().Yaw;
		UE_LOG(LogOverboardLevel, Log, TEXT("AOverboardGameMode: PlayerStart '%s' found; MuJoCo origin -> UE (%.1f, %.1f, %.1f) cm, yaw %.1f deg."),
			*Start->GetName(), OriginOffsetCm.X, OriginOffsetCm.Y, OriginOffsetCm.Z, OriginYawDeg);
	}
	else
	{
		UE_LOG(LogOverboardLevel, Log, TEXT("AOverboardGameMode: MuJoCo origin stays at UE world origin (bUsePlayerStartAsWorldOrigin=%s)."),
			bUsePlayerStartAsWorldOrigin ? TEXT("true, but no PlayerStart found") : TEXT("false"));
	}

	SpawnedBoard = World->SpawnActor<ABoardActor>(OriginOffsetCm + FVector(0.f, 0.f, 50.f), FRotator(0.f, OriginYawDeg, 0.f));
	if (SpawnedBoard)
	{
		// Must be set before the first wire packet is applied, or the board renders one frame at
		// the unoffset origin -- which in a large imported level is far enough away to read as a
		// flicker on screen.
		SpawnedBoard->SetWorldOriginOffsetCm(OriginOffsetCm);
		SpawnedBoard->SetWorldOriginYawDeg(OriginYawDeg);
	}

	// W2: something to look through. See AOverboardCameraPawn -- possesses itself, finds the
	// board lazily, so spawn order relative to SpawnedBoard above doesn't matter. Starting
	// transform here is just "somewhere behind the board"; the pawn's own Tick takes over
	// immediately once it acquires a follow target.
	// "Behind the board" is only behind it once the origin is yawed -- rotate the offset too, or
	// the camera starts off to one side and swings across on the first tick.
	const FVector CameraOffset = FRotator(0.f, OriginYawDeg, 0.f).RotateVector(FVector(-800.f, 0.f, 200.f));
	World->SpawnActor<AOverboardCameraPawn>(OriginOffsetCm + CameraOffset, FRotator(0.f, OriginYawDeg, 0.f));
}

void AOverboardGameMode::SpawnMotionReferenceMarkers(UWorld* World) const
{
	// overboard#162: a chase camera following position+yaw over a featureless plane pins the
	// board to the centre of frame with nothing else in the scene to show it's moving 15-20m
	// over a run. These are plain scenery landmarks, nothing more -- see the class header.
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* CylinderMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UStaticMesh* ConeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
	UStaticMesh* Shapes[] = { CubeMesh, CylinderMesh, ConeMesh };

	// A run covers roughly 15-20m; this spans ~40m so markers are visible well before and after
	// a run passes through. Grid, not random, so a capture is reproducible run to run.
	constexpr float kSpacingCm = 800.f;        // 8m between markers
	constexpr float kHalfExtentCm = 2000.f;    // +-20m => ~40m field
	constexpr float kClearRadiusCm = 300.f;    // 3m clear zone at spawn -- nothing sits on the board at t=0
	constexpr float kBaseSizeCm = 100.f;       // footprint before per-marker height variation
	constexpr float kBaseHeightCm = 250.f;
	constexpr float kHeightVariationCm = 150.f; // "vary height a little so heading is readable"

	int32 ShapeCycle = 0;
	for (float X = -kHalfExtentCm; X <= kHalfExtentCm; X += kSpacingCm)
	{
		for (float Y = -kHalfExtentCm; Y <= kHalfExtentCm; Y += kSpacingCm)
		{
			if (FMath::Sqrt(X * X + Y * Y) < kClearRadiusCm)
			{
				continue; // clear zone at spawn
			}

			UStaticMesh* Mesh = Shapes[ShapeCycle % 3];
			++ShapeCycle;
			if (!Mesh)
			{
				continue;
			}

			// Deterministic, not random, so re-running the same session looks the same --
			// matters for comparing captures across a fix, not just for a first look.
			const float HeightScale = 1.f + 0.5f * FMath::Sin(X * 0.011f + Y * 0.017f);
			const float HeightCm = kBaseHeightCm + kHeightVariationCm * HeightScale;

			AStaticMeshActor* Marker = World->SpawnActor<AStaticMeshActor>(FVector(X, Y, HeightCm * 0.5f), FRotator::ZeroRotator);
			if (!Marker)
			{
				continue;
			}
			UStaticMeshComponent* MeshComp = Marker->GetStaticMeshComponent();
			MeshComp->SetStaticMesh(Mesh);
			// Engine primitives default to ~100uu (1m) native size -- scale to the target footprint/height.
			MeshComp->SetWorldScale3D(FVector(kBaseSizeCm / 100.f, kBaseSizeCm / 100.f, HeightCm / 100.f));
			// Per-marker MID (Outer = this specific Marker actor) -- same guaranteed-non-null
			// source as the ground, see MakeFlatMaterial. This is the fix for the markers the COO
			// caught visibly checkerboarded in captures: they were never actually getting a flat
			// material either, for the same silent-null reason as the ground.
			UMaterialInterface* FlatMaterial = MakeFlatMaterial(Marker, FLinearColor(0.35f, 0.40f, 0.45f), TEXT("Marker"));
			if (FlatMaterial)
			{
				MeshComp->SetMaterial(0, FlatMaterial);
			}
			else
			{
				UE_LOG(LogOverboardLevel, Error, TEXT("AOverboardGameMode: motion-reference marker at (%.0f, %.0f) has no flat material to apply."), X, Y);
			}
			Marker->SetMobility(EComponentMobility::Static);

			// Scenery only -- THE RULE (overboard#162): if Unreal geometry ever affects the
			// board's motion, the renderer has started computing physics, which it must never do.
			// MuJoCo knows about a flat ground plane and nothing else.
			MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			MeshComp->SetCollisionResponseToAllChannels(ECR_Ignore);
			MeshComp->SetSimulatePhysics(false);
			MeshComp->SetEnableGravity(false);
		}
	}
}
