// OverboardGameMode_NoGround.h
//
// overboard#162: select this class directly from a level's World Settings -> GameMode Override
// dropdown to suppress the placeholder ground plane AND the placeholder motion-reference marker
// field -- e.g. an imported environment (Fab, etc.) that supplies its own floor and landmarks.
// It is a plain native C++ class, not a Blueprint, specifically so nobody has to author a
// Blueprint by hand to get this: native classes show up in the GameMode Override picker exactly
// the same as Blueprint ones.
//
// Everything else -- board actor, camera pawn, player controller -- spawns identically to
// AOverboardGameMode; only the placeholder scenery is suppressed. See OverboardGameMode.h.
#pragma once

#include "CoreMinimal.h"
#include "OverboardGameMode.h"
#include "OverboardGameMode_NoGround.generated.h"

UCLASS()
class OVERBOARDGAME_API AOverboardGameMode_NoGround : public AOverboardGameMode
{
	GENERATED_BODY()

public:
	AOverboardGameMode_NoGround();
};
