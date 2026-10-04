#include "PadRumble.h"

#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"

#if PLATFORM_MAC
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>
#endif

DEFINE_LOG_CATEGORY_STATIC(LogOverboardRumble, Log, All);

namespace
{
	// Low sharpness = a dull, low-frequency buzz, the nearest a pad gets to the real board's
	// ~70 Hz motor buzz.
	constexpr float kSharpness = 0.1f;
	// The continuous event loops; its own length only sets how often the loop restarts.
	constexpr double kEventSeconds = 30.0;
	constexpr double kRetryEverySeconds = 2.0;
}

FPadRumble::~FPadRumble()
{
	Shutdown();
}

void FPadRumble::Shutdown()
{
#if PLATFORM_MAC
	StopNative();
#endif
	LastLevel = -1.f;
}

#if PLATFORM_MAC

bool FPadRumble::StartNative()
{
	@autoreleasepool
	{
		GCController* Pad = GCController.current ? GCController.current : GCController.controllers.firstObject;
		if (Pad == nil || Pad.haptics == nil)
		{
			return false;
		}

		CHHapticEngine* Engine = [Pad.haptics createEngineWithLocality:GCHapticsLocalityDefault];
		if (Engine == nil)
		{
			UE_LOG(LogOverboardRumble, Warning, TEXT("PadRumble: the pad has haptics but no engine for the default locality."));
			return false;
		}
		Engine.playsHapticsOnly = YES;

		NSError* Error = nil;
		if (![Engine startAndReturnError:&Error])
		{
			UE_LOG(LogOverboardRumble, Warning, TEXT("PadRumble: haptic engine did not start: %s"), *FString(Error.localizedDescription));
			return false;
		}

		CHHapticEventParameter* Intensity = [[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f] autorelease];
		CHHapticEventParameter* Sharpness = [[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:kSharpness] autorelease];
		CHHapticEvent* Event = [[[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
		                                                      parameters:@[Intensity, Sharpness]
		                                                    relativeTime:0
		                                                        duration:kEventSeconds] autorelease];
		CHHapticPattern* Pattern = [[[CHHapticPattern alloc] initWithEvents:@[Event] parameters:@[] error:&Error] autorelease];
		id<CHHapticAdvancedPatternPlayer> Player = Pattern ? [Engine createAdvancedPlayerWithPattern:Pattern error:&Error] : nil;
		if (Player == nil)
		{
			UE_LOG(LogOverboardRumble, Warning, TEXT("PadRumble: no pattern player: %s"), Error ? *FString(Error.localizedDescription) : TEXT("unknown"));
			[Engine stopWithCompletionHandler:nil];
			return false;
		}
		Player.loopEnabled = YES;

		// Start silent; SetLevel raises the intensity.
		CHHapticDynamicParameter* Zero = [[[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:0.f relativeTime:0] autorelease];
		[Player sendParameters:@[Zero] atTime:CHHapticTimeImmediate error:nil];
		if (![Player startAtTime:CHHapticTimeImmediate error:&Error])
		{
			UE_LOG(LogOverboardRumble, Warning, TEXT("PadRumble: pattern player did not start: %s"), *FString(Error.localizedDescription));
			[Engine stopWithCompletionHandler:nil];
			return false;
		}

		HapticEngine = [Engine retain];
		HapticPlayer = [Player retain];
		UE_LOG(LogOverboardRumble, Log, TEXT("PadRumble: native haptics running on '%s'."), *FString(Pad.vendorName ? Pad.vendorName : @"pad"));
		return true;
	}
}

void FPadRumble::StopNative()
{
	@autoreleasepool
	{
		if (HapticPlayer)
		{
			id<CHHapticAdvancedPatternPlayer> Player = (id<CHHapticAdvancedPatternPlayer>)HapticPlayer;
			[Player stopAtTime:CHHapticTimeImmediate error:nil];
			[Player release];
			HapticPlayer = nullptr;
		}
		if (HapticEngine)
		{
			CHHapticEngine* Engine = (CHHapticEngine*)HapticEngine;
			[Engine stopWithCompletionHandler:nil];
			[Engine release];
			HapticEngine = nullptr;
		}
	}
	bNativeReady = false;
}

void FPadRumble::SetLevel(APlayerController* /*PC*/, float Level)
{
	Level = FMath::Clamp(Level, 0.f, 1.f);
	if (!bNativeReady)
	{
		const double Now = FPlatformTime::Seconds();
		if (Level <= 0.f || Now < NextRetrySeconds)
		{
			return; // nothing to play, or a recent start failed -- do not hammer the framework
		}
		NextRetrySeconds = Now + kRetryEverySeconds;
		bNativeReady = StartNative();
		LastLevel = -1.f;
		if (!bNativeReady)
		{
			return;
		}
	}
	if (FMath::Abs(Level - LastLevel) < 0.01f)
	{
		return;
	}

	@autoreleasepool
	{
		id<CHHapticAdvancedPatternPlayer> Player = (id<CHHapticAdvancedPatternPlayer>)HapticPlayer;
		CHHapticDynamicParameter* Param = [[[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:Level relativeTime:0] autorelease];
		NSError* Error = nil;
		if (![Player sendParameters:@[Param] atTime:CHHapticTimeImmediate error:&Error])
		{
			// Usually the pad disconnected or the engine was stopped by the system. Start again later.
			UE_LOG(LogOverboardRumble, Log, TEXT("PadRumble: lost the haptic engine (%s); will retry."), Error ? *FString(Error.localizedDescription) : TEXT("unknown"));
			StopNative();
			return;
		}
	}
	LastLevel = Level;
}

#else // !PLATFORM_MAC

void FPadRumble::SetLevel(APlayerController* PC, float Level)
{
	Level = FMath::Clamp(Level, 0.f, 1.f);
	if (!PC || FMath::Abs(Level - LastLevel) < 0.01f)
	{
		return;
	}
	if (DynamicHandle == 0)
	{
		DynamicHandle = PC->PlayDynamicForceFeedback(Level, -1.f, true, false, true, false, EDynamicForceFeedbackAction::Start);
	}
	else
	{
		PC->PlayDynamicForceFeedback(Level, -1.f, true, false, true, false, EDynamicForceFeedbackAction::Update, DynamicHandle);
	}
	LastLevel = Level;
}

#endif
