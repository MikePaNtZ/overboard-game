// OverboardPlayerController.h
//
// The player is the RIDER'S BRAIN. sim-host (MuJoCo) owns every board physics quantity; this
// class only turns pad and keyboard input into the InputIn packet once per frame, and turns the
// StateOut flags into output cues (rumble). It computes no board physics.
//
// Control design (Mike, 2026-10-04 -- see docs/playable-status.md):
//   - There is no speed loop. Fore/aft is a LEAN: lean forward to speed up, lean back to slow
//     down. Centre stick coasts.
//   - Steer is the rider's CARVE INTENT. Under sim-host --lean-steer the rider model turns it
//     into hips and deck bank; weight_shift_lateral has no effect there and is sent as 0.
//   - A hard lean back drags the tail pad and stops the board. That is a wanted brake, not a
//     fall, so L2 makes it easy and sure.
//
// PS5 DualSense mapping (Mike's choices):
//   Left stick Y   -> weight_shift_fore_aft: dead zone 0.10, curve 0.5x + 0.5x^3, NO smoothing
//                     (the sim's own ballast lag is the "weight"; a second filter here is latency)
//   Right stick X  -> steer (positive = turn right)
//   L2 (analog)    -> hard lean back: fore_aft = min(stick, -L2), so a full pull is always -1
//   Cross          -> arm (sim-host --hold-until-arm holds the board until the first arm bit)
//   Circle         -> reset
//   Options        -> camera cycle
// Keyboard fallback: W/S or Up/Down lean, A/D or Left/Right steer, Left Shift tail brake,
// Space arm, R reset, C camera, Esc quit. Digital keys ramp (KeyboardRampSpeed); the pad does not.
//
// Rumble: StateOut bit 5 (rider warning, pulsed) -> 2 Hz on/off; bit 6 (solid) -> continuous;
// a fall (bit 2) or a handoff (bit 4) -> one short jolt, then silence. See FPadRumble for why the
// Mac path does not use Unreal's force feedback.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "PadRumble.h"
#include "OverboardPlayerController.generated.h"

class UInputMappingContext;
class UInputAction;
struct FInputActionValue;
class FSocket;
class FInternetAddr;
class ABoardActor;

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

	// THE FIX (overboard#162): AddMappingContext must happen here, where GetLocalPlayer() is
	// guaranteed non-null, not in SetupInputComponent (where it silently did nothing in -game).
	virtual void ReceivedPlayer() override;

	// The values sent in the newest InputIn packet, for the HUD and the self-test.
	float GetLastSentForeAft() const { return LastSentForeAft; }
	float GetLastSentSteer() const { return LastSentSteer; }
	bool IsArmSent() const { return bArmHeld; }

	// The rider-warning cue phase, shared by rumble and HUD so both blink together.
	// 2 Hz: on for the first 0.25 s of every 0.5 s.
	static bool WarningPulseOn(double RealTimeSeconds) { return FMath::Fmod(RealTimeSeconds, 0.5) < 0.25; }

protected:
	// Below this magnitude a raw stick reads as zero (DualSense centre noise is a few percent).
	UPROPERTY(EditAnywhere, Category = "Board|Input", meta = (ClampMin = "0.0", ClampMax = "0.9"))
	float StickDeadzone = 0.10f;

	// Lean curve: out = (1 - k) x + k x^3 past the dead zone. k = 0.5 is Mike's choice: small
	// leans still answer (a weight, not a switch), and the edge stays near linear.
	UPROPERTY(EditAnywhere, Category = "Board|Input", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LeanCubicBlend = 0.5f;

	// Below this L2 travel the tail brake reads as zero, so a resting finger does not brake.
	UPROPERTY(EditAnywhere, Category = "Board|Input", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float TriggerDeadzone = 0.05f;

	// FInterpTo speed for the KEYBOARD path only: a key jumps 0 -> 1, a ramp makes it a lean.
	UPROPERTY(EditAnywhere, Category = "Board|Input", meta = (ClampMin = "0.5", ClampMax = "20.0"))
	float KeyboardRampSpeed = 3.0f;

	// Rumble levels, 0..1.
	UPROPERTY(EditAnywhere, Category = "Board|Rumble")
	float RumblePulsedLevel = 0.55f;
	UPROPERTY(EditAnywhere, Category = "Board|Rumble")
	float RumbleSolidLevel = 0.85f;
	UPROPERTY(EditAnywhere, Category = "Board|Rumble")
	float RumbleFallLevel = 1.0f;
	UPROPERTY(EditAnywhere, Category = "Board|Rumble")
	float RumbleFallSeconds = 0.35f;

private:
	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MappingContext;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_LeanPad;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_LeanKeys;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_SteerPad;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_SteerKeys;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_TailBrake;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Arm;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Reset;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_CameraCycle;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Quit;

	// Raw values from Enhanced Input, written by the handlers.
	float PadLean = 0.f;
	float KeyLean = 0.f;
	float PadSteer = 0.f;
	float KeySteer = 0.f;
	float TailBrake = 0.f;
	bool bArmHeld = false;
	bool bResetHeld = false;

	// Keyboard ramps.
	float SmoothedKeyLean = 0.f;
	float SmoothedKeySteer = 0.f;

	float LastSentForeAft = 0.f;
	float LastSentSteer = 0.f;

	FSocket* SendSocket = nullptr;
	TSharedPtr<FInternetAddr> HostAddr;
	uint64 SendSeq = 0; // monotonic; never reset while the socket is open, so the host can detect loss

	// Fall handling (bit 2 fallen, bit 4 handoff).
	bool bWasFallenLastTick = false;
	bool bAutoResetPending = false;
	double FallJoltUntilSeconds = 0.0;
	bool bWasDownLastTick = false;

	FPadRumble Rumble;

	const ABoardActor* FindBoard() const;
	void CheckForAutoResetOnFall(const ABoardActor* Board);
	void UpdateRumble(const ABoardActor* Board);

	void OnLeanPad(const FInputActionValue& Value);
	void OnLeanKeys(const FInputActionValue& Value);
	void OnSteerPad(const FInputActionValue& Value);
	void OnSteerKeys(const FInputActionValue& Value);
	void OnTailBrake(const FInputActionValue& Value);
	void OnArm(const FInputActionValue& Value);
	void OnReset(const FInputActionValue& Value);
	void OnCameraCycle(const FInputActionValue& Value);
	void OnQuit(const FInputActionValue& Value);

	float ShapeLean(float Raw) const;
	float ShapeSteer(float Raw) const;
	void SendInputPacket(float DeltaTime);
	void RunInputSelfTest();
};
