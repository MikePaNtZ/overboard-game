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
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneFinder(TEXT("/Engine/BasicShapes/Plane.Plane"));
	AStaticMeshActor* Ground = World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (Ground && PlaneFinder.Succeeded())
	{
		Ground->GetStaticMeshComponent()->SetStaticMesh(PlaneFinder.Object);
		Ground->GetStaticMeshComponent()->SetWorldScale3D(FVector(100.f, 100.f, 1.f));
		Ground->SetMobility(EComponentMobility::Static);
	}

	SpawnedBoard = World->SpawnActor<ABoardActor>(FVector(0.f, 0.f, 50.f), FRotator::ZeroRotator);

	// W2: something to look through. See AOverboardCameraPawn -- possesses itself, finds the
	// board lazily, so spawn order relative to SpawnedBoard above doesn't matter. Starting
	// transform here is just "somewhere behind the board"; the pawn's own Tick takes over
	// immediately once it acquires a follow target.
	World->SpawnActor<AOverboardCameraPawn>(FVector(-800.f, 0.f, 200.f), FRotator::ZeroRotator);
}
