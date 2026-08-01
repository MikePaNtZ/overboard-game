// OverboardGameMode.h
//
// Spawns a flat ground plane, the board actor, and (W2+) a chase camera pawn purely in C++, so
// the scene exists regardless of what (if anything) is authored in the level itself.
//
// bSpawnPlaceholderGround (overboard#162): a level that supplies its own floor -- a real
// imported environment, for instance -- must be able to say "don't give me your 100x100m
// checkered plane too, it will slice through my geometry." Per-LEVEL, not per-class-instance,
// because GameMode has no in-level instance to tweak; the idiomatic UE mechanism for that is a
// GameMode subclass selected per-map via World Settings -> GameMode Override. See
// AOverboardGameMode_NoGround below: a second, already-built native C++ class for exactly that
// dropdown, so nobody has to hand-author a Blueprint to turn the ground off. OB_Main keeps using
// this class (or leaves GameMode Override unset, which falls back to the project default, also
// this class) and is completely unaffected -- the flag defaults true and nothing about it changed.
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

	// For AOverboardPlayerController's reset-on-fall (overboard#162 W3) -- lets it find the
	// board without a separate hand-authored reference. May be null before BeginPlay has run.
	ABoardActor* GetSpawnedBoard() const { return SpawnedBoard; }

protected:
	// Default true: every existing level (OB_Main) keeps the placeholder ground exactly as
	// before. Set false only by AOverboardGameMode_NoGround's constructor, for levels that
	// supply their own floor. EditDefaultsOnly, not EditAnywhere -- there is no per-instance
	// GameMode in a level to tweak this on; the class itself is the per-level selection unit
	// (World Settings -> GameMode Override), so the default IS the whole knob.
	UPROPERTY(EditDefaultsOnly, Category = "Board|Level")
	bool bSpawnPlaceholderGround = true;

private:
	UPROPERTY(Transient)
	TObjectPtr<ABoardActor> SpawnedBoard;
};
