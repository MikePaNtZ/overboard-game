// OverboardGameMode.h
//
// W1 scope: spawns a flat ground plane and the placeholder board actor purely in C++, so the
// scene exists regardless of what (if anything) is authored in the level itself -- there is no
// editor session available to hand-place actors in a map for this pass. See wire/README.md /
// the PR description for the manual editor step this still needs (creating and setting a
// default level asset).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "OverboardGameMode.generated.h"

class ABoardActor;

UCLASS()
class OVERBOARDGAME_API AOverboardGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AOverboardGameMode();

	virtual void BeginPlay() override;

private:
	UPROPERTY(Transient)
	TObjectPtr<ABoardActor> SpawnedBoard;
};
