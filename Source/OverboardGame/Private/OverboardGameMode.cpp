#include "OverboardGameMode.h"

#include "BoardActor.h"
#include "OverboardPlayerController.h"
#include "OverboardCameraPawn.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

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
			// Third attempt, and the actual diagnosis (overboard#162): the COO A/B tested with
			// real headless captures. SetCastShadow(false) -> no change. Disabling Lumen diffuse
			// indirect + its denoiser -> no change, pattern pixel-identical. So it was never
			// shadowing or GI noise -- it's texture MINIFICATION ALIASING: WorldGridMaterial's
			// grid lines are fine-grained (centimetre-scale), stretched over a 100m plane and
			// undersampled at distance/grazing angles. The fix is spatial frequency, not
			// lighting: DefaultMaterial is the engine's literal flat/textureless default --
			// zero spatial frequency, so it is structurally incapable of aliasing regardless of
			// scale or viewing angle, not just "large squares that alias less".
			// Deliberately NOT disabling VSM or Lumen project-wide (still true) -- that trades one
			// local artifact for a global downgrade in the launch footage.
			UMaterialInterface* FlatMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
			if (FlatMaterial)
			{
				Ground->GetStaticMeshComponent()->SetMaterial(0, FlatMaterial);
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

	SpawnedBoard = World->SpawnActor<ABoardActor>(FVector(0.f, 0.f, 50.f), FRotator::ZeroRotator);

	// W2: something to look through. See AOverboardCameraPawn -- possesses itself, finds the
	// board lazily, so spawn order relative to SpawnedBoard above doesn't matter. Starting
	// transform here is just "somewhere behind the board"; the pawn's own Tick takes over
	// immediately once it acquires a follow target.
	World->SpawnActor<AOverboardCameraPawn>(FVector(-800.f, 0.f, 200.f), FRotator::ZeroRotator);
}

void AOverboardGameMode::SpawnMotionReferenceMarkers(UWorld* World) const
{
	// overboard#162: a chase camera following position+yaw over a featureless plane pins the
	// board to the centre of frame with nothing else in the scene to show it's moving 15-20m
	// over a run. These are plain scenery landmarks, nothing more -- see the class header.
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* CylinderMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UStaticMesh* ConeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
	UMaterialInterface* FlatMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
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
			if (FlatMaterial)
			{
				MeshComp->SetMaterial(0, FlatMaterial);
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
