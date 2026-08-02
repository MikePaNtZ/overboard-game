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
	//
	// +180 deg, and it is a fix, not a preference. The board's nose is its LOCAL -X (MuJoCo's
	// convention, carried through unchanged by MuJoCoToUnreal -- see overboard_onewheel.xml,
	// "FORWARD IS -X"), while a UE SpringArm places its camera behind the pawn's +X. Matching the
	// board's yaw directly therefore parked the camera on the NOSE side, filming the board driving
	// head-on into the lens: "lean forward" appeared to drive it towards the viewer, and because a
	// head-on view mirrors the horizontal axis, a correct right-hand carve read as turning left.
	// Both symptoms, one cause. Adding half a turn puts the camera behind the tail looking the way
	// the board travels, which is also the more legible shot -- the viewer sees where it is going,
	// not where it has been.
	const float TargetYaw = FollowTarget->GetActorRotation().Yaw + 180.f;
	const FRotator NewRotation = FMath::RInterpTo(GetActorRotation(), FRotator(0.f, TargetYaw, 0.f), DeltaSeconds, FollowYawSpeed);
	SetActorRotation(FRotator(0.f, NewRotation.Yaw, 0.f));
}
