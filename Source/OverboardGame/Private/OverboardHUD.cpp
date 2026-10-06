#include "OverboardHUD.h"

#include "BoardActor.h"
#include "HudPacketClient.h"
#include "OverboardPlayerController.h"
#include "RideCourseElements.h"
#include "OverboardWire.h"
#include "HAL/PlatformTime.h"
#include "TerrainVerification.g.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "EngineUtils.h"
#include "Logging/LogMacros.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TextureResource.h"
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

	constexpr float kBannerHeightPx = 96.f;
	constexpr float kTerrainTagHeightPx = 34.f;

	// ============================================================================================
	// The shared rider HUD spec, mirrored from tools/hud/hud_spec.json (version 4).
	//
	// The JSON is the single source for the offline renderer (tools/hud/render_hud.py) and this
	// game HUD. The game reads the file at build authoring time by a human, not at run time: the
	// numbers below are a hand copy, because a run-time file read would add a fragile dependency on
	// a path under the project directory for no behaviour the capture needs. Keep the two in step.
	//
	// All sizes are pixels at a 1080-pixel-high frame. The HUD multiplies each by frame height /
	// 1080 (the scale "K" below), so the panel keeps its proportions at any viewport size.
	// ============================================================================================

	// A colour is an sRGB hex value plus an alpha. The existing banner and tag pass sRGB-normalised
	// components straight into FLinearColor, so this does the same (no gamma conversion), which keeps
	// the panel matching those elements and the reference stills.
	FLinearColor HudColour(uint8 R, uint8 G, uint8 B, float A)
	{
		return FLinearColor(R / 255.f, G / 255.f, B / 255.f, A);
	}

	const FLinearColor kColPanel = HudColour(0x0E, 0x14, 0x1B, 0.72f);
	const FLinearColor kColPanelEdge = HudColour(0x5F, 0xC4, 0xCB, 0.35f);
	const FLinearColor kColText = HudColour(0xE8, 0xEE, 0xF2, 1.0f);
	const FLinearColor kColMuted = HudColour(0xA9, 0xBA, 0xC8, 1.0f);
	const FLinearColor kColDrive = HudColour(0x5F, 0xC4, 0xCB, 1.0f); // drive and brake share teal
	const FLinearColor kColTrack = HudColour(0xFF, 0xFF, 0xFF, 0.18f);
	const FLinearColor kColTick = HudColour(0xFF, 0xFF, 0xFF, 0.45f);
	const FLinearColor kColNearLimit = HudColour(0xFF, 0xB2, 0x3F, 1.0f);
	const FLinearColor kColOverLimit = HudColour(0xFF, 0x5A, 0x4A, 1.0f);
	const FLinearColor kColWarn = HudColour(0xFF, 0xB2, 0x3F, 1.0f);
	const FLinearColor kColWarnText = HudColour(0x0E, 0x14, 0x1B, 1.0f);
	const FLinearColor kColOverboard = HudColour(0xFF, 0x5A, 0x4A, 1.0f);
	const FLinearColor kColBatteryOk = HudColour(0xE8, 0xEE, 0xF2, 1.0f);
	const FLinearColor kColBatteryLow = HudColour(0xFF, 0xB2, 0x3F, 1.0f);

	// panel
	constexpr float kPanelMarginX = 48.f, kPanelMarginY = 48.f;
	constexpr float kPanelW = 440.f, kPanelH = 236.f;
	constexpr float kPanelRadius = 18.f;
	constexpr float kPanelEdge = 1.5f;
	constexpr float kPadX = 28.f, kPadY = 24.f;

	// speed
	constexpr float kSpeedNumeralPx = 92.f;
	constexpr float kSpeedUnitPx = 19.f;
	constexpr float kSpeedTracking = 0.18f;
	constexpr float kSpeedSmoothS = 0.2f;

	// torque
	constexpr float kTorqueLabelPx = 16.f;
	constexpr float kTorqueValuePx = 18.f;
	constexpr float kTorqueBarW = 384.f, kTorqueBarH = 12.f;
	constexpr float kTorqueBarGap = 12.f;
	constexpr float kTorqueNearLimitFrac = 0.85f;
	constexpr float kTorqueOverAtFrac = 0.98f;
	constexpr float kTorqueOverCapPx = 3.f;
	constexpr float kTorqueTopPx = 104.f;
	constexpr float kTorqueSmoothS = 0.08f;

	// battery
	constexpr float kBatteryIconW = 34.f, kBatteryIconH = 16.f;
	constexpr float kBatteryValuePx = 18.f;
	constexpr float kBatteryVoltPx = 16.f;
	constexpr float kBatteryLowSoc = 0.2f;
	constexpr float kBatterySmoothS = 0.5f;

	// warning
	constexpr float kChipPx = 17.f;
	constexpr float kChipPadX = 14.f, kChipPadY = 7.f;
	constexpr float kWarnEdgePx = 2.0f;
	constexpr float kWarnEdgeAlpha = 0.9f;
	constexpr float kWarnDimAlpha = 0.6f;
	constexpr float kWarnDimFillAlpha = 0.35f;
	constexpr float kChipTracking = 0.12f;

	// margin: as the authority used rises from warm_from to warn_at, the panel edge warms from teal
	// to amber, so the panel warns before the chip appears.
	constexpr float kMarginWarmFrom = 0.45f;
	constexpr float kMarginWarnAt = 0.70f;
	constexpr float kMarginEdgeAlpha0 = 0.35f, kMarginEdgeAlpha1 = 0.9f;

	// live-source constants (task spec)
	constexpr float kHudWheelRadiusM = 0.146f;       // speed_mph = |wheel_rate| x radius, in mph
	constexpr float kMsToMph = 2.2369363f;
	constexpr float kMotorKt = 0.658f;            // torque = Kt x current
	constexpr float kMotorCurrentLimitA = 90.f;   // torque limit = Kt x 90 A
	constexpr float kHudStaleSeconds = 0.5f;      // a HUD packet older than this hides battery/margin

	const TCHAR* kChipPushback = TEXT("PUSHBACK");
	const TCHAR* kChipOverboard = TEXT("OVERBOARD");
	const TCHAR* kTorqueLabel = TEXT("TORQUE");
	const TCHAR* kSpeedUnit = TEXT("MPH");
	const TCHAR* kTorqueUnit = TEXT("N·m"); // U+00B7 middle dot
	const TCHAR* kMinus = TEXT("-");        // ASCII: the offline engine font may have no U+2212 glyph
}

AOverboardHUD::AOverboardHUD()
{
	PrimaryActorTick.bCanEverTick = false; // DrawHUD is driven by the renderer, not by Tick
}

AOverboardHUD::~AOverboardHUD() = default;

void AOverboardHUD::BeginPlay()
{
	Super::BeginPlay();
	// Start the HUD telemetry listener early, so a battery and margin packet has arrived before the
	// first frame is drawn or captured.
	EnsureHudClientStarted();
}

void AOverboardHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (HudClient.IsValid())
	{
		HudClient->Shutdown();
		HudClient.Reset();
	}
	Super::EndPlay(EndPlayReason);
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

	DrawRiderCues(*Board);
	DrawGamePanel(*Board);

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

// ================================================================================================
// The shared rider HUD panel. docs/hud-spec.md and tools/hud/hud_spec.json are the spec;
// tools/hud/render_hud.py draw() is the reference geometry and draw order. One panel, bottom left:
// speed, torque against its limit, battery, and the rider-warning chip and panel edge.
//
// Live sources (task spec):
//  - speed: board newest raw sample, |WheelRateRadS| x 0.146 m, in mph;
//  - torque and its limit: Kt x current, Kt x 90 A, from the board's motor current;
//  - battery charge, pack voltage and authority margin: the HUD telemetry packet (OBHD);
//  - the warning state: the board's own flags (bit 5 pulsed, bit 6 solid, bit 4 handoff).
// The pulse shares AOverboardPlayerController::WarningPulseOn, so the chip blinks with the rumble.
// ================================================================================================
void AOverboardHUD::DrawRiderCues(const ABoardActor& Board)
{
	EnsureHudClientStarted();

	const float H = Canvas->ClipY;
	const float K = H / 1080.f; // 1080-pixel-high reference -> this viewport

	// --- live values ----------------------------------------------------------------------------
	OverboardWire::FBoardState State;
	const bool bHaveState = Board.GetLatestState(State);
	const float RawSpeedMph = bHaveState ? FMath::Abs(State.WheelRateRadS) * kHudWheelRadiusM * kMsToMph : 0.f;
	const float RawTorqueNm = bHaveState ? kMotorKt * State.MotorCurrentA : 0.f;
	const float TorqueLimitNm = kMotorKt * kMotorCurrentLimitA;

	FHudPacket Packet;
	double PacketArrivalSeconds = 0.0;
	const bool bHavePacket = HudClient.IsValid() && HudClient->GetLatest(Packet, PacketArrivalSeconds);
	const double Now = FPlatformTime::Seconds();
	const bool bPacketFresh = bHavePacket && (Now - PacketArrivalSeconds) <= kHudStaleSeconds;

	// --- flags ----------------------------------------------------------------------------------
	const uint16 Flags = Board.GetLatestFlags();
	const bool bHandoff = Board.IsPhysicsHandoff();
	const bool bDown = bHandoff; // bit 2 alone is |pitch| > 20 deg, reached by a tail-brake stop
	const bool bSolid = (Flags & OverboardWire::EStateFlags::RiderWarningSolid) != 0;
	const bool bPulsed = (Flags & OverboardWire::EStateFlags::RiderWarningPulsed) != 0;
	const bool bPulseOn = AOverboardPlayerController::WarningPulseOn(Now);
	// The chip alpha blinks with the rumble on a pulsed warning (spec: 0.55 to 1), and stays full on
	// a solid warning or the handoff.
	const float ChipAlpha = (bHandoff || bSolid) ? 1.0f : (bPulseOn ? 1.0f : kWarnDimAlpha);

	// --- causal first-order smoothing -----------------------------------------------------------
	float Dt = bHaveSmoothed ? static_cast<float>(Now - LastPanelDrawSeconds) : 0.f;
	Dt = FMath::Clamp(Dt, 0.f, 0.1f);
	LastPanelDrawSeconds = Now;
	auto Smooth = [Dt](float Prev, float Target, float TauS) -> float
	{
		if (TauS <= 0.f || Dt <= 0.f)
		{
			return Target;
		}
		const float A = 1.f - FMath::Exp(-Dt / TauS);
		return Prev + A * (Target - Prev);
	};
	if (!bHaveSmoothed)
	{
		SmoothedSpeedMph = RawSpeedMph;
		SmoothedTorqueNm = RawTorqueNm;
		SmoothedSoc = bPacketFresh ? Packet.BattSoc : 0.f;
		SmoothedVolt = bPacketFresh ? Packet.BattV : 0.f;
		SmoothedMargin = bPacketFresh ? Packet.Margin : 0.f;
		bHaveSmoothed = true;
	}
	else
	{
		SmoothedSpeedMph = Smooth(SmoothedSpeedMph, RawSpeedMph, kSpeedSmoothS);
		SmoothedTorqueNm = Smooth(SmoothedTorqueNm, RawTorqueNm, kTorqueSmoothS);
		if (bPacketFresh)
		{
			SmoothedSoc = Smooth(SmoothedSoc, Packet.BattSoc, kBatterySmoothS);
			SmoothedVolt = Smooth(SmoothedVolt, Packet.BattV, kBatterySmoothS);
			SmoothedMargin = Smooth(SmoothedMargin, Packet.Margin, kBatterySmoothS);
		}
	}

	// dim(): after the handoff every value is muted grey at 0.6 alpha -- the controller no longer rides.
	auto Dim = [bHandoff](const FLinearColor& C) -> FLinearColor
	{
		if (!bHandoff)
		{
			return C;
		}
		return FLinearColor(kColMuted.R, kColMuted.G, kColMuted.B, kColMuted.A * kWarnDimAlpha);
	};

	// --- panel geometry -------------------------------------------------------------------------
	const float Pw = kPanelW * K, Ph = kPanelH * K;
	const float X0 = kPanelMarginX * K;
	const float Y0 = H - kPanelMarginY * K - Ph;
	const float Px = X0 + kPadX * K, Py = Y0 + kPadY * K;
	const float InnerW = Pw - 2.f * kPadX * K;

	// --- panel edge colour: teal, warming with the margin, chip colour on a warning, red on handoff
	FLinearColor EdgeColour = kColPanelEdge;
	float EdgeWidth = FMath::Max(1.f, kPanelEdge * K);
	if (bPacketFresh)
	{
		const float U = FMath::Clamp((SmoothedMargin - kMarginWarmFrom) / (kMarginWarnAt - kMarginWarmFrom), 0.f, 1.f);
		if (U > 0.f)
		{
			const float Alpha = FMath::Lerp(kMarginEdgeAlpha0, kMarginEdgeAlpha1, U);
			EdgeColour = FLinearColor(kColWarn.R, kColWarn.G, kColWarn.B, Alpha);
		}
	}
	if (bHandoff || bSolid || bPulsed)
	{
		const FLinearColor Base = bHandoff ? kColOverboard : kColWarn;
		const float Alpha = kWarnEdgeAlpha * ((bHandoff || bSolid) ? 1.0f : ChipAlpha);
		EdgeColour = FLinearColor(Base.R, Base.G, Base.B, Alpha);
		EdgeWidth = FMath::Max(1.f, kWarnEdgePx * K);
	}

	// panel fill and edge (draw order: fill first, then the edge ring on top)
	FillRoundedRect(X0, Y0, X0 + Pw, Y0 + Ph, kPanelRadius * K, kColPanel);
	StrokeRoundedRect(X0, Y0, X0 + Pw, Y0 + Ph, kPanelRadius * K, EdgeWidth, EdgeColour);

	// --- speed: a large numeral, the unit beside it on the same baseline ------------------------
	{
		const FHudTextFont NumFont = MakeFont(EHudFont::Numeral, kSpeedNumeralPx * K);
		const FString Num = FString::Printf(TEXT("%.0f"), FMath::Max(0.f, SmoothedSpeedMph));
		const float OrgX = Px - 0.04f * kSpeedNumeralPx * K;
		const float OrgY = Py - 0.20f * kSpeedNumeralPx * K;
		DrawHudText(Num, NumFont, OrgX, OrgY, Dim(kColText));
		const float NumRight = OrgX + MeasureTextWidth(Num, NumFont);
		const FHudTextFont UnitFont = MakeFont(EHudFont::Label, kSpeedUnitPx * K);
		const float BaseN = OrgY + FontBaseline(NumFont);
		DrawTrackedText(kSpeedUnit, UnitFont, NumRight + 12.f * K, BaseN - FontBaseline(UnitFont),
			Dim(kColMuted), kSpeedTracking);
	}

	// --- the warning chip, top right of the panel -----------------------------------------------
	const TCHAR* ChipText = bHandoff ? kChipOverboard : ((bSolid || bPulsed) ? kChipPushback : nullptr);
	if (ChipText != nullptr)
	{
		const FLinearColor ChipCol = bHandoff ? kColOverboard : kColWarn;
		const FHudTextFont ChipFont = MakeFont(EHudFont::Label, kChipPx * K);
		const float Tw = MeasureTrackedWidth(ChipText, ChipFont, kChipTracking);
		const float Cw = Tw + 2.f * kChipPadX * K;
		const float Chh = kChipPx * K + 2.f * kChipPadY * K;
		const float Cx1 = X0 + Pw - kPadX * K;
		const float Cy0 = Py + 4.f * K;
		FillRoundedRect(Cx1 - Cw, Cy0, Cx1, Cy0 + Chh, Chh * 0.5f,
			FLinearColor(ChipCol.R, ChipCol.G, ChipCol.B, ChipAlpha));
		DrawTrackedText(ChipText, ChipFont, Cx1 - Cw + kChipPadX * K, Cy0 + kChipPadY * K - 0.12f * kChipPx * K,
			FLinearColor(kColWarnText.R, kColWarnText.G, kColWarnText.B, ChipAlpha), kChipTracking);
	}

	// --- torque: label, "value / limit", then a centred bar with the limits at its ends ---------
	{
		const float Ty = Py + kTorqueTopPx * K;
		const FHudTextFont LabelFontInfo = MakeFont(EHudFont::Label, kTorqueLabelPx * K);
		DrawTrackedText(kTorqueLabel, LabelFontInfo, Px, Ty, Dim(kColMuted), 0.16f);

		const float Tau = SmoothedTorqueNm;
		const float Lim = FMath::Max(1e-6f, TorqueLimitNm);
		const bool bOver = FMath::Abs(Tau) >= Lim * kTorqueOverAtFrac;
		const FHudTextFont ValueFontInfo = MakeFont(EHudFont::Value, kTorqueValuePx * K);
		const FString Sign = (Tau >= 0.5f) ? TEXT("+") : ((Tau <= -0.5f) ? kMinus : TEXT(""));
		const FString VTxt = FString::Printf(TEXT("%s%.0f"), *Sign, FMath::Abs(Tau));
		const FString LTxt = FString::Printf(TEXT(" / %.0f %s"), Lim, kTorqueUnit);
		const float Bw = FMath::Min(kTorqueBarW * K, InnerW);
		const float Bh = kTorqueBarH * K;
		const float Vy = Ty - 0.22f * kTorqueValuePx * K;
		const float Lw = MeasureTextWidth(LTxt, ValueFontInfo);
		const float Vw = MeasureTextWidth(VTxt, ValueFontInfo);
		DrawHudText(LTxt, ValueFontInfo, Px + Bw - Lw, Vy, Dim(kColMuted));
		DrawHudText(VTxt, ValueFontInfo, Px + Bw - Lw - Vw, Vy, Dim(bOver ? kColOverLimit : kColText));

		const float By = Ty + kTorqueLabelPx * K + kTorqueBarGap * K;
		const float Bx0 = Px, Bx1 = Px + Bw, Bc = Px + Bw * 0.5f;
		FillRoundedRect(Bx0, By, Bx1, By + Bh, Bh * 0.5f, kColTrack);

		const float Frac = FMath::Clamp(Tau / Lim, -1.f, 1.f);
		FLinearColor FillCol;
		if (bHandoff)
		{
			FillCol = FLinearColor(kColMuted.R, kColMuted.G, kColMuted.B, kWarnDimFillAlpha);
		}
		else if (bOver)
		{
			FillCol = kColOverLimit;
		}
		else if (FMath::Abs(Frac) >= kTorqueNearLimitFrac)
		{
			FillCol = kColNearLimit;
		}
		else
		{
			FillCol = kColDrive;
		}
		if (FMath::Abs(Frac) > 1e-3f)
		{
			float A = Bc, B = Bc + Frac * Bw * 0.5f;
			if (A > B)
			{
				Swap(A, B);
			}
			FillRoundedRect(A, By, FMath::Max(B, A + Bh), By + Bh, Bh * 0.5f, FillCol);
		}

		// the two limits and zero
		const float TickXs[3] = { Bx0, Bc, Bx1 };
		for (float Xt : TickXs)
		{
			DrawRect(kColTick, Xt - 0.75f * K, By - 4.f * K, 1.5f * K, Bh + 8.f * K);
		}

		// a red cap past the end the request went beyond
		if (bOver && !bHandoff)
		{
			const float D = kTorqueOverCapPx * K;
			if (Tau > 0.f)
			{
				DrawRect(kColOverLimit, Bx1 + 2.f * K, By - 5.f * K, D, Bh + 10.f * K);
			}
			else
			{
				DrawRect(kColOverLimit, Bx0 - 2.f * K - D, By - 5.f * K, D, Bh + 10.f * K);
			}
		}
	}

	// --- battery: icon filled to the charge, the charge, the pack voltage ------------------------
	// Hidden when the HUD packet is stale or was never received: a missing source, per the spec.
	if (bPacketFresh)
	{
		const float Iw = kBatteryIconW * K, Ih = kBatteryIconH * K;
		const float Yb = Y0 + Ph - kPadY * K - Ih;
		const float Soc = FMath::Clamp(SmoothedSoc, 0.f, 1.f);
		const FLinearColor BattCol = (Soc < kBatteryLowSoc) ? kColBatteryLow : kColBatteryOk;
		const FLinearColor Border = Dim(kColMuted);

		// icon outline (four thin rects) plus the terminal nub
		const float Bw = FMath::Max(1.f, 1.5f * K);
		DrawRect(Border, Px, Yb, Iw, Bw);                     // top
		DrawRect(Border, Px, Yb + Ih - Bw, Iw, Bw);           // bottom
		DrawRect(Border, Px, Yb, Bw, Ih);                     // left
		DrawRect(Border, Px + Iw - Bw, Yb, Bw, Ih);           // right
		DrawRect(Border, Px + Iw, Yb + Ih * 0.3f, 3.f * K, Ih * 0.4f);
		const float Inset = 3.f * K;
		DrawRect(Dim(BattCol), Px + Inset, Yb + Inset, (Iw - 2.f * Inset) * Soc, Ih - 2.f * Inset);

		const FHudTextFont ValueFontInfo = MakeFont(EHudFont::Value, kBatteryValuePx * K);
		const FString Txt = FString::Printf(TEXT("%.1f%%"), 100.f * Soc);
		const float Tx = Px + Iw + 16.f * K;
		DrawHudText(Txt, ValueFontInfo, Tx, Yb + Ih * 0.5f - 0.62f * kBatteryValuePx * K, Dim(kColText));
		const FHudTextFont VoltFontInfo = MakeFont(EHudFont::Value, kBatteryVoltPx * K);
		const float Tx2 = Tx + MeasureTextWidth(Txt, ValueFontInfo) + 14.f * K;
		DrawHudText(FString::Printf(TEXT("%.1f V"), SmoothedVolt), VoltFontInfo,
			Tx2, Yb + Ih * 0.5f - 0.60f * kBatteryVoltPx * K, Dim(kColMuted));
	}

	// The centre reset prompt stays: it is a game cue, not part of the offline HUD. It draws over
	// the panel when the board is down or handed off.
	if (bDown)
	{
		DrawFallenPrompt();
	}
}

void AOverboardHUD::DrawFallenPrompt()
{
	const float W = Canvas->ClipX;
	const float H = Canvas->ClipY;
	const float K = H / 1080.f;

	ABoardActor* Board = FindBoard();
	const bool bSteppedOff = Board && Board->IsSteppedOff();
	const FString Title = bSteppedOff ? TEXT("STEPPED OFF") : TEXT("FALLEN");
	const FString Detail = bSteppedOff ? TEXT("Press Circle (or R) to ride again") : TEXT("Press Circle (or R) to reset");
	const FHudTextFont TitleFont = MakeFont(EHudFont::Label, 44.f * K);
	const FHudTextFont DetailFont = MakeFont(EHudFont::Value, 20.f * K);

	const float TitleW = MeasureTrackedWidth(Title, TitleFont, 0.12f);
	const float DetailW = MeasureTextWidth(Detail, DetailFont);
	const float BoxW = FMath::Max(TitleW, DetailW) + 72.f * K;
	const float BoxH = 44.f * K + 20.f * K + 44.f * K;
	const float BoxX = (W - BoxW) * 0.5f;
	const float BoxY = H * 0.38f;

	FillRoundedRect(BoxX, BoxY, BoxX + BoxW, BoxY + BoxH, kPanelRadius * K, kColPanel);
	StrokeRoundedRect(BoxX, BoxY, BoxX + BoxW, BoxY + BoxH, kPanelRadius * K, FMath::Max(1.f, kWarnEdgePx * K), kColOverLimit);

	DrawTrackedText(Title, TitleFont, (W - TitleW) * 0.5f, BoxY + 26.f * K, kColOverLimit, 0.12f);
	DrawHudText(Detail, DetailFont, (W - DetailW) * 0.5f, BoxY + 26.f * K + 44.f * K + 6.f * K, kColText);
}

// The game layer (ARideCourseElements): a small top-centre panel with the run time and score,
// the zone the board is in, and the newest event. Before the board is armed it shows how to
// start. It reads the elements actor's readout only; nothing here touches the board.
void AOverboardHUD::DrawGamePanel(const ABoardActor& Board)
{
	const ARideCourseElements* Game = nullptr;
	for (TActorIterator<ARideCourseElements> It(GetWorld()); It; ++It)
	{
		Game = *It;
		break;
	}
	if (!Game || Board.IsPhysicsHandoff())
	{
		return; // no game on this level, or the fall prompt owns the centre of the screen
	}
	const FRideGameReadout& R = Game->GetReadout();

	const float W = Canvas->ClipX;
	const float K = Canvas->ClipY / 1080.f;
	const FHudTextFont Big = MakeFont(EHudFont::Value, 30.f * K);
	const FHudTextFont Small = MakeFont(EHudFont::Label, 15.f * K);

	const AOverboardPlayerController* PC = Cast<AOverboardPlayerController>(GetOwningPlayerController());

	// Cruise mode (embarcadero): no score panel, as a free ride has nothing to score. Only the
	// arm hint shows, until the first arm.
	if (R.bCruise)
	{
		if (PC && !PC->HasArmedOnce())
		{
			const FString Hint = TEXT("Press Cross (or Space) to arm");
			const float HintW = MeasureTextWidth(Hint, Big);
			const float BoxW = HintW + 64.f * K;
			const float BoxH = 64.f * K;
			const float BoxX = (W - BoxW) * 0.5f;
			const float BoxY = 28.f * K;
			FillRoundedRect(BoxX, BoxY, BoxX + BoxW, BoxY + BoxH, kPanelRadius * K, kColPanel);
			DrawHudText(Hint, Big, (W - HintW) * 0.5f, BoxY + 16.f * K, kColText);
		}
		return;
	}

	// Laps mode (Level 1): the lap number and time, then the last/best/target times, the next
	// checkpoint, and the missed list. Toasts share the bottom of the panel.
	if (R.bLaps)
	{
		auto Mmsss = [](double S) -> FString
		{
			const int32 T = FMath::FloorToInt(FMath::Max(S, 0.0) * 10.0);
			return FString::Printf(TEXT("%d:%02d.%d"), T / 600, (T / 10) % 60, T % 10);
		};
		FString Head;
		if (PC && !PC->HasArmedOnce())
		{
			Head = TEXT("Press Cross (or Space) to arm");
		}
		else if (R.LapNumber == 0)
		{
			Head = TEXT("Ride through START / FINISH");
		}
		else
		{
			Head = FString::Printf(TEXT("LAP %d   %s   %d PTS"), R.LapNumber, *Mmsss(R.LapTimeSeconds), R.Score);
		}
		const float HeadW = MeasureTextWidth(Head, Big);
		const float BoxW = FMath::Max(HeadW + 64.f * K, 360.f * K);
		const float BoxH = 64.f * K;
		const float BoxX = (W - BoxW) * 0.5f;
		const float BoxY = 28.f * K;
		FillRoundedRect(BoxX, BoxY, BoxX + BoxW, BoxY + BoxH, kPanelRadius * K, kColPanel);
		DrawHudText(Head, Big, (W - HeadW) * 0.5f, BoxY + 16.f * K, kColText);

		float Y = BoxY + BoxH + 10.f * K;
		const FString Best = R.BestCleanSeconds > 0.0 ? Mmsss(R.BestCleanSeconds) : FString(TEXT("--:--.-"));
		const FString Times = FString::Printf(TEXT("LAST %s    BEST %s    TARGET %s"),
			*Mmsss(R.LastLapSeconds), *Best, *Mmsss(R.TargetLapSeconds));
		const float TimesW = MeasureTrackedWidth(Times, Small, 0.12f);
		DrawTrackedText(Times, Small, (W - TimesW) * 0.5f, Y, kColText, 0.12f);
		Y += 24.f * K;
		if (!R.NextCheckpointLabel.IsEmpty())
		{
			const FString Next = FString::Printf(TEXT("NEXT: %s"), *R.NextCheckpointLabel);
			const float NextW = MeasureTrackedWidth(Next, Small, 0.12f);
			DrawTrackedText(Next, Small, (W - NextW) * 0.5f, Y, kColWarn, 0.12f);
			Y += 24.f * K;
		}
		if (!R.MissedList.IsEmpty())
		{
			const FString Miss = FString::Printf(TEXT("MISSED: %s"), *R.MissedList.ToUpper());
			const float MissW = MeasureTrackedWidth(Miss, Small, 0.12f);
			DrawTrackedText(Miss, Small, (W - MissW) * 0.5f, Y, kColOverLimit, 0.12f);
			Y += 24.f * K;
		}
		if (!R.Toast.IsEmpty())
		{
			const float Fade = FMath::Clamp(2.5f - static_cast<float>(R.ToastAgeSeconds), 0.f, 1.f);
			FLinearColor C = kColText;
			C.A *= Fade;
			const float TW = MeasureTextWidth(R.Toast, Big);
			DrawHudText(R.Toast, Big, (W - TW) * 0.5f, Y, C);
		}
		return;
	}

	FString Line;
	if (PC && !PC->HasArmedOnce())
	{
		Line = TEXT("Press Cross (or Space) to arm");
	}
	else if (!R.bRunActive && !R.bFinished)
	{
		Line = TEXT("Ride through START");
	}
	else
	{
		const int32 Tenths = FMath::FloorToInt(R.ElapsedSeconds * 10.0);
		Line = FString::Printf(TEXT("%d:%02d.%d    %d PTS"), Tenths / 600, (Tenths / 10) % 60, Tenths % 10, R.Score);
	}

	const float LineW = MeasureTextWidth(Line, Big);
	const float BoxW = FMath::Max(LineW + 64.f * K, 320.f * K);
	const float BoxH = 64.f * K;
	const float BoxX = (W - BoxW) * 0.5f;
	const float BoxY = 28.f * K;
	FillRoundedRect(BoxX, BoxY, BoxX + BoxW, BoxY + BoxH, kPanelRadius * K, kColPanel);
	DrawHudText(Line, Big, (W - LineW) * 0.5f, BoxY + 16.f * K, R.bFinished ? kColOverLimit : kColText);

	float Y = BoxY + BoxH + 10.f * K;
	if (!R.ZoneLabel.IsEmpty())
	{
		const float ZW = MeasureTrackedWidth(R.ZoneLabel, Small, 0.12f);
		DrawTrackedText(R.ZoneLabel, Small, (W - ZW) * 0.5f, Y, kColWarn, 0.12f);
		Y += 24.f * K;
	}
	if (!R.Toast.IsEmpty())
	{
		const float Fade = FMath::Clamp(2.5f - static_cast<float>(R.ToastAgeSeconds), 0.f, 1.f);
		FLinearColor C = kColText;
		C.A *= Fade;
		const float TW = MeasureTextWidth(R.Toast, Big);
		DrawHudText(R.Toast, Big, (W - TW) * 0.5f, Y, C);
	}
}

// ------------------------------------------------------------------------------------------------
// Fonts, canvas primitives and text. See the header for what each does.
// ------------------------------------------------------------------------------------------------

void AOverboardHUD::EnsureHudClientStarted()
{
	if (!HudClient.IsValid())
	{
		HudClient = MakeUnique<FHudPacketClient>();
		HudClient->StartListening();
	}
}

AOverboardHUD::FHudTextFont AOverboardHUD::MakeFont(EHudFont Role, float SizePx) const
{
	// The numeral is the only truly large element, so it uses the large offline font; the labels
	// and values use the medium one. Each is scaled so its glyph height matches the spec pixel size.
	const UFont* Font = (Role == EHudFont::Numeral)
		? (GEngine ? GEngine->GetLargeFont() : nullptr)
		: (GEngine ? GEngine->GetMediumFont() : nullptr);
	FHudTextFont Out;
	Out.Font = Font;
	Out.SizePx = FMath::Max(1.f, SizePx);
	const float NativeHeight = Font ? FMath::Max(1.f, static_cast<float>(Font->GetMaxCharHeight())) : 1.f;
	Out.Scale = Out.SizePx / NativeHeight;
	return Out;
}

float AOverboardHUD::FontBaseline(const FHudTextFont& Font) const
{
	// Roboto's ascent is close to this fraction of the glyph height.
	return Font.SizePx * 0.80f;
}

float AOverboardHUD::MeasureTextWidth(const FString& Text, const FHudTextFont& Font) const
{
	if (!Font.Font)
	{
		return 0.f;
	}
	float W = 0.f, H = 0.f;
	GetTextSize(Text, W, H, const_cast<UFont*>(Font.Font), Font.Scale);
	return W;
}

float AOverboardHUD::MeasureTrackedWidth(const FString& Text, const FHudTextFont& Font, float Tracking) const
{
	float Width = 0.f;
	for (int32 i = 0; i < Text.Len(); ++i)
	{
		Width += MeasureTextWidth(Text.Mid(i, 1), Font);
	}
	if (Text.Len() > 1)
	{
		Width += Tracking * Font.SizePx * (Text.Len() - 1);
	}
	return Width;
}

void AOverboardHUD::DrawHudText(const FString& Text, const FHudTextFont& Font, float X, float Y, const FLinearColor& Color)
{
	if (!Canvas || Text.IsEmpty() || !Font.Font)
	{
		return;
	}
	DrawText(Text, Color, X, Y, const_cast<UFont*>(Font.Font), Font.Scale);
}

void AOverboardHUD::DrawTrackedText(const FString& Text, const FHudTextFont& Font, float X, float Y, const FLinearColor& Color, float Tracking)
{
	float Cursor = X;
	for (int32 i = 0; i < Text.Len(); ++i)
	{
		const FString Ch = Text.Mid(i, 1);
		DrawHudText(Ch, Font, Cursor, Y, Color);
		Cursor += MeasureTextWidth(Ch, Font) + Tracking * Font.SizePx;
	}
}

void AOverboardHUD::FillArc(float Cx, float Cy, float Radius, float A0Rad, float A1Rad, const FLinearColor& Color)
{
	if (!Canvas || Radius <= 0.f)
	{
		return;
	}
	constexpr int32 N = 8;
	TArray<FCanvasUVTri> Tris;
	Tris.Reserve(N);
	float PrevX = Cx + Radius * FMath::Cos(A0Rad);
	float PrevY = Cy + Radius * FMath::Sin(A0Rad);
	for (int32 i = 1; i <= N; ++i)
	{
		const float A = FMath::Lerp(A0Rad, A1Rad, static_cast<float>(i) / N);
		const float Xn = Cx + Radius * FMath::Cos(A);
		const float Yn = Cy + Radius * FMath::Sin(A);
		FCanvasUVTri Tri;
		Tri.V0_Pos = FVector2D(Cx, Cy);
		Tri.V1_Pos = FVector2D(PrevX, PrevY);
		Tri.V2_Pos = FVector2D(Xn, Yn);
		Tri.V0_Color = Tri.V1_Color = Tri.V2_Color = Color;
		Tri.V0_UV = Tri.V1_UV = Tri.V2_UV = FVector2D::ZeroVector;
		Tris.Add(Tri);
		PrevX = Xn;
		PrevY = Yn;
	}
	FCanvasTriangleItem Item(Tris, GWhiteTexture);
	Item.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Item);
}

void AOverboardHUD::StrokeArc(float Cx, float Cy, float OuterRadius, float Width, float A0Rad, float A1Rad, const FLinearColor& Color)
{
	if (!Canvas || OuterRadius <= 0.f || Width <= 0.f)
	{
		return;
	}
	const float Inner = FMath::Max(0.f, OuterRadius - Width);
	constexpr int32 N = 8;
	TArray<FCanvasUVTri> Tris;
	Tris.Reserve(N * 2);
	for (int32 i = 0; i < N; ++i)
	{
		const float Aa = FMath::Lerp(A0Rad, A1Rad, static_cast<float>(i) / N);
		const float Ab = FMath::Lerp(A0Rad, A1Rad, static_cast<float>(i + 1) / N);
		const FVector2D Oa(Cx + OuterRadius * FMath::Cos(Aa), Cy + OuterRadius * FMath::Sin(Aa));
		const FVector2D Ob(Cx + OuterRadius * FMath::Cos(Ab), Cy + OuterRadius * FMath::Sin(Ab));
		const FVector2D Ia(Cx + Inner * FMath::Cos(Aa), Cy + Inner * FMath::Sin(Aa));
		const FVector2D Ib(Cx + Inner * FMath::Cos(Ab), Cy + Inner * FMath::Sin(Ab));
		FCanvasUVTri T0;
		T0.V0_Pos = Ia; T0.V1_Pos = Oa; T0.V2_Pos = Ob;
		FCanvasUVTri T1;
		T1.V0_Pos = Ia; T1.V1_Pos = Ob; T1.V2_Pos = Ib;
		T0.V0_Color = T0.V1_Color = T0.V2_Color = Color;
		T1.V0_Color = T1.V1_Color = T1.V2_Color = Color;
		T0.V0_UV = T0.V1_UV = T0.V2_UV = FVector2D::ZeroVector;
		T1.V0_UV = T1.V1_UV = T1.V2_UV = FVector2D::ZeroVector;
		Tris.Add(T0);
		Tris.Add(T1);
	}
	FCanvasTriangleItem Item(Tris, GWhiteTexture);
	Item.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Item);
}

void AOverboardHUD::FillRoundedRect(float X0, float Y0, float X1, float Y1, float Radius, const FLinearColor& Color)
{
	if (!Canvas)
	{
		return;
	}
	const float R = FMath::Clamp(Radius, 0.f, FMath::Min((X1 - X0) * 0.5f, (Y1 - Y0) * 0.5f));
	if (R < 0.5f)
	{
		DrawRect(Color, X0, Y0, X1 - X0, Y1 - Y0);
		return;
	}
	// a middle column, two side rects and four corner fans tile the rounded rect without overlap,
	// so a translucent colour blends once everywhere.
	DrawRect(Color, X0 + R, Y0, (X1 - X0) - 2.f * R, Y1 - Y0);
	DrawRect(Color, X0, Y0 + R, R, (Y1 - Y0) - 2.f * R);
	DrawRect(Color, X1 - R, Y0 + R, R, (Y1 - Y0) - 2.f * R);
	FillArc(X0 + R, Y0 + R, R, PI, 1.5f * PI, Color);     // top-left
	FillArc(X1 - R, Y0 + R, R, 1.5f * PI, 2.f * PI, Color); // top-right
	FillArc(X1 - R, Y1 - R, R, 0.f, 0.5f * PI, Color);     // bottom-right
	FillArc(X0 + R, Y1 - R, R, 0.5f * PI, PI, Color);      // bottom-left
}

void AOverboardHUD::StrokeRoundedRect(float X0, float Y0, float X1, float Y1, float Radius, float Width, const FLinearColor& Color)
{
	if (!Canvas || Width <= 0.f)
	{
		return;
	}
	const float R = FMath::Clamp(Radius, 0.f, FMath::Min((X1 - X0) * 0.5f, (Y1 - Y0) * 0.5f));
	// the four straight sides
	DrawRect(Color, X0 + R, Y0, (X1 - X0) - 2.f * R, Width);             // top
	DrawRect(Color, X0 + R, Y1 - Width, (X1 - X0) - 2.f * R, Width);     // bottom
	DrawRect(Color, X0, Y0 + R, Width, (Y1 - Y0) - 2.f * R);            // left
	DrawRect(Color, X1 - Width, Y0 + R, Width, (Y1 - Y0) - 2.f * R);     // right
	if (R >= 0.5f)
	{
		StrokeArc(X0 + R, Y0 + R, R, Width, PI, 1.5f * PI, Color);       // top-left
		StrokeArc(X1 - R, Y0 + R, R, Width, 1.5f * PI, 2.f * PI, Color); // top-right
		StrokeArc(X1 - R, Y1 - R, R, Width, 0.f, 0.5f * PI, Color);      // bottom-right
		StrokeArc(X0 + R, Y1 - R, R, Width, 0.5f * PI, PI, Color);       // bottom-left
	}
}
