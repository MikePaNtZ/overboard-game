#include "OverboardHUD.h"

#include "BoardActor.h"
#include "TerrainVerification.g.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Logging/LogMacros.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogOverboardHUD, Log, All);

namespace
{
	// The banner. Text is deliberately about the MACHINERY, never about an outcome -- see the
	// header's "must not become a behavioural claim". A run that recovers must not have been
	// told it was going to crash.
	const TCHAR* kBannerTitle = TEXT("LOSS OF PITCH AUTHORITY");
	const TCHAR* kBannerDetail = TEXT("commanded current at the envelope limit, below speed-cap onset");

	const TCHAR* kTerrainTagTitle = TEXT("TERRAIN UNVERIFIED");

	// overboard-game#28. Deliberately calm wording -- this is a normal mode, not a fault -- and
	// deliberately not phrased as an outcome, same "machinery, not a claim" rule as the authority
	// banner: it says what input engages the board, not anything about what will happen next.
	const TCHAR* kEngageBannerTitle = TEXT("DISENGAGED");
	const TCHAR* kEngageBannerDetail = TEXT("press SPACE or GAMEPAD A to ride");

	constexpr float kBannerHeightPx = 96.f;
	constexpr float kTerrainTagHeightPx = 34.f;
	constexpr float kEngageBannerHeightPx = 80.f;
}

AOverboardHUD::AOverboardHUD()
{
	PrimaryActorTick.bCanEverTick = false; // DrawHUD is driven by the renderer, not by Tick
}

void AOverboardHUD::ResolveTerrainVerificationOnce()
{
	if (bTerrainResolved)
	{
		return;
	}
	bTerrainResolved = true;

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// GetMapName() carries the PIE prefix ("UEDPIE_0_OB_City") in an editor session and does not
	// in a packaged build. Strip it -- otherwise every PIE run reports an unknown level, which
	// IsLevelVerified() correctly treats as unverified, so the tag would be permanently on
	// screen in the editor and a reader would stop trusting it. A warning nobody believes is
	// the failure mode this whole banner exists to avoid.
	TerrainLevelName = UWorld::RemovePIEPrefix(World->GetMapName());

	bTerrainUnverified = !OverboardTerrainVerification::IsLevelVerified(
		TCHAR_TO_UTF8(*TerrainLevelName));

	UE_LOG(LogOverboardHUD, Log,
		TEXT("terrain verification for level '%s': %s (from the table generated out of ")
		TEXT("terrain/levels/, ADR-0011 condition 2)"),
		*TerrainLevelName, bTerrainUnverified ? TEXT("UNVERIFIED") : TEXT("verified"));
}

ABoardActor* AOverboardHUD::FindBoard() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<ABoardActor> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void AOverboardHUD::DrawHUD()
{
	Super::DrawHUD();

	if (!Canvas)
	{
		return;
	}

	ResolveTerrainVerificationOnce();
	DrawTerrainTag();

	ABoardActor* Board = FindBoard();
	if (!Board)
	{
		return;
	}

	// overboard-game#28. Drawn/logged before the authority-warning banner below on purpose: the
	// two are mutually exclusive in practice (the authority warning implies the board is moving,
	// which implies engaged), but resolving that by ORDER here rather than by adding a runtime
	// guard keeps this simple and keeps the loss-of-authority banner's own logic -- which the ADR
	// leans on for lead time -- untouched.
	{
		const bool bEngagedNow = Board->IsEngaged();
		if (bEngagedNow != bWasEngagedLastFrame)
		{
			UE_LOG(LogOverboardHUD, Log, TEXT("engage banner: state changed to %s"),
				bEngagedNow ? TEXT("ENGAGED") : TEXT("DISENGAGED"));
		}
		bWasEngagedLastFrame = bEngagedNow;

		if (!bEngagedNow)
		{
			DrawEngageBanner();
		}
	}

	const bool bWarningNow = Board->IsAuthorityWarning();
	const double Now = FPlatformTime::Seconds();

	// ---- Rising edge: ZERO debounce ------------------------------------------------------
	//
	// The banner goes up on the first frame the bit is seen. ADR-0011 is relying on 2.868 s of
	// lead against `FALLEN`, and issue #19 requires that lead to survive whatever the client
	// adds. Any smoothing, confirmation window or n-of-m filter here would come straight out of
	// it, so there is none.
	if (bWarningNow && !bWarningLastFrame)
	{
		bBannerVisible = true;
		bLoggedThisEvent = false;
	}

	if (bWarningNow)
	{
		BannerHoldUntilSeconds = Now + MinimumHoldSeconds;
	}
	else if (bBannerVisible && Now >= BannerHoldUntilSeconds)
	{
		// ---- Clearing edge: hysteresis lives HERE, where it cannot cost lead --------------
		bBannerVisible = false;
		UE_LOG(LogOverboardHUD, Log,
			TEXT("loss-of-authority banner cleared (held %.2fs past the signal)"), MinimumHoldSeconds);
	}

	bWarningLastFrame = bWarningNow;

	if (!bBannerVisible)
	{
		return;
	}

	// ---- The measured number issue #19 asks for -------------------------------------------
	//
	// AC2 wants the END-TO-END lead -- "detection to something the player can perceive" -- as a
	// measured number rather than the host-side 2.868 s quoted forward. The host-side half is
	// measured in overboard#205; this is the piece that was missing, and it is measured HERE
	// rather than in the actor because here is the first moment anything has actually been
	// drawn. A latency logged from Tick would be measuring the intent to draw.
	//
	// What is measured: the wall-clock interval from the UDP receive timestamp of the packet
	// that first carried the bit (FBoardStateClient stamps every sample on arrival, on the
	// socket thread) to this DrawHUD call. Everything after this point is the renderer's own
	// present latency, which is outside this repo and identical for every element on screen.
	const bool bJustRaised = !bLoggedThisEvent;
	if (bJustRaised)
	{
		bLoggedThisEvent = true;
		const double ArrivalSeconds = Board->GetAuthorityWarningArrivalTimeSeconds();
		if (ArrivalSeconds > 0.0)
		{
			UE_LOG(LogOverboardHUD, Warning,
				TEXT("LOSS OF PITCH AUTHORITY drawn. Receipt-to-draw latency %.1f ms ")
				TEXT("(packet arrived at %.6f, drawn at %.6f). ADR-0011 measures the host-side ")
				TEXT("warning leading FALLEN by 2.868 s; this latency is what the client spends of it."),
				(Now - ArrivalSeconds) * 1000.0, ArrivalSeconds, Now);
		}
		else
		{
			UE_LOG(LogOverboardHUD, Warning,
				TEXT("LOSS OF PITCH AUTHORITY drawn, but no arrival timestamp was recorded -- ")
				TEXT("latency not measured this event."));
		}
	}

	// Pulse. Motion is what gets a peripheral element noticed; a static bar is easy to stop
	// seeing. Never drops below half opacity, so the banner is legible at every phase rather
	// than blinking out.
	const float Pulse = 0.75f + 0.25f * FMath::Sin(static_cast<float>(Now) * 2.f * PI * PulseHz);
	DrawAuthorityBanner(Pulse);

	// -ObAuthorityShot: capture the frame the banner is FIRST drawn on.
	//
	// Issue #19 AC3 wants this demonstrated by triggering it, not by reading the code, and the
	// evidence for "the player can see it" is a frame with it in. An externally timed capture
	// (`screencapture` on a sleep) can miss the window and cannot prove which frame it caught;
	// requesting it from inside the draw call cannot. Off unless the switch is passed, so it
	// costs a released build nothing.
	if (bJustRaised && FParse::Param(FCommandLine::Get(), TEXT("ObAuthorityShot")))
	{
		FScreenshotRequest::RequestScreenshot(false);
		UE_LOG(LogOverboardHUD, Warning, TEXT("-ObAuthorityShot: screenshot requested on the first banner frame."));
	}
}

void AOverboardHUD::DrawAuthorityBanner(float Alpha)
{
	const float W = Canvas->SizeX;
	const float H = Canvas->SizeY;

	// Top-centre. Not a corner: the player is looking at the board, which is centre-frame, and a
	// corner element is exactly the thing peripheral vision drops under load.
	const float BarY = H * 0.06f;

	DrawRect(FLinearColor(0.65f * Alpha, 0.f, 0.f, 0.85f), 0.f, BarY, W, kBannerHeightPx);
	DrawRect(FLinearColor(1.0f * Alpha, 0.85f * Alpha, 0.f, 1.f), 0.f, BarY, W, 4.f);
	DrawRect(FLinearColor(1.0f * Alpha, 0.85f * Alpha, 0.f, 1.f), 0.f, BarY + kBannerHeightPx - 4.f, W, 4.f);

	float TitleW = 0.f, TitleH = 0.f;
	GetTextSize(kBannerTitle, TitleW, TitleH, GEngine->GetLargeFont(), 2.0f);
	DrawText(kBannerTitle, FLinearColor::White, (W - TitleW) * 0.5f, BarY + 16.f,
		GEngine->GetLargeFont(), 2.0f);

	float DetailW = 0.f, DetailH = 0.f;
	GetTextSize(kBannerDetail, DetailW, DetailH, GEngine->GetMediumFont(), 1.0f);
	DrawText(kBannerDetail, FLinearColor(1.f, 0.9f, 0.75f, 1.f), (W - DetailW) * 0.5f,
		BarY + 16.f + TitleH + 6.f, GEngine->GetMediumFont(), 1.0f);
}

void AOverboardHUD::DrawTerrainTag()
{
	if (!bTerrainUnverified)
	{
		return;
	}

	// Bottom-left, small, permanent. It has to be IN FRAME on any capture -- that is its whole
	// job -- without competing with the authority banner, which is a live event and this is not.
	const FString Text = FString::Printf(
		TEXT("%s  --  %s has no measured drivable surface (terrain/levels/%s.terrain)"),
		kTerrainTagTitle, *TerrainLevelName, *TerrainLevelName);

	float TW = 0.f, TH = 0.f;
	GetTextSize(*Text, TW, TH, GEngine->GetMediumFont(), 1.0f);

	const float X = 16.f;
	const float Y = Canvas->SizeY - kTerrainTagHeightPx - 16.f;

	DrawRect(FLinearColor(0.35f, 0.22f, 0.f, 0.8f), X - 8.f, Y - 6.f, TW + 16.f, TH + 12.f);
	DrawText(Text, FLinearColor(1.f, 0.85f, 0.4f, 1.f), X, Y, GEngine->GetMediumFont(), 1.0f);
}

void AOverboardHUD::DrawEngageBanner()
{
	// overboard-game#28. Bottom-centre, not top-centre: the authority banner already owns top-
	// centre and disengaged is not urgent enough to compete for the same real estate a player is
	// trained to glance at for danger. Steady blue, no pulse -- unlike DrawAuthorityBanner, this
	// is a mode the board sits in for as long as nobody has pressed start, not a fleeting event
	// that needs motion to get noticed, and a calm colour is deliberately the opposite of the red
	// authority banner so the two can never be confused for each other, including by someone who
	// glances at a screenshot without reading either line of text.
	const float W = Canvas->SizeX;
	const float H = Canvas->SizeY;
	const float BarY = H * 0.8f;

	DrawRect(FLinearColor(0.f, 0.18f, 0.35f, 0.85f), 0.f, BarY, W, kEngageBannerHeightPx);
	DrawRect(FLinearColor(0.3f, 0.7f, 1.f, 1.f), 0.f, BarY, W, 3.f);
	DrawRect(FLinearColor(0.3f, 0.7f, 1.f, 1.f), 0.f, BarY + kEngageBannerHeightPx - 3.f, W, 3.f);

	float TitleW = 0.f, TitleH = 0.f;
	GetTextSize(kEngageBannerTitle, TitleW, TitleH, GEngine->GetLargeFont(), 1.6f);
	DrawText(kEngageBannerTitle, FLinearColor::White, (W - TitleW) * 0.5f, BarY + 12.f,
		GEngine->GetLargeFont(), 1.6f);

	float DetailW = 0.f, DetailH = 0.f;
	GetTextSize(kEngageBannerDetail, DetailW, DetailH, GEngine->GetMediumFont(), 1.0f);
	DrawText(kEngageBannerDetail, FLinearColor(0.85f, 0.95f, 1.f, 1.f), (W - DetailW) * 0.5f,
		BarY + 12.f + TitleH + 4.f, GEngine->GetMediumFont(), 1.0f);
}
