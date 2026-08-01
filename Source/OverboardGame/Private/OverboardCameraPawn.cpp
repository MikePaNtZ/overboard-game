#include "OverboardCameraPawn.h"

#include "BoardActor.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Kismet/GameplayStatics.h"

AOverboardCameraPawn::AOverboardCameraPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(Root);
	SpringArm->TargetArmLength = ArmLengthCm;
	SpringArm->SetRelativeRotation(FRotator(ArmPitchDeg, 0.f, 0.f));
	SpringArm->bDoCollisionTest = false; // W2: no scene geometry worth colliding the boom against yet
	SpringArm->bEnableCameraLag = true;
	SpringArm->CameraLagSpeed = 8.f;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);

	// No GameMode/PlayerStart ceremony needed for a single-player prototype -- see class header.
	AutoPossessPlayer = EAutoReceiveInput::Player0;
}

void AOverboardCameraPawn::TryAcquireFollowTarget()
{
	if (FollowTarget.IsValid())
	{
		return;
	}

	TArray<AActor*> Found;
	UGameplayStatics::GetAllActorsOfClass(GetWorld(), ABoardActor::StaticClass(), Found);
	if (Found.Num() > 0)
	{
		FollowTarget = Cast<ABoardActor>(Found[0]);
	}
}

void AOverboardCameraPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!FollowTarget.IsValid())
	{
		TryAcquireFollowTarget();
		if (!FollowTarget.IsValid())
		{
			return; // nothing to follow yet -- hold position, do not guess
		}
	}

	const FVector TargetLocation = FollowTarget->GetActorLocation() + FVector(0.f, 0.f, FollowHeightOffsetCm);
	const FVector NewLocation = FMath::VInterpTo(GetActorLocation(), TargetLocation, DeltaSeconds, FollowLocationSpeed);
	SetActorLocation(NewLocation);

	// Yaw only -- deliberately does not inherit the board's pitch/roll (see class header).
	const float TargetYaw = FollowTarget->GetActorRotation().Yaw;
	const FRotator NewRotation = FMath::RInterpTo(GetActorRotation(), FRotator(0.f, TargetYaw, 0.f), DeltaSeconds, FollowYawSpeed);
	SetActorRotation(FRotator(0.f, NewRotation.Yaw, 0.f));
}
