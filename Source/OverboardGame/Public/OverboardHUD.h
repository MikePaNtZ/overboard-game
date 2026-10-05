// OverboardHUD.h
//
// The in-game surfacing of ADR-0011's loss-of-authority warning -- condition 3 of the second
// ratification, and overboard-game#19.
//
// ADR-0011 moved two exit criteria onto the hardware gate and stated that the move is honest only
// under three conditions. The third is:
//
//     "The loss-of-authority warning ships as the in-game surfacing of the cliff.
//      2.868 s of lead is adequate."
//
// overboard#205 landed the SIGNAL. This is the half a player can see.
//
// ============================================================================================
// WHY A NATIVE AHUD AND NOT A UMG WIDGET
// ============================================================================================
//
// Same reasoning as every other piece of scene setup in this project: no .uasset, no editor GUI
// step, no Blueprint. `AOverboardGameMode` spawns its ground, markers, board and camera in C++
// so the scene exists regardless of what is authored in the level; a warning banner that
// depended on a hand-authored widget asset would be the one piece of the safety annunciation
// that could silently go missing -- and this repo has already been burned twice by exactly that
// class of failure (the null flat material in #12, the class-resolution fallbacks in #18).
//
// ============================================================================================
// TWO CONSTRAINTS FROM THE ADR THAT SHAPE THE TREATMENT
// ============================================================================================
//
// **1. The lead must survive the client.** ADR-0011 measures the warning at 3.000 s against
// `FALLEN` at 5.868 s -- 2.868 s of lead, where `FALLEN` itself TRAILS saturation by -0.948 s
// and so annunciates a fall already decided. Issue #19 is explicit that "the lead time must
// survive whatever filtering or debouncing the client adds". So:
//
//   - the rising edge has ZERO debounce. The banner is drawn on the first frame the bit is seen.
//   - the newest RAW sample is used, not the render-delayed interpolated pose -- the same call
//     `ABoardActor::IsFallen()` already makes, and for the same reason.
//   - all hysteresis is on the CLEARING edge (`kMinimumHoldSeconds`), where it cannot cost lead.
//
// **2. It must not become a behavioural claim.** Under the `Playable Sim` provenance rules a
// game run may state facts about the machinery and never a result. So the banner says what the
// machine is doing -- commanded current at the envelope limit, below speed-cap onset -- and
// never predicts an outcome. "The board is about to flip" would be a result, and would be a
// false one on any run that recovers.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "HudPacketClient.h" // the TUniquePtr<FHudPacketClient> member needs the complete type here
#include "OverboardHUD.generated.h"

class ABoardActor;
class UFont;

UCLASS()
class OVERBOARDGAME_API AOverboardHUD : public AHUD
{
	GENERATED_BODY()

public:
	AOverboardHUD();
	// Defined in the .cpp, where FHudPacketClient is a complete type, so the forward-declared
	// TUniquePtr member below can be destroyed.
	virtual ~AOverboardHUD() override;

	virtual void BeginPlay() override;
	virtual void DrawHUD() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

protected:
	/// Minimum time the banner stays up once raised, seconds. Hysteresis on the CLEARING edge
	/// only. The signal is a filtered utilisation crossing a threshold, so it can dither across
	/// it for a frame or two near the boundary; without a hold that reads as a flicker, and a
	/// flickering warning is one a player learns to ignore. Deliberately NOT applied to the
	/// rising edge, where it would come straight out of the 2.868 s of lead the ADR is relying
	/// on.
	UPROPERTY(EditDefaultsOnly, Category = "HUD|Authority")
	float MinimumHoldSeconds = 0.75f;

	/// Pulse rate of the banner, Hz. Motion is what makes a peripheral element get noticed; a
	/// static red bar is easy to stop seeing.
	UPROPERTY(EditDefaultsOnly, Category = "HUD|Authority")
	float PulseHz = 3.0f;

private:
	ABoardActor* FindBoard() const;

	void DrawAuthorityBanner(float Alpha);
	void DrawTerrainTag();

	// The shared rider HUD panel (docs/hud-spec.md, tools/hud/hud_spec.json). One panel, bottom
	// left: speed, torque against its limit, battery, and the rider-warning chip and panel edge.
	// Replaces the two interim cues that lived here before the spec landed.
	void DrawRiderCues(const ABoardActor& Board);

	// The centre "FALLEN" prompt, drawn over the panel when the board is down or handed off.
	void DrawFallenPrompt();
	void DrawGamePanel(const ABoardActor& Board);

	// --- Fonts ----------------------------------------------------------------------------------
	// The spec asks for Roboto Light/Medium/Regular from Engine/Content/Slate/Fonts. A Slate
	// FSlateFontInfo drawn through FCanvasTextItem renders nothing on this HUD canvas in
	// -game -RenderOffscreen (the Slate glyph atlas is not resident for the canvas there), so the
	// HUD uses the engine's offline Roboto UFonts (GEngine->Get*Font()) scaled to the spec pixel
	// size instead. The weight is approximate -- the engine ships only one offline weight per size
	// class -- but the layout and sizes follow hud_spec.json. See the note in OverboardHUD.cpp.
	enum class EHudFont : uint8 { Numeral, Label, Value };
	struct FHudTextFont
	{
		const UFont* Font = nullptr;
		float Scale = 1.f;   // UFont draw scale that makes the glyph height match SizePx
		float SizePx = 1.f;  // the spec pixel size, for letter tracking
	};
	FHudTextFont MakeFont(EHudFont Role, float SizePx) const;

	// --- Canvas primitives ----------------------------------------------------------------------
	// A Canvas AHUD has no rounded-rectangle primitive, so these build one from filled rectangles
	// (AHUD::DrawRect) and triangle fans (FCanvasTriangleItem). The spec draws nothing but rounded
	// rectangles, rectangles and text.
	void FillRoundedRect(float X0, float Y0, float X1, float Y1, float Radius, const FLinearColor& Color);
	void StrokeRoundedRect(float X0, float Y0, float X1, float Y1, float Radius, float Width, const FLinearColor& Color);
	void FillArc(float Cx, float Cy, float Radius, float A0Rad, float A1Rad, const FLinearColor& Color);
	void StrokeArc(float Cx, float Cy, float OuterRadius, float Width, float A0Rad, float A1Rad, const FLinearColor& Color);

	// --- Text -----------------------------------------------------------------------------------
	void DrawHudText(const FString& Text, const FHudTextFont& Font, float X, float Y, const FLinearColor& Color);
	// Draws each character with extra spacing (a fraction of the font size), for the small
	// letter-spaced labels the spec uses (unit, chip, torque label).
	void DrawTrackedText(const FString& Text, const FHudTextFont& Font, float X, float Y, const FLinearColor& Color, float Tracking);
	float MeasureTextWidth(const FString& Text, const FHudTextFont& Font) const;
	float MeasureTrackedWidth(const FString& Text, const FHudTextFont& Font, float Tracking) const;
	float FontBaseline(const FHudTextFont& Font) const;

	// --- HUD telemetry packet (battery and margin) ----------------------------------------------
	void EnsureHudClientStarted();
	TUniquePtr<FHudPacketClient> HudClient;

	// --- Causal first-order smoothing (hud_spec.json smooth_s per row) --------------------------
	// A live HUD does what the offline renderer does: an exponential filter per value, so the
	// numbers do not jitter frame to frame.
	bool bHaveSmoothed = false;
	double LastPanelDrawSeconds = 0.0;
	float SmoothedSpeedMph = 0.f;
	float SmoothedTorqueNm = 0.f;
	float SmoothedSoc = 0.f;
	float SmoothedVolt = 0.f;
	float SmoothedMargin = 0.f;

	/// Resolves this level's name and looks it up in the table generated from the terrain
	/// declarations. When the level's drivable surface has not been measured against the
	/// ADR-0011 authored-world envelope, the HUD says so, permanently and on screen -- the
	/// third of the three things that close the `kind external` hole in `terraincheck` (see
	/// terrain/README.md): the parser refuses to let unmeasured geometry call itself verified,
	/// `--strict` fails on it, and this makes it impossible to shoot footage on such a level
	/// without the tag being in frame.
	///
	/// Done lazily on the first DrawHUD rather than pushed in from AOverboardGameMode::BeginPlay,
	/// because the HUD is spawned by the PlayerController and the two orderings are not
	/// guaranteed. A tag that depended on winning that race would go missing exactly as silently
	/// as the null flat material in #12 did.
	void ResolveTerrainVerificationOnce();

	bool bTerrainResolved = false;

	/// True while the banner is being drawn -- includes the post-clear hold.
	bool bBannerVisible = false;

	/// True on the previous frame's raw signal, for edge detection.
	bool bWarningLastFrame = false;

	/// `FPlatformTime::Seconds()` at which the banner may next be taken down.
	double BannerHoldUntilSeconds = 0.0;

	/// Set once per rising edge, so the receipt-to-pixel latency is logged exactly once per
	/// event rather than every frame. See DrawHUD for what is measured and why it is measured
	/// here rather than in the actor.
	bool bLoggedThisEvent = false;

	bool bTerrainUnverified = false;
	FString TerrainLevelName;
};
