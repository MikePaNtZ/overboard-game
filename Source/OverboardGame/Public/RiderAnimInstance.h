// RiderAnimInstance.h
//
// The riding blendspace plus a procedural layer on top, all in C++ (no AnimBP asset).
//
// URiderAnimInstance is a UAnimSingleNodeInstance, so the blendspace plays exactly as before.
// Its proxy then edits the evaluated pose:
//   - the whole body leans over the PLANTED feet (pelvis subtree rotated about the feet), by the
//     lean vector the board actor computes from the sim's ballast displacement;
//   - hips drop (crouch) and two-bone IK puts the feet back where the blendspace had them, which
//     is what bends the knees and hips;
//   - the chest turns a little into the carve, the arms balance outward with the turn;
//   - a slow breathing motion in the chest;
//   - the head turns to look along the direction of travel.
//
// Which inputs are simulated and which are invented is the board actor's business -- see
// ABoardActor::UpdateRenderRider. This class only applies what it is given.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/AnimSingleNodeInstanceProxy.h"
#include "RiderAnimInstance.generated.h"

struct FRiderProceduralInputs
{
	FVector LeanVecCS = FVector::ZeroVector; // horizontal; direction the head goes, length = degrees
	FVector TravelDirCS = FVector(0, 1, 0);  // horizontal unit vector, direction of travel
	float TurnSigned = 0.f;                  // -1..1, + = turning towards +Side (Up x Travel)
	float CrouchCm = 0.f;                    // pelvis drop
	float TimeS = 0.f;                       // deterministic clock for breathing and balance motion
	float HeadLookWeight = 1.f;
	float FootLiftCm[2] = {0.f, 0.f};        // per-foot IK target lift (component Z), feet onto the pads
	FVector SoleLocal[2][2];                 // [foot][heel, toe] sole points in the foot bone frame
	bool bFlattenFeet = false;               // turn each foot so its sole lies flat (heel and toe level)
	bool bEnabled = false;
};

struct FRiderAnimInstanceProxy : public FAnimSingleNodeInstanceProxy
{
	FRiderAnimInstanceProxy() = default;
	FRiderAnimInstanceProxy(UAnimInstance* InAnimInstance) : FAnimSingleNodeInstanceProxy(InAnimInstance) {}

	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;

private:
	FRiderProceduralInputs Inputs;
	bool bLoggedMissingBones = false;
};

UCLASS(transient, NotBlueprintable)
class OVERBOARDGAME_API URiderAnimInstance : public UAnimSingleNodeInstance
{
	GENERATED_BODY()

public:
	FRiderProceduralInputs Inputs; // written on the game thread, copied by the proxy in PreUpdate

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
};
