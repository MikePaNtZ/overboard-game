#include "DemoRider.h"

#include "RideCourseElements.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogDemoRider, Log, All);

namespace
{
	constexpr double kStartX = 90.0;        // city_hill: s = 90 - MuJoCo x
	constexpr double kDemoWheelRadiusM = 0.146;
	constexpr double kKappaMax = 0.25;      // lean-steer full-stick curvature, 1/m (sim-host)
	constexpr double kLookaheadM = 4.0;
	constexpr double kStopBoxEnterS = 90.5;
	constexpr double kSpeedTrapS0 = 52.0, kSpeedTrapS1 = 76.0;
	constexpr double kFinishS = 172.0;
}

void FDemoRider::Enter(EPhase Next, double Seconds)
{
	Phase = Next;
	PhaseStart = Seconds;
	UE_LOG(LogDemoRider, Log, TEXT("DemoRider: phase %d at %.2f s"), static_cast<int32>(Next), Seconds);
}

// The line through the slalom: 0.4 m outside each flag (flags at |y| = 1.8 m, alternating).
double FDemoRider::TargetY(double S)
{
	static const double Pts[][2] = {
		{ 0.0, 0.0 }, { 12.0, 0.0 }, { 20.0, 2.2 }, { 30.0, -2.2 }, { 40.0, 2.2 }, { 50.0, -2.2 }, { 58.0, 0.0 }, { 94.0, 0.0 }, { 97.0, -1.2 }, { 112.0, -1.2 }, { 116.0, 0.0 }, { 400.0, 0.0 },
	};
	for (int32 i = 1; i < UE_ARRAY_COUNT(Pts); ++i)
	{
		if (S <= Pts[i][0])
		{
			const double A = (S - Pts[i - 1][0]) / (Pts[i][0] - Pts[i - 1][0]);
			return FMath::Lerp(Pts[i - 1][1], Pts[i][1], FMath::Clamp(A, 0.0, 1.0));
		}
	}
	return 0.0;
}

double FDemoRider::TargetSpeed(double S)
{
	if (S < 8.0) { return 2.5; }            // run-in
	if (S < kSpeedTrapS0) { return 3.0; }   // slalom
	if (S < kSpeedTrapS1) { return 8.0; }   // speed trap: top speed (pushback starts at 8.34 m/s)
	if (S < kStopBoxEnterS) { return 1.2; } // brake for the stop box
	if (S < 112.0) { return 2.4; }          // slow zone (< 3 m/s)
	if (S < 167.0) { return 3.2; }          // 12 % climb
	return 2.5;                              // crest
}

FDemoRider::FDemoRider()
{
	// Since controls ddb1535 (pad-mode pull-away) the plain quick pull-away is clean, and the old
	// slow restart (lean -0.05, 7 s settle) tipped the board back; -ObDemoSlowRestart keeps it.
	bQuickRestart = !FParse::Param(FCommandLine::Get(), TEXT("ObDemoSlowRestart"));
	// -ObDemoFullBrake: a full L2 pull in the stop box (the tail tip-over test case).
	bFullBrake = FParse::Param(FCommandLine::Get(), TEXT("ObDemoFullBrake"));
}

FDemoPadOutput FDemoRider::Update(double Seconds, float DeltaSeconds, bool bHaveState, const OverboardWire::FBoardState& State,
	bool bDown, const FRideGameReadout* Readout)
{
	FDemoPadOutput Out;
	if (!bHaveState)
	{
		return Out;
	}
	const double S = kStartX - State.Pos[0];
	const double Y = State.Pos[1];
	const double V = State.WheelRateRadS * kDemoWheelRadiusM; // positive = forward
	const double T = Seconds - PhaseStart;

	auto RideControl = [&](double TargetV)
	{
		// Lean to hold a speed, as a rider does: proportional plus a slow integral (the grade).
		// A rider does not throw full weight forward from a standstill: the target speed ramps
		// up at 0.4 m/s^2 and the lean stays within +-0.3 (with the 10 cm reach, a sustained
		// +0.5 lean from rest drove the nose into the street in 1.6 s).
		RampedTargetV = FMath::Min(TargetV, RampedTargetV + 0.4 * DeltaSeconds);
		if (TargetV < RampedTargetV) { RampedTargetV = TargetV; }
		// The wheel speed rocks with the pitch; a rider feels the average, not each swing.
		const double A = 1.0 - FMath::Exp(-DeltaSeconds / 0.25);
		FilteredV += A * (V - FilteredV);
		const float Err = static_cast<float>(RampedTargetV - FilteredV);
		SpeedIntegral = FMath::Clamp(SpeedIntegral + Err * DeltaSeconds, -3.f, 3.f);
		// In the speed trap the board is already rolling, so the rider commits more weight.
		const bool bClimb = S >= 112.0 && S < 167.0;
		const float LeanCap = (S >= kSpeedTrapS0 && S < kSpeedTrapS1) ? 0.5f : (bClimb ? 0.35f : 0.3f);
		// Gains halved for the 10 cm reach (controls track: one stick unit now moves the rider twice
		// as far, so the old gains made the demo's own speed loop rock the board).
		Out.Lean = FMath::Clamp(0.11f * Err + 0.04f * SpeedIntegral, -LeanCap, LeanCap);
		if (S >= kSpeedTrapS0 && S < kSpeedTrapS1)
		{
			// Top-speed run: commit a steady forward lean, as a rider tucks in.
			Out.Lean = FMath::Clamp(0.25f + 0.11f * Err, -LeanCap, LeanCap);
		}
		// Coming off the speed trap, the hard lean back (L2) does most of the braking.
		if (S >= kSpeedTrapS1 && S < kStopBoxEnterS && Err < -0.5f)
		{
			Out.TailBrake = FMath::Clamp(-0.3f * Err, 0.f, 0.7f);
		}

		// Carve along the line by pure pursuit. Heading: yaw positive = left, so the rightward
		// heading angle is -yaw; positive steer turns right.
		const double Ahead = FMath::Max(kLookaheadM, 1.2 * FMath::Abs(V));
		const double Theta = FMath::Atan2(TargetY(S + Ahead) - Y, Ahead);
		const double Alpha = Theta + State.YawRad;
		const double Kappa = 2.0 * FMath::Sin(Alpha) / Ahead;
		// At most 0.6 stick: full steer at 3-4 m/s on the 15 % descent is past the carve limit and
		// rolls the board over (controls track, same in every sim build).
		Out.Steer = static_cast<float>(FMath::Clamp(Kappa / kKappaMax, -0.6, 0.6));
	};

	switch (Phase)
	{
	case EPhase::Wait:
		// Arm only once the game runs smoothly: while it loads it can run at ~1 fps, and a short
		// arm press then falls between two frames and never reaches the sim.
		SmoothFrames = DeltaSeconds < 0.1f ? SmoothFrames + 1 : 0;
		if (Seconds > 2.5 && SmoothFrames >= 30) { Enter(EPhase::Arm, Seconds); }
		break;
	case EPhase::Arm:
		// Hold Cross until the sim releases the board (it moves), or 2 s at most.
		Out.bArm = true;
		if ((T > 0.3 && FMath::Abs(V) > 0.05) || T > 2.0) { Enter(EPhase::Ride, Seconds); }
		break;
	case EPhase::Ride:
		RideControl(TargetSpeed(S));
		if (bDown) { bFellInRide = true; Enter(EPhase::Down, Seconds); }
		else if (S >= kStopBoxEnterS) { Enter(EPhase::StopBox, Seconds); }
		break;
	case EPhase::StopBox:
		// The deliberate lean back on L2 drags the tail pad and stops the board. 0.6, not a full
		// pull: a full pull at walking speed on the flat tipped the board past the 35 deg fall
		// limit (reported to the controls track).
		// Ease the brake as the board slows: at walking speed a firm tail drag lets the motor drive
		// the board over its tail pivot (controls track).
		Out.TailBrake = bFullBrake ? 1.f : static_cast<float>(FMath::Clamp(0.25 + 0.2 * FMath::Abs(V), 0.25, 0.6));
		if (FMath::Abs(V) < 0.15 && T > 0.5) { Enter(EPhase::Hold, Seconds); }
		break;
	case EPhase::Hold:
		// Stand on the brake for a moment, then ease off over 2 s: a sudden release swings the
		// nose down and starts a rock that the 10 cm reach makes worse.
		// Ease off the tail over 3 s and let the board settle before pulling away: a quick
		// restart from a tail-down stop ended in a nose strike every time (reported to controls).
		Out.TailBrake = static_cast<float>(0.25 * FMath::Clamp(1.0 - T / (bQuickRestart ? 1.5 : 3.0), 0.0, 1.0));
		Out.Lean = bQuickRestart ? 0.f : -0.05f;
		if (T > (bQuickRestart ? 3.0 : 7.0)) { SpeedIntegral = 0.f; RampedTargetV = 0.0; FilteredV = 0.0; Enter(EPhase::Ride2, Seconds); }
		break;
	case EPhase::Ride2:
		RideControl(TargetSpeed(S));
		// A stall (the 12 % climb at 95 kg can stall): after 8 s stopped, go on to the fall test.
		StalledSeconds = FMath::Abs(V) < 0.3 ? StalledSeconds + DeltaSeconds : 0.0;
		if (StalledSeconds > 8.0) { UE_LOG(LogDemoRider, Log, TEXT("DemoRider: stalled at s %.1f"), S); Enter(EPhase::Kick, Seconds); }
		else if (bDown) { bFellInRide = true; Enter(EPhase::Down, Seconds); }
		else if (S >= kFinishS + 2.0) { Enter(EPhase::AfterFinish, Seconds); }
		break;
	case EPhase::AfterFinish:
		RideControl(0.0); // ease to a stop on the crest
		if (T > 3.0) { Enter(EPhase::Kick, Seconds); }
		break;
	case EPhase::Kick:
		Out.bKick = T < 0.1; // the fall test: one disturbance, then the board goes over
		if (bDown) { Enter(EPhase::Down, Seconds); }
		else if (T > 3.0) { Enter(EPhase::Kick, Seconds); }
		break;
	case EPhase::Down:
		if (T > 3.0) { Enter(EPhase::Reset, Seconds); }
		break;
	case EPhase::Reset:
		Out.bReset = T < 0.3;
		if (T > 3.0)
		{
			// A fall during the ride: start the run again. The planned fall test: the demo is over.
			if (bFellInRide && ++Retries <= 2)
			{
				bFellInRide = false;
				SpeedIntegral = 0.f;
				RampedTargetV = 0.0;
				Enter(EPhase::Arm, Seconds);
			}
			else
			{
				Enter(EPhase::Done, Seconds);
			}
		}
		break;
	case EPhase::Done:
		Out.bFinished = true;
		break;
	}

	if (Seconds - LastLog > 0.5)
	{
		LastLog = Seconds;
		UE_LOG(LogDemoRider, Log, TEXT("DemoRider: t %.1f s %.1f y %+.2f v %.2f yaw %+.3f pitch %+.3f lean %+.2f steer %+.2f L2 %.1f score %d"),
			Seconds, S, Y, V, State.YawRad, State.PitchRad, Out.Lean, Out.Steer, Out.TailBrake, Readout ? Readout->Score : 0);
	}
	return Out;
}
