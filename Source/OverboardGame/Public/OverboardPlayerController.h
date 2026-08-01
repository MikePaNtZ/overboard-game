// OverboardPlayerController.h
//
// W2 (overboard#162): the real stick-to-channel mapping, deadzone/curve shaping, and a
// per-frame send, now that the CEO has ruled on control mode.
//
// THE CONTROL MODE MATTERS FOR HOW THIS READS: the outer velocity loop is ON (Senior Controls is
// closing it now) -- the board station-keeps. Centre stick means "hold position," not "no
// input"; the host actively parks the board when the stick is centred. That makes the deadzone
// load-bearing in a way it wouldn't be for a "shove and coast" control mode: a small
// uncorrected drift on a centred stick becomes a permanent slow creep rather than something
// friction eventually eats, because the host is actively trying to null out whatever ground-speed
// command it's given. This class's only job is to hand the host a clean, deadzoned, clamped
// [-1,1] command every frame; it does no station-keeping itself.
//
// Mapping (per #162's W2 dispatch):
//   left stick fore/aft (Gamepad_LeftY)  -> weight_shift_fore_aft (ground-speed command)
//   right stick lateral (Gamepad_RightX) -> weight_shift_lateral AND steer, both from the same
//                                            physical axis (lean-to-steer: leaning is how you
//                                            steer, so one stick axis legitimately drives both
//                                            wire channels)
//
// steer is explicitly a NON-PHYSICAL game channel (the simulated wheel is a cylinder and cannot
// carve) -- see the wire spec and the repo README's "what is deliberately not physical" section.
// Nothing produced via this channel may be presented as a controls result.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "OverboardPlayerController.generated.h"

class UInputMappingContext;
class UInputAction;
struct FInputActionValue;
class FSocket;
class FInternetAddr;

UCLASS()
class OVERBOARDGAME_API AOverboardPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AOverboardPlayerController();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;

protected:
	// Below this magnitude (post-normalization, [0,1] range) a raw stick reads as zero. Sized
	// comfortably above typical Xbox/PS5 stick centre noise (a couple of percent) so a centred
	// stick genuinely commands zero rather than a slow creep once the host is actively
	// station-keeping against whatever it's told. Named/tunable rather than a magic number so
	// this can be retuned once someone actually drives it.
	UPROPERTY(EditAnywhere, Category = "Board|Input", meta = (ClampMin = "0.0", ClampMax = "0.9"))
	float StickDeadzone = 0.12f;

	// Response curve applied past the deadzone: output = sign(x) * rescaled(x) ^ ResponseCurveExponent,
	// where rescaled(x) remaps [Deadzone, 1] back to [0, 1] so the curve starts at 0 right past the
	// deadzone edge instead of jumping. 1.0 = linear. >1.0 gives finer control near centre (small
	// stick deflection -> even smaller command) while still reaching +-1 at full deflection --
	// chosen because centre-stick is now "hold position", so small corrections near centre matter
	// more than they would under a "shove and coast" control mode. 2.0 is a starting point, not a
	// measured value; retune once someone has actually driven it (see PR).
	UPROPERTY(EditAnywhere, Category = "Board|Input", meta = (ClampMin = "1.0", ClampMax = "4.0"))
	float ResponseCurveExponent = 2.0f;

private:
	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MappingContext;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_WeightShiftForeAft;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_WeightShiftLateral;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Steer; // NON-PHYSICAL game channel
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Arm;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Reset;

	// Raw, unshaped stick state as last reported by Enhanced Input. Deadzone/curve shaping and
	// clamping happen in SendInputPacket, right before encoding -- not here -- so there's exactly
	// one place that turns "what the stick is doing" into "what goes on the wire".
	float CurrentForeAft = 0.f;
	float CurrentLateral = 0.f;
	float CurrentSteer = 0.f;
	bool bArmHeld = false;
	bool bResetHeld = false;

	FSocket* SendSocket = nullptr;
	TSharedPtr<FInternetAddr> HostAddr;
	uint64 SendSeq = 0; // monotonic; never reset while the socket is open, so the host can detect loss

	void OnWeightShiftForeAft(const FInputActionValue& Value);
	void OnWeightShiftLateral(const FInputActionValue& Value);
	void OnSteer(const FInputActionValue& Value);
	void OnArm(const FInputActionValue& Value);
	void OnReset(const FInputActionValue& Value);

	// Deadzone + response curve, applied to a raw stick axis already in [-1,1]. Does not clamp
	// its own output beyond what the curve already guarantees -- SendInputPacket clamps again
	// right before the wire regardless, per the "do not rely on the host to sanitise" rule.
	float ShapeAxis(float Raw) const;

	void SendInputPacket();
};
