#include "BoardActor.h"

#include "Components/StaticMeshComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "CoordinateTransform.h"
#include "HAL/PlatformTime.h"

ABoardActor::ABoardActor()
{
	PrimaryActorTick.bCanEverTick = true;

	BoxMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BoxMesh"));
	RootComponent = BoxMesh;

	// Placeholder for the real board model -- explicitly fine for W1 (see issue #162).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		BoxMesh->SetStaticMesh(CubeFinder.Object);
	}
	// Board-ish proportions rather than a 1m cube: long, thin, low.
	BoxMesh->SetRelativeScale3D(FVector(0.7f, 0.25f, 0.08f));

	// THE RULE: this application computes no board physics. Collision and gravity are off --
	// the transform comes entirely from wire state.
	BoxMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BoxMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	BoxMesh->SetSimulatePhysics(false);
	BoxMesh->SetEnableGravity(false);
	BoxMesh->SetMobility(EComponentMobility::Movable);
}

void ABoardActor::BeginPlay()
{
	Super::BeginPlay();

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

	TArray<FTimestampedBoardState> History;
	StateClient->GetHistorySnapshot(History, 8);
	if (History.Num() == 0)
	{
		return; // nothing received yet -- hold current pose, do not guess
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

		SetActorLocation(FMath::Lerp(PosA, PosB, Alpha));
		SetActorRotation(FQuat::Slerp(QuatA, QuatB, Alpha));
		return;
	}

	// No bracket found: RenderTime falls outside the whole history window. Clamp to whichever
	// end it's outside of instead of extrapolating.
	const FTimestampedBoardState& Clamp = (RenderTime < History[0].ArrivalTimeSeconds) ? History[0] : History.Last();
	TransformToApply = OverboardWire::MuJoCoToUnreal(Clamp.State.Pos, Clamp.State.Quat);
	SetActorLocation(FVector(TransformToApply.PosCm[0], TransformToApply.PosCm[1], TransformToApply.PosCm[2]));
	SetActorRotation(FQuat(TransformToApply.QuatWXYZ[1], TransformToApply.QuatWXYZ[2], TransformToApply.QuatWXYZ[3], TransformToApply.QuatWXYZ[0]));
}
