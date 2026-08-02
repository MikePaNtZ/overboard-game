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
#include "OverboardHUD.generated.h"

class ABoardActor;

UCLASS()
class OVERBOARDGAME_API AOverboardHUD : public AHUD
{
	GENERATED_BODY()

public:
	AOverboardHUD();

	virtual void DrawHUD() override;

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
