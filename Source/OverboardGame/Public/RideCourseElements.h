// RideCourseElements -- the game layer on a MuJoCo course: gates with a timer, slalom flags, a
// speed trap, a stop box, a slow zone and a no-buzz climb, with a score.
//
// READ-ONLY by rule (ADR-0009 boundary): this actor reads the board's newest StateOut sample
// (position, speed, pitch, flags) and scores the ride. It puts no force on the board and has no
// collision. Obstacles the board can hit come later as MuJoCo objects, not from here.
//
// The layout comes from tools/play/elements/<course>.json (tools/play/gen_elements.py), in course
// coordinates: s = distance along the street (MuJoCo x = start_x - s) and MuJoCo lateral y.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RideCourseElements.generated.h"

class ABoardActor;
class UMaterialInterface;
class UStaticMesh;

// What the HUD shows for the game layer.
struct FRideGameReadout
{
	bool bRunActive = false;   // between the START gate and the FINISH gate (or a fall)
	bool bFinished = false;
	double ElapsedSeconds = 0.0;
	int32 Score = 0;
	FString ZoneLabel;         // the zone the board is in now, or empty
	FString Toast;             // the newest event ("FLAG +100", "TAIL STOP +500", ...)
	double ToastAgeSeconds = 1e9;
};

UCLASS()
class OVERBOARDGAME_API ARideCourseElements : public AActor
{
	GENERATED_BODY()

public:
	ARideCourseElements();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// Loads tools/play/elements/<CourseName>.json. False if the file is missing or bad.
	bool LoadLayout(const FString& CourseName);

	const FRideGameReadout& GetReadout() const { return Readout; }

private:
	enum class EKind : uint8 { Gate, Flag, StopBox, SlowZone, NoBuzz, SpeedTrap, Cone, Debris, Unknown };

	struct FElement
	{
		EKind Kind = EKind::Gate;
		FString Id;
		FString Label;
		double S = 0.0, S0 = 0.0, S1 = 0.0, Y = 0.0, HalfWidth = 0.0;
		double StopSpeed = 0.3, TailPitchRad = 0.25, MaxSpeed = 3.0, BonusSpeed = 7.0;
		double PeakSpeed = 0.0;
		FVector SizeM = FVector(0.8, 0.5, 0.12); // debris box
		double YawDeg = 0.0;
		// Run state
		bool bDone = false;       // scored or missed for this run
		bool bFailed = false;     // the zone rule was broken in this run
		bool bTailSeen = false;
	};

	TArray<FElement> Elements;
	TArray<double> ProfileS, ProfileZ;
	double StartX = 90.0;
	double LaneHalfWidth = 4.5;
	double StreetHalfWidth = 6.0;
	TMap<FString, int32> Scores;

	FRideGameReadout Readout;
	double RunStartSeconds = 0.0;
	double LastS = -1e9;
	bool bVisualsBuilt = false;
	bool bLoggedFrame = false;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> CubeMesh;
	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> CylinderMesh;
	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> ConeMesh;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> BaseMaterial;

	TWeakObjectPtr<ABoardActor> Board;

	double HeightAt(double S) const;
	FVector ToWorld(double S, double Y, double UpM) const;
	void BuildVisuals();
	void AddBox(const FVector& CentreWorld, const FVector& SizeM, const FLinearColor& Color, float YawDeg = 0.f);
	void AddPole(const FVector& BaseWorld, double HeightM, double RadiusM, const FLinearColor& Color);
	void AddLabel(const FVector& World, const FString& Text, const FLinearColor& Color, float SizeCm, float YawDeg);
	void ResetRun();
	void Award(const FString& Key, const FString& Text, int32 Multiplier = 1);
	void Event(const FString& Text);
};
