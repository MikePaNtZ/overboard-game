#include "DemoRider.h"

#include "RideCourseElements.h"
#include "Dom/JsonObject.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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

	// The sim-host turn law with the speed fade: the measured full-stick curvature at speed v
	// (headless_pilot.py imports this from export_level; the formula is reproduced here).
	double FullStickKappa(double V)
	{
		return FMath::Min(0.25, 0.6 * 9.81 / (V * V)) * FMath::Clamp((V - 0.8) / 2.2, 0.0, 1.0);
	}
}

void FDemoRider::Enter(EPhase Next, double Seconds)
{
	Phase = Next;
	PhaseStart = Seconds;
	UE_LOG(LogDemoRider, Log, TEXT("DemoRider: phase %d at %.2f s"), static_cast<int32>(Next), Seconds);
}

// The line through the slalom: 0.4 m outside each flag (flags at |y| = 1.8 m, alternating).
// Then onto y = -1.2 m before the stop box: a pull-away at ~1 m/s barely carves, and the old line
// (moving over after the box) rode into cone 1 at (s 99, y 0).
double FDemoRider::TargetY(double S)
{
	static const double Pts[][2] = {
		{ 0.0, 0.0 }, { 12.0, 0.0 }, { 20.0, 2.2 }, { 30.0, -2.2 }, { 40.0, 2.2 }, { 50.0, -2.2 }, { 58.0, 0.0 }, { 78.0, 0.0 }, { 86.0, -1.2 }, { 112.0, -1.2 }, { 116.0, 0.0 }, { 400.0, 0.0 },
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
	// -ObDemoCarveTest (INCONCLUSIVE as a test, 2026-10-05: on the 15 % descent the demo cannot hold
	// 6 m/s with its lean caps and drifts to the street edge; kept for a flatter level): the whole descent at a target of 6.5 m/s (8 m/s is the pushback limit on 15 %:
	// a nosedive whatever the carve), and from s 40 m to the
	// end of the speed trap, carve reversals every 1.5 s at the game's 0.4 g cap (the 6-8 m/s
	// carve check for the controls track's carving model).
	bCarveTest = FParse::Param(FCommandLine::Get(), TEXT("ObDemoCarveTest"));
	// -ObDemoLaps=N (laps mode only): end the script after N laps or a fall. Default 1.
	FParse::Value(FCommandLine::Get(), TEXT("ObDemoLaps="), DemoLaps);
	DemoLaps = FMath::Max(1, DemoLaps);
}

void FDemoRider::LoadCourse(const FString& CourseName)
{
	if (CourseName.IsEmpty() || CourseName == TEXT("none"))
	{
		return;
	}
	const FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT("tools/play/elements"), CourseName + TEXT(".json"));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* DemoPath = nullptr;
	if (!Root->TryGetArrayField(TEXT("demo_path"), DemoPath) || DemoPath->Num() < 2)
	{
		return; // no path: keep the city_hill ride
	}
	for (const TSharedPtr<FJsonValue>& V : *DemoPath)
	{
		const TArray<TSharedPtr<FJsonValue>>& P = V->AsArray();
		if (P.Num() >= 3)
		{
			PathX.Add(P[0]->AsNumber());
			PathY.Add(P[1]->AsNumber());
			PathV.Add(P[2]->AsNumber());
		}
	}
	const int32 N = PathX.Num();
	// Signed curvature (left +) at each point, from the turn in heading over the local arc length.
	PathKappa.SetNum(N);
	for (int32 i = 0; i < N; ++i)
	{
		const int32 Prev = (i - 1 + N) % N;
		const int32 Next = (i + 1) % N;
		const double H1 = FMath::Atan2(PathY[i] - PathY[Prev], PathX[i] - PathX[Prev]);
		const double H2 = FMath::Atan2(PathY[Next] - PathY[i], PathX[Next] - PathX[i]);
		const double Dh = FMath::UnwindRadians(H2 - H1);
		const double D0 = FMath::Sqrt(FMath::Square(PathX[i] - PathX[Prev]) + FMath::Square(PathY[i] - PathY[Prev]));
		const double D1 = FMath::Sqrt(FMath::Square(PathX[Next] - PathX[i]) + FMath::Square(PathY[Next] - PathY[i]));
		PathKappa[i] = Dh / FMath::Max(0.5 * (D0 + D1), 1e-3);
	}
	// Crossings (embarcadero cruise): the stop line and the road segment it protects.
	const TArray<TSharedPtr<FJsonValue>>* CrossingsJson = nullptr;
	if (Root->TryGetArrayField(TEXT("crossings"), CrossingsJson))
	{
		for (const TSharedPtr<FJsonValue>& V : *CrossingsJson)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			if (!O.IsValid()) { continue; }
			FCrossing C;
			C.StopIdx = static_cast<int32>(O->GetIntegerField(TEXT("stop_idx")));
			O->TryGetNumberField(TEXT("clear_m"), C.ClearM);
			const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* B = nullptr;
			if (O->TryGetArrayField(TEXT("a"), A) && A->Num() == 2) { C.Ax = (*A)[0]->AsNumber(); C.Ay = (*A)[1]->AsNumber(); }
			if (O->TryGetArrayField(TEXT("b"), B) && B->Num() == 2) { C.Bx = (*B)[0]->AsNumber(); C.By = (*B)[1]->AsNumber(); }
			Crossings.Add(C);
		}
	}

	bLapsMode = true;
	UE_LOG(LogDemoRider, Log, TEXT("DemoRider: laps mode, %d path points, %d lap(s), %d crossing(s)."), N, DemoLaps, Crossings.Num());
}

FDemoPadOutput FDemoRider::Update(double Seconds, float DeltaSeconds, bool bHaveState, const OverboardWire::FBoardState& State,
	bool bDown, const FRideGameReadout* Readout, const TArray<FMovingObjectSample>* Objects)
{
	FDemoPadOutput Out;
	if (!bHaveState)
	{
		return Out;
	}
	// Time step from the wall clock, like the real-time sim, not the game's frame delta: without
	// rendering (-nullrhi) the game clock runs ~3.5x fast, which tripped the stall guard (and
	// sped up the ramp and the integral) in headless tests.
	DeltaSeconds = LastUpdateSeconds < 0.0 ? 0.f : static_cast<float>(FMath::Clamp(Seconds - LastUpdateSeconds, 0.0, 0.1));
	LastUpdateSeconds = Seconds;
	if (bLapsMode)
	{
		return UpdateLaps(Seconds, DeltaSeconds, State, bDown, Readout, Objects);
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
		// No windup at a standstill: a board held still (on its pad, or settling) must not meet a
		// lean that grew while it stood.
		const float IntegralCap = FMath::Abs(V) < 0.3 ? 0.5f : 3.f;
		SpeedIntegral = FMath::Clamp(SpeedIntegral + Err * DeltaSeconds, -IntegralCap, IntegralCap);
		// In the speed trap the board is already rolling, so the rider commits more weight.
		const bool bClimb = S >= 112.0 && S < 167.0;
		const float LeanCap = (S >= kSpeedTrapS0 && S < kSpeedTrapS1) ? 0.6f : (bClimb ? 0.35f : 0.3f);
		// Gains halved for the 10 cm reach (controls track: one stick unit now moves the rider twice
		// as far, so the old gains made the demo's own speed loop rock the board).
		Out.Lean = FMath::Clamp(0.11f * Err + 0.04f * SpeedIntegral, -LeanCap, LeanCap);
		if (S >= kSpeedTrapS0 && S < kSpeedTrapS1 && !bCarveTest)
		{
			// Top-speed run: commit a steady forward lean, as a rider tucks in.
			Out.Lean = FMath::Clamp(0.35f + 0.11f * Err, -LeanCap, LeanCap);
		}
		// Coming off the speed trap, the hard lean back (L2) does most of the braking.
		if (S >= kSpeedTrapS1 && S < kStopBoxEnterS && Err < -0.5f)
		{
			Out.TailBrake = FMath::Clamp(-0.3f * Err, 0.f, 0.7f);
		}

		// Carve along the line by pure pursuit. Heading: yaw positive = left, so the rightward
		// heading angle is -yaw; positive steer turns right.
		const double Ahead = FMath::Max(kLookaheadM, 1.2 * FMath::Abs(V));
		// Carve test: a straight line; the only carving is the timed reversals below (the slalom
		// line at 6-7 m/s needs far more than 0.4 g and is not what the test checks).
		const double LineY = bCarveTest ? 0.0 : TargetY(S + Ahead);
		const double Theta = FMath::Atan2(LineY - Y, Ahead);
		const double Alpha = Theta + State.YawRad;
		const double Kappa = 2.0 * FMath::Sin(Alpha) / Ahead;
		// At most 0.6 stick: full steer at 3-4 m/s on the 15 % descent is past the carve limit and
		// rolls the board over (controls track, same in every sim build).
		Out.Steer = static_cast<float>(FMath::Clamp(Kappa / kKappaMax, -0.6, 0.6));
		if (bCarveTest && S >= 40.0 && S < kSpeedTrapS1)
		{
			// Same cap as the rider body: 0.4 g of the sim's full-stick curvature min(0.25, 0.6 g / v^2).
			const double V2 = FMath::Max(V * V, 0.01);
			const double Cap = FMath::Min(1.0, (0.4 * 9.81 / V2) / FMath::Min(0.25, 0.6 * 9.81 / V2));
			// Reverse on position, not on a timer (a timer drifted the board into the kerb at
			// y = 6 m): carve right until 1.5 m right of the centre line, then left, and so on.
			if (Y > 1.0) { bCarveRight = false; }
			if (Y < -1.0) { bCarveRight = true; }
			// Through the same rate limit as the pad's rider body (5 stick units/s): a step from
			// +cap to -cap at 7.7 m/s from 30 deg of bank threw the board over (controls trace).
			const float Want = static_cast<float>(bCarveRight ? Cap : -Cap);
			CarveSteer = FMath::FInterpConstantTo(CarveSteer, Want, DeltaSeconds, 5.f);
			Out.Steer = CarveSteer;
			CarvePeakSpeed = FMath::Max(CarvePeakSpeed, V);
		}
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
		RideControl(bCarveTest && S < kSpeedTrapS1 ? (S < 8.0 ? 2.5 : 6.0) : TargetSpeed(S));
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
		if (T > (bQuickRestart ? 3.0 : 7.0)) { SpeedIntegral = 0.f; RampedTargetV = 0.0; FilteredV = 0.0; StalledSeconds = 0.0; Enter(EPhase::Ride2, Seconds); }
		break;
	case EPhase::Ride2:
		RideControl(TargetSpeed(S));
		// A stall (the 12 % climb at 95 kg can stall): after 8 s stopped, go on to the fall test.
		// Count only while the rider is asking to go (ramped target >= 1 m/s): a slow pull-away
		// after a long settle is not a stall, and the guard fired the fall-test kick there.
		StalledSeconds = (FMath::Abs(V) < 0.3 && RampedTargetV >= 1.0) ? StalledSeconds + DeltaSeconds : 0.0;
		if (StalledSeconds > 8.0) { UE_LOG(LogDemoRider, Log, TEXT("DemoRider: stalled at s %.1f"), S); Enter(EPhase::Kick, Seconds); }
		else if (bDown) { bFellInRide = true; Enter(EPhase::Down, Seconds); }
		else if (S >= kFinishS + 2.0) { Enter(EPhase::AfterFinish, Seconds); }
		break;
	case EPhase::AfterFinish:
		// Stop on the short crest before the fall test, or the wipeout slides off the street end.
		RideControl(0.0);
		Out.TailBrake = FMath::Abs(V) > 0.3 ? 0.6f : 0.f;
		if ((FMath::Abs(V) < 0.3 && T > 1.0) || T > 6.0) { Enter(EPhase::Kick, Seconds); }
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

// Distance from (X, Y) to the segment A-B. Mirrors headless_pilot.py seg_dist.
static double SegDist(double X, double Y, double Ax, double Ay, double Bx, double By)
{
	const double Dx = Bx - Ax;
	const double Dy = By - Ay;
	const double T = FMath::Clamp(((X - Ax) * Dx + (Y - Ay) * Dy) / FMath::Max(Dx * Dx + Dy * Dy, 1e-9), 0.0, 1.0);
	return FMath::Sqrt(FMath::Square(X - Ax - T * Dx) + FMath::Square(Y - Ay - T * Dy));
}

FDemoPadOutput FDemoRider::UpdateLaps(double Seconds, float DeltaSeconds, const OverboardWire::FBoardState& State,
	bool bDown, const FRideGameReadout* Readout, const TArray<FMovingObjectSample>* Objects)
{
	// A direct port of headless_pilot.py DemoRider: forward-only nearest-index search on the closed
	// demo path, pure pursuit plus a curvature feedforward, and a PI speed loop on the lean. The
	// clock and dt come from the wall clock (passed in), like the real-time sim.
	FDemoPadOutput Out;
	const double X = State.Pos[0];
	const double Y = State.Pos[1];
	const double V = State.WheelRateRadS * kDemoWheelRadiusM; // positive = forward
	const double T = Seconds - PhaseStart;
	const int32 N = PathX.Num();
	auto Wrap = [N](int32 K) { return ((K % N) + N) % N; };
	// Sample the path at a CONTINUOUS index (linear interpolation between the two bracketing
	// points), not at round(index). The path is 1 m apart, so the index is a distance in metres.
	// The reference pilot snaps round() to an integer index; at a turn onset a ~0.1 m/s speed
	// difference (unavoidable between the 100 Hz Python loop and the game) flips that index by one
	// and starts the carve a point early or late. Interpolation reads the same feed-forward for
	// both, so the game no longer cuts 1.5 m inside the first cone. The control intent is unchanged.
	auto SampleXY = [&](double FIdx, double& OutX, double& OutY)
	{
		const double Fl = FMath::FloorToDouble(FIdx);
		const double Frac = FIdx - Fl;
		const int32 A = Wrap(static_cast<int32>(Fl));
		const int32 B = Wrap(A + 1);
		OutX = PathX[A] + Frac * (PathX[B] - PathX[A]);
		OutY = PathY[A] + Frac * (PathY[B] - PathY[A]);
	};
	auto SampleKappa = [&](double FIdx)
	{
		const double Fl = FMath::FloorToDouble(FIdx);
		const double Frac = FIdx - Fl;
		const int32 A = Wrap(static_cast<int32>(Fl));
		const int32 B = Wrap(A + 1);
		return PathKappa[A] + Frac * (PathKappa[B] - PathKappa[A]);
	};

	// Log each completed lap (a readout edge) and the first fall.
	if (Readout && Readout->CompletedLaps > LastSeenCompletedLaps)
	{
		LastSeenCompletedLaps = Readout->CompletedLaps;
		if (Readout->bLastLapClean)
		{
			UE_LOG(LogDemoRider, Log, TEXT("OBLAP %d %.1f CLEAN"), Readout->CompletedLaps, Readout->LastLapSeconds);
		}
		else
		{
			UE_LOG(LogDemoRider, Log, TEXT("OBLAP %d %.1f MISSED %s"), Readout->CompletedLaps,
				Readout->LastLapSeconds, *Readout->LastLapMissed);
		}
	}

	switch (Phase)
	{
	case EPhase::Wait:
		// Arm only once the game runs smoothly (see the city_hill path for why).
		SmoothFrames = DeltaSeconds < 0.1f ? SmoothFrames + 1 : 0;
		if (Seconds > 2.5 && SmoothFrames >= 30) { Enter(EPhase::Arm, Seconds); }
		break;
	case EPhase::Arm:
		Out.bArm = true;
		if ((T > 0.3 && FMath::Abs(V) > 0.05) || T > 2.0) { Enter(EPhase::RideLaps, Seconds); }
		break;
	case EPhase::RideLaps:
	{
		Out.bArm = true; // keep the board armed, like the reference pilot
		if (bDown)
		{
			if (!bLoggedFall) { bLoggedFall = true; UE_LOG(LogDemoRider, Log, TEXT("OBFALL %.1f %.1f"), X, Y); }
			Enter(EPhase::Done, Seconds);
			break;
		}
		if (Readout && Readout->CompletedLaps >= DemoLaps) { Enter(EPhase::Done, Seconds); break; }

		// Heading from the world velocity when moving; else the first path segment.
		const double Spd = FMath::Sqrt(State.LinVel[0] * State.LinVel[0] + State.LinVel[1] * State.LinVel[1]);
		if (Spd > 0.4) { Heading = FMath::Atan2(State.LinVel[1], State.LinVel[0]); bHaveHeading = true; }
		else if (!bHaveHeading) { Heading = FMath::Atan2(PathY[1] - PathY[0], PathX[1] - PathX[0]); bHaveHeading = true; }

		// Advance the nearest path index, forward only (the first sample searches the whole path).
		double Best = 1e18;
		int32 Bi = PathIdx;
		const int32 Lo = bFirstSample ? 0 : PathIdx;
		const int32 Hi = bFirstSample ? N : PathIdx + 60;
		for (int32 k = Lo; k < Hi; ++k)
		{
			const int32 m = Wrap(k);
			const double d = FMath::Square(PathX[m] - X) + FMath::Square(PathY[m] - Y);
			if (d < Best) { Best = d; Bi = k; }
		}
		PathIdx = Bi;
		bFirstSample = false;

		// Pure pursuit plus a curvature feedforward about 0.7 s ahead (positive steer = right).
		const double Ld = FMath::Max(3.5, 1.5 * FMath::Abs(V));
		double TgtX, TgtY;
		SampleXY(PathIdx + Ld, TgtX, TgtY);
		double Alpha = FMath::Atan2(TgtY - Y, TgtX - X) - Heading;
		Alpha = FMath::UnwindRadians(Alpha);
		const double Kff = SampleKappa(PathIdx + 1.0 * FMath::Abs(V) + 1.0);
		const double KappaLeft = 0.6 * 2.0 * FMath::Sin(Alpha) / Ld + 0.8 * Kff;
		const double Full = FullStickKappa(FMath::Max(FMath::Abs(V), 1.0));
		const double Cap = FMath::Abs(V) < 3.2 ? 1.0 : 0.6;
		const double Want = FMath::Clamp(-KappaLeft / Full, -Cap, Cap);
		SteerOut = FMath::FInterpConstantTo(SteerOut, Want, DeltaSeconds, 5.0); // 5 stick/s rate limit
		Out.Steer = static_cast<float>(SteerOut);

		// Speed loop: ramp the target up at 0.6 m/s^2 (down at once), filter the speed, lean P + I.
		double Vt = PathV[Wrap(PathIdx + 2)];

		// Give way at the crossings: hold at the stop line while a car or a cyclist is inside the
		// crossing zone, then 2 s more. A direct port of headless_pilot.py's crossing loop. Uses
		// the wall-clock Seconds for the 2 s clear timer (the state packet has no sim time here).
		for (FCrossing& C : Crossings)
		{
			const int32 ToStop = Wrap(C.StopIdx - PathIdx);
			if (ToStop > 25) { continue; }
			bool bBusy = false;
			if (Objects)
			{
				for (const FMovingObjectSample& O : *Objects)
				{
					if (O.Kind != 0 && O.Kind != 2) { continue; } // only cars and cyclists block
					if (SegDist(O.Pos[0], O.Pos[1], C.Ax, C.Ay, C.Bx, C.By) < C.ClearM) { bBusy = true; break; }
				}
			}
			if (bBusy) { C.ClearSince = -1.0; }
			else if (C.ClearSince < 0.0) { C.ClearSince = Seconds; }
			if (bBusy || Seconds - C.ClearSince < 2.0)
			{
				Vt = FMath::Min(Vt, ToStop <= 3 ? 0.0 : 0.25 * ToStop);
				if (!C.bLoggedHold)
				{
					C.bLoggedHold = true;
					UE_LOG(LogDemoRider, Log, TEXT("OBWAIT crossing stop_idx %d at t %.1f x %+.1f y %+.1f"), C.StopIdx, Seconds, X, Y);
				}
			}
		}
		RampedTargetV = Vt >= RampedTargetV ? FMath::Min(Vt, RampedTargetV + 0.6 * DeltaSeconds) : Vt;
		const double Af = 1.0 - FMath::Exp(-DeltaSeconds / 0.25);
		FilteredV += Af * (V - FilteredV);
		const double Err = RampedTargetV - FilteredV;
		const double ICap = FMath::Abs(V) < 0.3 ? 0.5 : 1.0;
		SpeedIntegral = FMath::Clamp(SpeedIntegral + static_cast<float>(Err * DeltaSeconds),
			static_cast<float>(-ICap), static_cast<float>(ICap));
		const double Accel = DeltaSeconds > 1e-6 ? (FilteredV - FilteredVPrev) / DeltaSeconds : 0.0;
		FilteredVPrev = FilteredV;
		Out.Lean = static_cast<float>(FMath::Clamp(0.20 * Err - 0.05 * Accel + 0.01 * SpeedIntegral, -0.35, 0.35));
		break;
	}
	case EPhase::Done:
		Out.bFinished = true;
		break;
	default:
		break;
	}

	if (Seconds - LastLog > 0.5)
	{
		LastLog = Seconds;
		UE_LOG(LogDemoRider, Log, TEXT("DemoRider: t %.1f x %+.1f y %+.1f v %.2f idx %d lean %+.2f steer %+.2f laps %d/%d"),
			Seconds, X, Y, V, PathIdx, Out.Lean, Out.Steer, Readout ? Readout->CompletedLaps : 0, DemoLaps);
	}
	return Out;
}
