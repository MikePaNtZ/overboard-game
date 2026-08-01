#include "OverboardGameMode.h"

#include "BoardActor.h"
#include "OverboardPlayerController.h"
#include "OverboardCameraPawn.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
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
			// Shadow acne fix (overboard#162, first PIE session): a 1m unit plane stretched 100x
			// gives its UVs/normals an extreme scale, a classic Virtual Shadow Map acne source. The
			// plane is flat and has nothing meaningful to cast a shadow onto itself or anything
			// below it -- it should only ever RECEIVE the board's shadow, never cast its own.
			// Deliberately not disabling VSMs/Lumen project-wide: that would trade one local
			// artifact for a global downgrade in how the launch footage looks.
			Ground->GetStaticMeshComponent()->SetCastShadow(false);
		}
	}

	SpawnedBoard = World->SpawnActor<ABoardActor>(FVector(0.f, 0.f, 50.f), FRotator::ZeroRotator);

	// W2: something to look through. See AOverboardCameraPawn -- possesses itself, finds the
	// board lazily, so spawn order relative to SpawnedBoard above doesn't matter. Starting
	// transform here is just "somewhere behind the board"; the pawn's own Tick takes over
	// immediately once it acquires a follow target.
	World->SpawnActor<AOverboardCameraPawn>(FVector(-800.f, 0.f, 200.f), FRotator::ZeroRotator);
}
