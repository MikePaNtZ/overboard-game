// OverboardCameraPawn.h
//
// W2 (overboard#162): "someone who isn't us picks up a controller and doesn't immediately put
// it down" needs something to look through. There was no pawn/camera at all in W1
// (AOverboardGameMode::DefaultPawnClass was nullptr -- the board's pose came entirely from the
// wire, not a pawn, and that's still true; this class doesn't change how the board is driven).
//
// A minimal chase camera: a spring arm behind and above whatever ABoardActor it's following,
// smoothly chasing the board's LOCATION and YAW only -- it deliberately does not inherit the
// board's pitch/roll (leaning while balancing would otherwise tip the camera too, which reads as
// motion sickness, not "feel"). AutoPossessPlayer handles single-player possession without any
// GameMode/PlayerStart ceremony; AOverboardGameMode spawns one instance alongside the board.
//
// HONESTY NOTE: first human verification (Mike's PIE session, overboard#162) confirmed the
// follow logic works and the board stays in view -- feedback was "a little too far out (minor)",
// which pulled ArmLengthCm in modestly. Pitch angle and follow speeds are still first-guess,
// unverified. See the PR for exactly what "verified running" does and does not cover here.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "OverboardCameraPawn.generated.h"

class USpringArmComponent;
class UCameraComponent;
class ABoardActor;

UCLASS()
class OVERBOARDGAME_API AOverboardCameraPawn : public APawn
{
	GENERATED_BODY()

public:
	AOverboardCameraPawn();

	virtual void Tick(float DeltaSeconds) override;

protected:
	UPROPERTY(VisibleAnywhere, Category = "Camera")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	TObjectPtr<USpringArmComponent> SpringArm;

	UPROPERTY(VisibleAnywhere, Category = "Camera")
	TObjectPtr<UCameraComponent> Camera;

	// Framing/follow tunables. First human verification (overboard#162, Mike's first PIE
	// session): follow logic works, board stays in view the whole time, framing is "a little too
	// far out (minor)". ArmLengthCm tightened modestly off that feedback (650 -> 480) --
	// deliberately not a close chase cam, since the board covers real ground under lean and
	// losing it off-frame is worse than it reading slightly small. The rest are still unverified
	// first guesses. All named so they're the obvious thing to retune further.
	UPROPERTY(EditAnywhere, Category = "Board|Camera")
	float ArmLengthCm = 480.f;

	UPROPERTY(EditAnywhere, Category = "Board|Camera")
	float ArmPitchDeg = -18.f;

	UPROPERTY(EditAnywhere, Category = "Board|Camera")
	float FollowHeightOffsetCm = 60.f;

	UPROPERTY(EditAnywhere, Category = "Board|Camera")
	float FollowLocationSpeed = 4.f; // FInterpTo speed, position

	// overboard#162: reduced from 2.0. This reverses an earlier decision, deliberately -- an
	// intermediate attempt at lagging this same speed was explicitly NOT landed because at the
	// time "turn not discernible" turned out to be the host sending a yaw-free quaternion (fixed
	// upstream in overboard/#175), so a camera fix would have been compensating for a bug that no
	// longer existed. That reasoning was right then and is confirmed wrong now: the board
	// genuinely yaws (peak-to-peak swing measured at 162 degrees), and the chase camera follows
	// that yaw so closely that the board stays pinned facing up-screen and only the world rotates
	// around it -- the carve is real and still invisible. At Speed=2.0 a first-order lag's
	// steady-state trailing angle for a turn is approximately (turn rate / Speed), which for a
	// multi-second 162 degree swing was only a few degrees -- not enough to read. Lower is more
	// lag; tune against real footage, not this reasoning -- this is a starting point, not a
	// verified value (no display in this environment to check it against).
	UPROPERTY(EditAnywhere, Category = "Board|Camera")
	float FollowYawSpeed = 0.6f; // FInterpTo speed, yaw only

private:
	TWeakObjectPtr<ABoardActor> FollowTarget;

	// Finds the board actor lazily rather than requiring GameMode to wire it up, so spawn order
	// between the camera pawn and the board actor doesn't matter.
	void TryAcquireFollowTarget();
};
