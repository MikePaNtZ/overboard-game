// DemoRider -- a scripted PLAYER for demo videos (-ObDemoRider on the command line).
//
// It stands in for the person holding the pad: each frame it reads the newest board state (the
// same numbers a player reads off the screen: position on the street, speed, heading) and the
// game readout, and it sets the pad values -- lean, carve, L2, Cross, Circle, and the fall-test
// kick. AOverboardPlayerController then shapes and sends them exactly as it does for a real pad.
// It computes no board physics and puts no force on the board: sim-host still does all of that.
//
// The ride (city_hill): arm, ride the descent at a steady speed while weaving the slalom flags,
// tail-brake to a stop in the stop box, cross the slow zone under 3 m/s, climb, finish, then
// one fall-test kick and a reset.

#pragma once

#include "CoreMinimal.h"
#include "OverboardWire.h"

struct FRideGameReadout;

struct FDemoPadOutput
{
	float Lean = 0.f;      // stick-equivalent, [-1, 1], BEFORE the controller's shaping
	float Steer = 0.f;     // [-1, 1], positive = turn right
	float TailBrake = 0.f; // L2, [0, 1]
	bool bArm = false;
	bool bReset = false;
	bool bKick = false;    // InputIn bit 2, a one-shot disturbance (fall test)
	bool bFinished = false; // the script is over
};

class OVERBOARDGAME_API FDemoRider
{
public:
	FDemoRider();
	// Seconds = time since the demo started. bDown = fallen or in a handoff.
	FDemoPadOutput Update(double Seconds, float DeltaSeconds, bool bHaveState, const OverboardWire::FBoardState& State,
		bool bDown, const FRideGameReadout* Readout);

private:
	enum class EPhase : uint8 { Wait, Arm, Ride, StopBox, Hold, Ride2, AfterFinish, Kick, Down, Reset, Done };
	EPhase Phase = EPhase::Wait;
	double PhaseStart = 0.0;
	float SpeedIntegral = 0.f;
	double RampedTargetV = 0.0;
	double FilteredV = 0.0;
	double StalledSeconds = 0.0;
	int32 SmoothFrames = 0;
	bool bQuickRestart = false;
	bool bFullBrake = false;
	bool bFellInRide = false;
	int32 Retries = 0;
	double LastLog = -1.0;

	void Enter(EPhase Next, double Seconds);
	static double TargetY(double S);
	static double TargetSpeed(double S);
};
