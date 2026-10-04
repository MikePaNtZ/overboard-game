// RenderLookOverride.h
//
// A render level's own lighting rig has to replace the environment's, not add to it. City Park
// ships a 2018-era rig (a stationary sun, BP_Sky_Sphere, AtmosphericFog, a height fog and an
// unbound post-process volume) inside its Showcase sublevel, which this repo must never save.
// So the replacement happens at runtime instead: this actor switches off every actor of the
// listed kinds that lives in ANOTHER level than its own. Nothing is destroyed, nothing on disk
// changes, and a level without this actor renders City Park exactly as before.
//
// Cosmetic only. It touches lights, sky, fog and post-process -- never the board, never a pose.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RenderLookOverride.generated.h"

UCLASS()
class OVERBOARDGAME_API ARenderLookOverride : public AActor
{
	GENERATED_BODY()

public:
	ARenderLookOverride();
	virtual void Tick(float DeltaSeconds) override;

	// Substrings of class names to switch off when they live in a foreign level.
	UPROPERTY(EditAnywhere, Category = "Render")
	TArray<FString> DisableClassNameContains;

private:
	// Sublevels stream in on their own schedule, so the pass repeats every tick. It is a walk over
	// a few hundred actors; the cost is nothing beside a frame of the scene it is cleaning up.
	void DisableForeignLookActors();
	int32 LoggedDisabledCount = -1;
};
