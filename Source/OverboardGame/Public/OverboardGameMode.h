// OverboardGameMode.h
//
// Spawns a flat ground plane, motion-reference marker scenery, the board actor, and (W2+) a
// chase camera pawn purely in C++, so the scene exists regardless of what (if anything) is
// authored in the level itself.
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

	// overboard#162: a chase camera following the board's position/yaw over a featureless plane
	// gives the scene no motion reference -- the board reads as pinned to the centre of frame
	// with nothing to move relative to (this is what "the turn just looks like sideways drift"
	// actually was; the carve itself was always correct in the data, see PR). Scatters simple
	// primitive markers (scenery only, collision off) across roughly the area a run covers.
	// Same per-level toggle pattern as bSpawnPlaceholderGround, suppressed together with it by
	// AOverboardGameMode_NoGround -- a real imported environment supplies its own landmarks and
	// should not also get this placeholder field scattered across it.
	UPROPERTY(EditDefaultsOnly, Category = "Board|Level")
	bool bSpawnMotionReferenceMarkers = true;

	// Whether this level's PlayerStart defines where MuJoCo's origin sits in the world.
	//
	// Default FALSE, and that default is load-bearing. OB_Main has a PlayerStart at (0, 0, 92) --
	// the ordinary "lift a pawn clear of the floor" offset every default level ships with. An
	// earlier revision read the first PlayerStart unconditionally, which silently raised the board
	// 92 cm above OB_Main's placeholder ground while claiming to change nothing there. A level has
	// to opt in, because a PlayerStart's Z means "where a pawn stands", not "where the ground is",
	// and those differ by exactly the amount that makes a board look like it is hovering.
	//
	// Set true only by AOverboardGameMode_NoGround: a level that supplies its own environment is
	// the only kind that needs to say where in that environment the board belongs.
	UPROPERTY(EditDefaultsOnly, Category = "Board|Level")
	bool bUsePlayerStartAsWorldOrigin = false;

private:
	UPROPERTY(Transient)
	TObjectPtr<ABoardActor> SpawnedBoard;

	// Scatters primitive marker actors on a grid -- see bSpawnMotionReferenceMarkers.
	void SpawnMotionReferenceMarkers(UWorld* World) const;
};
