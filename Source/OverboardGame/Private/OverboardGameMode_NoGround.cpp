#include "OverboardGameMode_NoGround.h"

AOverboardGameMode_NoGround::AOverboardGameMode_NoGround()
{
	bSpawnPlaceholderGround = false;
	// A real imported environment supplies its own landmarks -- don't also scatter the
	// placeholder motion-reference field across it (overboard#162).
	bSpawnMotionReferenceMarkers = false;
	// ...and it is the only kind of level that needs to say WHERE in itself the board goes, so
	// this is the only class that reads the PlayerStart. See bUsePlayerStartAsWorldOrigin.
	bUsePlayerStartAsWorldOrigin = true;
}
