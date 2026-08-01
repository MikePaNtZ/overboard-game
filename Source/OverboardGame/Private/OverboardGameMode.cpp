#include "OverboardGameMode.h"

#include "BoardActor.h"
#include "OverboardPlayerController.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/ConstructorHelpers.h"

AOverboardGameMode::AOverboardGameMode()
{
	PlayerControllerClass = AOverboardPlayerController::StaticClass();
	DefaultPawnClass = nullptr; // no avatar for W1 -- board pose comes from the wire, not a pawn
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
}
