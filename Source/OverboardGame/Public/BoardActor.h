// BoardActor.h
//
// The rendered board. THE RULE: this actor computes no physics. Collision and gravity are
// disabled in the constructor; its transform every tick comes entirely from state received over
// the wire, one render-delay behind the wall clock, interpolated -- never extrapolated. A
// placeholder box mesh stands in for the real board model (explicitly fine for W1).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BoardStateClient.h"
#include "BoardActor.generated.h"

class UStaticMeshComponent;

UCLASS()
class OVERBOARDGAME_API ABoardActor : public AActor
{
	GENERATED_BODY()

public:
	ABoardActor();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

protected:
	UPROPERTY(VisibleAnywhere, Category = "Board")
	TObjectPtr<UStaticMeshComponent> BoxMesh;

	// How far behind the wall clock we render, in seconds. Must always be able to find two real
	// samples to interpolate between; too small and we run out of history and hold the last
	// known pose (still not extrapolation, just a stall). Tune once the host's real send rate
	// (500 Hz control loop) is confirmed -- this default is a conservative placeholder.
	UPROPERTY(EditAnywhere, Category = "Board|Networking")
	float RenderDelaySeconds = 0.05f;

private:
	TUniquePtr<FBoardStateClient> StateClient;

	// Applies the interpolated, transformed pose to the actor. No-op if we don't yet have at
	// least two received samples.
	void UpdatePoseFromHistory();
};
