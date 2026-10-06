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

	// Loads tools/play/elements/<CourseName>.json. If it has a "demo_path", the demo runs the 2D
	// lap follower; else it keeps the city_hill ride. Called once, before the first Update.
	void LoadCourse(const FString& CourseName);
	bool IsLapsMode() const { return bLapsMode; }

private:
	enum class EPhase : uint8 { Wait, Arm, Ride, StopBox, Hold, Ride2, AfterFinish, Kick, Down, Reset, Done, RideLaps };
	EPhase Phase = EPhase::Wait;
	double PhaseStart = 0.0;
	float SpeedIntegral = 0.f;
	double RampedTargetV = 0.0;
	double FilteredV = 0.0;
	double StalledSeconds = 0.0;
	int32 SmoothFrames = 0;
	bool bQuickRestart = false;
	bool bFullBrake = false;
	bool bCarveTest = false;
	double CarvePeakSpeed = 0.0;
	bool bCarveRight = true;
	float CarveSteer = 0.f;
	bool bFellInRide = false;
	int32 Retries = 0;
	double LastLog = -1.0;
	double LastUpdateSeconds = -1.0;

	// --- Laps mode (parking_lot and any course with a "demo_path") -----------------------------
	// A 2D pure-pursuit follower on the closed demo path, ported exactly from the reference pilot
	// (tools/levels/headless_pilot.py, class DemoRider). Empty paths keep the city_hill ride.
	bool bLapsMode = false;
	int32 DemoLaps = 1;                 // -ObDemoLaps=N: end after N laps (or a fall)
	TArray<double> PathX, PathY, PathV; // the demo path (x, y, v_target), 1 m apart, closed
	TArray<double> PathKappa;           // signed path curvature (left +) at each point
	double SteerOut = 0.0;              // rate-limited steer command, [-1, 1]
	int32 PathIdx = 0;                  // the nearest path index (advances forward only)
	bool bFirstSample = true;           // the first sample searches the whole path
	double Heading = 0.0;
	bool bHaveHeading = false;
	double FilteredVPrev = 0.0;
	int32 LastSeenCompletedLaps = 0;
	bool bLoggedFall = false;

	FDemoPadOutput UpdateLaps(double Seconds, float DeltaSeconds, const OverboardWire::FBoardState& State,
		bool bDown, const FRideGameReadout* Readout);

	void Enter(EPhase Next, double Seconds);
	static double TargetY(double S);
	static double TargetSpeed(double S);
};
