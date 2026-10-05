// PadRumble -- one continuous, low-frequency rumble level on the player's gamepad.
//
// Why this class exists: UE 5.7's Mac controller layer (AppleControllerInterface.h) implements
// SetForceFeedbackChannelValue as an EMPTY function, so every standard Unreal rumble call is
// silently dropped on a Mac. On Mac this class drives the pad itself through Apple's
// GameController haptics (GCController.haptics -> Core Haptics) with plain pattern players:
// a held level is a train of short overlapping buzzes (the advanced player, which could change
// the intensity live, is refused by macOS for a DualSense). On other platforms it uses
// APlayerController::PlayDynamicForceFeedback on the large (low-frequency) motors.
//
// The level is a pure output cue. Nothing here reads or changes board physics.

#pragma once

#include "CoreMinimal.h"

class APlayerController;

class OVERBOARDGAME_API FPadRumble
{
public:
	~FPadRumble();

	// Intensity in [0, 1]. Call every frame; repeated equal values cost nothing.
	void SetLevel(APlayerController* PC, float Intensity);

	// Stops the rumble and releases the haptic engine.
	void Shutdown();

	// True once a haptic engine runs on a connected pad (Mac only; always false elsewhere).
	bool HasNativeHaptics() const { return bNativeReady; }

private:
	float LastLevel = -1.f;
	bool bNativeReady = false;
	double NextRetrySeconds = 0.0;

#if PLATFORM_MAC
	void* HapticEngine = nullptr; // CHHapticEngine*, retained
	double NextBuzzSeconds = 0.0;
	bool StartNative();
	void StopNative();
#else
	uint64 DynamicHandle = 0;
#endif
};
