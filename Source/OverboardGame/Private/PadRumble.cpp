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
	// One buzz, and how often a held level starts the next one (they overlap, so it reads as one).
	constexpr double kBuzzSeconds = 0.12;
	constexpr double kBuzzEverySeconds = 0.10;
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
		HapticEngine = [Engine retain];
		UE_LOG(LogOverboardRumble, Log, TEXT("PadRumble: native haptics running on '%s'."), *FString(Pad.vendorName ? Pad.vendorName : @"pad"));
		return true;
	}
}

void FPadRumble::StopNative()
{
	@autoreleasepool
	{
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
	// A DualSense on macOS (Bluetooth, tested 2026-10-05 with tools/play/haptics_probe) accepts
	// the PLAIN pattern player and rejects the ADVANCED one ("Couldn't communicate with a helper
	// application", then every later player fails). A plain player cannot change its intensity
	// once started, so a held level is a train of short overlapping buzzes, each started at the
	// level asked for at that moment.
	Level = FMath::Clamp(Level, 0.f, 1.f);
	const double Now = FPlatformTime::Seconds();
	if (Level <= 0.f)
	{
		return; // the last buzz ends by itself within kBuzzSeconds
	}
	if (!bNativeReady)
	{
		if (Now < NextRetrySeconds)
		{
			return;
		}
		NextRetrySeconds = Now + kRetryEverySeconds;
		bNativeReady = StartNative();
		if (!bNativeReady)
		{
			return;
		}
	}
	if (Now < NextBuzzSeconds)
	{
		return;
	}
	NextBuzzSeconds = Now + kBuzzEverySeconds;

	@autoreleasepool
	{
		CHHapticEngine* Engine = (CHHapticEngine*)HapticEngine;
		CHHapticEventParameter* Intensity = [[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:Level] autorelease];
		CHHapticEventParameter* Sharpness = [[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:kSharpness] autorelease];
		CHHapticEvent* Event = [[[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
		                                                      parameters:@[Intensity, Sharpness]
		                                                    relativeTime:0
		                                                        duration:kBuzzSeconds] autorelease];
		NSError* Error = nil;
		CHHapticPattern* Pattern = [[[CHHapticPattern alloc] initWithEvents:@[Event] parameters:@[] error:&Error] autorelease];
		id<CHHapticPatternPlayer> Player = Pattern ? [Engine createPlayerWithPattern:Pattern error:&Error] : nil;
		if (Player == nil || ![Player startAtTime:CHHapticTimeImmediate error:&Error])
		{
			// Usually the pad disconnected or the system stopped the engine. Start again later.
			UE_LOG(LogOverboardRumble, Log, TEXT("PadRumble: buzz failed (%s); will retry."), Error ? *FString(Error.localizedDescription) : TEXT("unknown"));
			StopNative();
		}
	}
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
