// fake_sender: emits real OBW1 packets over UDP to 127.0.0.1:9601, standing in for the
// controls host so the receive+decode path can be proven end-to-end without the real host
// (which the Controls session is building against ADR-0010 right now and cannot hand us).
//
// Usage:
//   fake_sender                 sends N well-formed packets, pos sweeping in X, one per 20ms
//   fake_sender --bad-magic     sends one corrupt-magic packet, to prove the receiver drops it
//   fake_sender --rotate        sends a scripted attitude sequence for the in-editor handedness
//                                check -- see docs/w1-manual-editor-steps.md for what to expect
//                                on screen for each phase
//   fake_sender --burst         mimics the real host's measured cadence instead of a tidy 20ms
//                                pace: bursts of 8 packets sent back-to-back, then a 16ms idle
//                                gap, repeated. Exercises BoardStateClient's drain-and-retain
//                                path against the actual bursty timing (mean ~2ms inter-packet,
//                                p99 ~15ms, max ~17.6ms, measured on the real Controls host --
//                                see overboard#162) rather than the smooth pace the client was
//                                originally written against.
//   fake_sender --count N       override packet count for the default position-sweep mode, or
//                                burst count for --burst (default 50; ignored by --rotate, which
//                                is time-boxed instead)
//   fake_sender --rider         sets rider_fore_aft_m/rider_lateral_m to fixed, realistic
//                                displacements (3cm fore, 4cm lateral -- the COO's stated "full
//                                lateral" figure) on every packet in ANY mode, v2 schema. Exists
//                                to exercise the rider-offset rendering path without a real host
//                                (which doesn't speak v2 yet either). Combine with other modes,
//                                e.g. `fake_sender --rider --rotate`.
//   fake_sender --carve         sweeps wheel rate and rider lateral displacement through their
//                                full ranges (and past them, to exercise the clamps) so every
//                                corner of the rider riding blendspace is reached. Use this, not
//                                --rider, to check the riding animation: --rider holds a FIXED
//                                displacement, so the turn axis never moves. ~24 s, self-labelling
//                                per phase. Overrides --rider's rider channel if both are given.
//   fake_sender --authority-cliff
//                               replays ADR-0011's MEASURED full-stick-from-rest cliff at 50Hz:
//                                the loss-of-authority warning bit at sim_t 3.000s, envelope
//                                saturation at 4.920s, FALLEN at 5.868s, inverted at 6.470s.
//                                Exists so the warning can be TRIGGERED and its on-screen lead
//                                timed -- the real host computes the signal but does not put it
//                                on the wire yet (see EStateFlags::AuthorityWarning). Replays
//                                measured numbers; computes no physics.
//
// macOS/BSD sockets only, no UE dependency.
#include "../OverboardWire.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace OverboardWire;

namespace
{
	constexpr int kSendIntervalMs = 20; // 50 Hz -- arbitrary, just fast enough to look continuous

	// Set by --rider; applied in BaseState() below. 0 (default) means "no rider offset", which
	// is indistinguishable from a v1 host as far as the rider fields go.
	bool gRiderTest = false;
	constexpr float kRiderTestForeAftM = 0.03f;   // 3cm forward
	constexpr float kRiderTestLateralM = -0.04f;  // 4cm lateral -- COO's stated "full lateral" figure

	int OpenSocketToHost(sockaddr_in& OutDest)
	{
		int Sock = socket(AF_INET, SOCK_DGRAM, 0);
		if (Sock < 0)
		{
			std::perror("socket");
			return -1;
		}
		OutDest = sockaddr_in{};
		OutDest.sin_family = AF_INET;
		OutDest.sin_port = htons(9601);
		inet_pton(AF_INET, "127.0.0.1", &OutDest.sin_addr);
		return Sock;
	}

	void SendState(int Sock, const sockaddr_in& Dest, const FBoardState& State)
	{
		uint8_t Buf[kStatePacketWireSizeV2]; // BaseState() defaults to v2 (kStateSchemaVersionLatest)
		EncodeBoardState(State, Buf);
		ssize_t Sent = sendto(Sock, Buf, sizeof(Buf), 0, reinterpret_cast<const sockaddr*>(&Dest), sizeof(Dest));
		if (Sent != static_cast<ssize_t>(sizeof(Buf)))
		{
			std::perror("sendto");
		}
	}

	FBoardState BaseState(uint64_t Seq, double SimTimeS)
	{
		FBoardState State;
		State.Flags = EStateFlags::Armed | EStateFlags::Valid;
		State.Seq = Seq;
		State.SimTimeS = SimTimeS;
		State.Pos[0] = 0.f;
		State.Pos[1] = 0.f;
		State.Pos[2] = 0.5f;
		State.Quat[0] = 1.f; State.Quat[1] = 0.f; State.Quat[2] = 0.f; State.Quat[3] = 0.f;
		if (gRiderTest)
		{
			State.RiderForeAftM = kRiderTestForeAftM;
			State.RiderLateralM = kRiderTestLateralM;
		}
		return State;
	}

	// ---- The same physically-named attitudes wire/tests/test_wire.cpp asserts on
	// MuJoCoToUnreal(), so the on-screen check and the CI check are testing the same claims.
	// See the derivation comments there for why each is the raw MuJoCo quaternion it is.
	void SetNoseUp(FBoardState& S, double PhiRad)   { S.Quat[0] = static_cast<float>(std::cos(PhiRad / 2.0)); S.Quat[1] = 0.f; S.Quat[2] = static_cast<float>(-std::sin(PhiRad / 2.0)); S.Quat[3] = 0.f; }
	void SetYawLeft(FBoardState& S, double PhiRad)  { S.Quat[0] = static_cast<float>(std::cos(PhiRad / 2.0)); S.Quat[1] = 0.f; S.Quat[2] = 0.f; S.Quat[3] = static_cast<float>(std::sin(PhiRad / 2.0)); }
	void SetRollRight(FBoardState& S, double PhiRad){ S.Quat[0] = static_cast<float>(std::cos(PhiRad / 2.0)); S.Quat[1] = static_cast<float>(std::sin(PhiRad / 2.0)); S.Quat[2] = 0.f; S.Quat[3] = 0.f; }

	// ---- --authority-cliff ------------------------------------------------------------------
	//
	// Replays ADR-0011's measured full-stick-from-rest cliff at 50 Hz so the loss-of-authority
	// warning can be TRIGGERED and its on-screen lead timed, without the real host (which does
	// not put the warning on the wire yet -- see EStateFlags::AuthorityWarning).
	//
	// **This computes nothing.** Every instant below is a number measured in `overboard` and
	// replayed here; the boundary rule that nothing outside that repo computes board physics is
	// intact, in the same way a pose track is replayed rather than derived. The pitch between
	// the stated instants is interpolated purely so the board moves on screen.
	//
	// | sim time | event                                    | source                  |
	// |----------|------------------------------------------|-------------------------|
	// | 3.000 s  | filtered authority utilisation > 0.85    | overboard#205           |
	// | 4.920 s  | envelope saturated, 40 A pinned          | ADR-0011 / #205 (4.92)  |
	// | 5.868 s  | FALLEN trips, -20.1 deg                  | ADR-0011 / #205 (5.87)  |
	// | 6.470 s  | fully inverted, -179.5 deg               | ADR-0011                |
	//
	// The point of the mode is the first row against the third: 2.868 s of lead, versus FALLEN
	// TRAILING saturation by -0.948 s.
	constexpr double kCliffWarningS    = 3.000;
	constexpr double kCliffSaturationS = 4.920;
	constexpr double kCliffFallenS     = 5.868;
	constexpr double kCliffInvertedS   = 6.470;
	constexpr double kCliffEndS        = 7.000;

	double CliffPitchDeg(double T)
	{
		// Piecewise-linear through ADR-0011's own table. Presentation only.
		if (T <= kCliffSaturationS) { return -10.4 * (T / kCliffSaturationS); }
		if (T <= kCliffFallenS)     { return -10.4 + (-20.1 + 10.4) * (T - kCliffSaturationS) / (kCliffFallenS - kCliffSaturationS); }
		if (T <= kCliffInvertedS)   { return -20.1 + (-179.5 + 20.1) * (T - kCliffFallenS) / (kCliffInvertedS - kCliffFallenS); }
		return -179.5;
	}

	void RunAuthorityCliff()
	{
		sockaddr_in Dest;
		int Sock = OpenSocketToHost(Dest);
		if (Sock < 0) { return; }

		std::printf("fake_sender --authority-cliff: replaying ADR-0011's measured full-stick cliff.\n");
		std::printf("  warning bit set at sim_t=%.3f s, FALLEN at %.3f s -- %.3f s of lead.\n",
			kCliffWarningS, kCliffFallenS, kCliffFallenS - kCliffWarningS);

		const auto WallStart = std::chrono::steady_clock::now();
		bool AnnouncedWarning = false;
		bool AnnouncedFallen = false;
		uint64_t Seq = 0;

		for (double T = 0.0; T <= kCliffEndS; T += kSendIntervalMs / 1000.0)
		{
			FBoardState State = BaseState(Seq++, T);

			const double PitchDeg = CliffPitchDeg(T);
			const double PitchRad = PitchDeg * 3.14159265358979323846 / 180.0;
			State.PitchRad = static_cast<float>(PitchRad);
			SetNoseUp(State, PitchRad);
			State.Pos[0] = static_cast<float>(0.5 * 2.0 * T * T); // roughly the recorded ground speed
			State.MotorCurrentA = static_cast<float>(T >= kCliffSaturationS ? 40.0 : 40.0 * (T / kCliffSaturationS));

			if (T >= kCliffWarningS)
			{
				State.Flags |= EStateFlags::AuthorityWarning;
				if (!AnnouncedWarning)
				{
					AnnouncedWarning = true;
					const double WallMs = std::chrono::duration<double, std::milli>(
						std::chrono::steady_clock::now() - WallStart).count();
					std::printf("  [wall %8.1f ms] AuthorityWarning bit SET   (sim_t=%.3f s)\n", WallMs, T);
					std::fflush(stdout);
				}
			}
			if (T >= kCliffFallenS)
			{
				State.Flags |= EStateFlags::Fallen;
				if (!AnnouncedFallen)
				{
					AnnouncedFallen = true;
					const double WallMs = std::chrono::duration<double, std::milli>(
						std::chrono::steady_clock::now() - WallStart).count();
					std::printf("  [wall %8.1f ms] Fallen bit SET             (sim_t=%.3f s)\n", WallMs, T);
					std::fflush(stdout);
				}
			}

			SendState(Sock, Dest, State);

			// ABSOLUTE schedule, not accumulated sleeps. `sleep_for(20ms)` in a loop measured
			// ~8 Hz here, not 50 -- macOS sleep granularity plus per-iteration work, compounding
			// every iteration. That is not a cosmetic difference: it is EXACTLY the defect
			// overboard#191 fixed, where `send-input` paced on a wall clock delivered 7-13 Hz
			// against the host's 100 ms staleness cutoff, silently zeroed a run-varying fraction
			// of the input, and thereby MASKED the very instability ADR-0011 was called over.
			// The one lesson that repo paid for twice is not worth re-learning in a test tool,
			// so this replay is paced against a fixed origin and reports the rate it achieved.
			std::this_thread::sleep_until(WallStart + std::chrono::duration_cast<
				std::chrono::steady_clock::duration>(std::chrono::duration<double>(T + kSendIntervalMs / 1000.0)));
		}

		const double ElapsedS = std::chrono::duration<double>(
			std::chrono::steady_clock::now() - WallStart).count();
		const double RateHz = Seq / ElapsedS;
		std::printf("fake_sender --authority-cliff: done -- %llu packets in %.3f s = %.1f Hz.\n",
			static_cast<unsigned long long>(Seq), ElapsedS, RateHz);
		if (RateHz < 12.0)
		{
			std::printf("  WARNING: delivered at %.1f Hz, at or under the host's 100 ms staleness\n"
			            "  cutoff. A run at this rate measures nothing (overboard#191).\n", RateHz);
		}
		close(Sock);
	}

	// ---- --carve ---------------------------------------------------------------------------
	//
	// Exercises every corner of the rider riding blendspace (overboard-game rider animation work).
	// This mode exists because --rider holds a FIXED displacement, so with it the blendspace turn
	// axis sits at one constant value and the rider never visibly moves -- the Left_2/3 and
	// Right_2/3 poses would first appear in real footage, never having been seen.
	//
	// Deliberately sweeps PAST the client's declared full-lean gains (5.0 m/s, 0.04 m lateral) so
	// the clamps at both ends get exercised too: an animation that keeps deforming past the axis
	// limit is a mapping bug, and one that pops on reaching it is a blend bug. Neither is visible
	// if the stimulus stays politely inside range.
	//
	// This is a TEST STIMULUS, not a simulation: the numbers are chosen to cover the input space,
	// and no combination here is claimed to be a trajectory MuJoCo would produce.
	void RunCarveSweep()
	{
		constexpr float kWheelRadiusM = 0.1454f; // matches BoardActor.cpp / mesh/README.md
		constexpr double kPhaseSeconds[] = {6.0, 12.0, 4.0, 2.0};
		const char* const kPhaseLabels[] = {
			"accelerate 0 -> 6 m/s, straight (Idle -> Forward)",
			"hold ~3 m/s, lateral sweeps full left <-> full right x2 (Left_1/2/3, Right_1/2/3)",
			"decelerate through zero to -2 m/s (Forward -> Idle -> Backward)",
			"back to rest (Idle)",
		};

		sockaddr_in Dest;
		int Sock = OpenSocketToHost(Dest);
		if (Sock < 0) { return; }

		const double Dt = kSendIntervalMs / 1000.0;
		double T = 0.0;
		double PosX = 0.0;
		double WheelAngleRad = 0.0;
		uint64_t Seq = 0;
		int ReportedPhase = -1;

		double TotalSeconds = 0.0;
		for (double P : kPhaseSeconds) { TotalSeconds += P; }

		while (T < TotalSeconds)
		{
			// Locate the phase and the elapsed time within it.
			int Phase = 0;
			double PhaseStart = 0.0;
			while (Phase < 3 && T >= PhaseStart + kPhaseSeconds[Phase])
			{
				PhaseStart += kPhaseSeconds[Phase];
				++Phase;
			}
			const double U = (T - PhaseStart) / kPhaseSeconds[Phase]; // 0..1 within the phase

			if (Phase != ReportedPhase)
			{
				ReportedPhase = Phase;
				std::printf("[fake_sender --carve] %s\n", kPhaseLabels[Phase]);
				std::fflush(stdout);
			}

			double SpeedMs = 0.0;
			double LateralM = 0.0;
			switch (Phase)
			{
				case 0: SpeedMs = 6.0 * U; break;
				case 1: SpeedMs = 3.0; LateralM = 0.055 * std::sin(U * 2.0 * 2.0 * M_PI); break;
				case 2: SpeedMs = 6.0 - 8.0 * U; break;
				default: SpeedMs = -2.0 + 2.0 * U; break;
			}

			PosX += SpeedMs * Dt;
			WheelAngleRad += (SpeedMs / kWheelRadiusM) * Dt;

			FBoardState State = BaseState(Seq++, T);
			State.Pos[0] = static_cast<float>(PosX);
			State.WheelRateRadS = static_cast<float>(SpeedMs / kWheelRadiusM);
			State.WheelAngleRad = static_cast<float>(WheelAngleRad);
			// Overwrite whatever --rider set: this mode owns the rider channel by construction,
			// so `--carve --rider` is not a contradiction that silently produces a frozen sweep.
			State.RiderLateralM = static_cast<float>(LateralM);
			State.RiderForeAftM = static_cast<float>(0.03 * (SpeedMs / 6.0));
			SendState(Sock, Dest, State);

			std::this_thread::sleep_for(std::chrono::milliseconds(kSendIntervalMs));
			T += Dt;
		}

		std::printf("[fake_sender --carve] done -- %llu packets over %.1f s\n",
			static_cast<unsigned long long>(Seq), TotalSeconds);
		close(Sock);
	}

	void RunPositionSweep(int Count)
	{
		sockaddr_in Dest;
		int Sock = OpenSocketToHost(Dest);
		if (Sock < 0) { return; }

		for (int i = 0; i < Count; ++i)
		{
			FBoardState State = BaseState(static_cast<uint64_t>(i), i * (kSendIntervalMs / 1000.0));
			State.Pos[0] = 0.01f * i; // sweep X so a receiver can visibly see motion
			SendState(Sock, Dest, State);
			std::this_thread::sleep_for(std::chrono::milliseconds(kSendIntervalMs));
		}
		std::printf("sent %d OBW1 packets to 127.0.0.1:9601\n", Count);
		close(Sock);
	}

	// Mimics the real host's measured cadence: a burst of kBurstSize packets sent with no
	// pacing delay between them, then one kBurstGapMs idle stall, repeated. Real numbers this is
	// modelling (measured on the actual Controls host, reproduces on an idle machine -- macOS
	// timer coalescing quantizing short sleeps, structural not a bug): mean inter-packet
	// ~1.97ms (~507Hz aggregate), p50 ~0.09ms, p99 ~15.03ms, max ~17.64ms. 8 packets back-to-back
	// then a 16ms gap reproduces that shape closely enough to exercise the same drain path.
	void RunBurst(int NumBursts)
	{
		constexpr int kBurstSize = 8;
		constexpr int kBurstGapMs = 16;

		sockaddr_in Dest;
		int Sock = OpenSocketToHost(Dest);
		if (Sock < 0) { return; }

		uint64_t Seq = 0;
		double SimTimeS = 0.0;
		for (int b = 0; b < NumBursts; ++b)
		{
			for (int i = 0; i < kBurstSize; ++i)
			{
				FBoardState State = BaseState(Seq++, SimTimeS);
				SimTimeS += 0.002; // ~500Hz nominal spacing within a burst
				State.Pos[0] = 0.01f * static_cast<float>(Seq);
				SendState(Sock, Dest, State);
				// Deliberately no sleep here -- back-to-back, matching the real host's burst.
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(kBurstGapMs));
		}
		std::printf("sent %d bursts of %d OBW1 packets (%llu total) to 127.0.0.1:9601\n",
			NumBursts, kBurstSize, static_cast<unsigned long long>(Seq));
		close(Sock);
	}

	void RunBadMagic()
	{
		sockaddr_in Dest;
		int Sock = OpenSocketToHost(Dest);
		if (Sock < 0) { return; }

		FBoardState State = BaseState(0, 0.0);
		uint8_t Buf[kStatePacketWireSizeV2]; // BaseState() defaults to v2 (kStateSchemaVersionLatest)
		EncodeBoardState(State, Buf);
		Buf[0] = 0xDE; // corrupt the magic byte
		std::printf("sending 1 corrupt-magic packet\n");
		ssize_t Sent = sendto(Sock, Buf, sizeof(Buf), 0, reinterpret_cast<sockaddr*>(&Dest), sizeof(Dest));
		if (Sent != static_cast<ssize_t>(sizeof(Buf)))
		{
			std::perror("sendto");
		}
		close(Sock);
	}

	void RunRotationSequence()
	{
		sockaddr_in Dest;
		int Sock = OpenSocketToHost(Dest);
		if (Sock < 0) { return; }

		const double HeldAngleRad = 25.0 * M_PI / 180.0; // ~25 degrees -- visible, not extreme
		const double SweepAmplitudeRad = HeldAngleRad;
		const double SweepPeriodSeconds = 3.0; // one full up/down cycle every 3s

		// Kind-tagged steps rather than function pointers, so each step can close over
		// HeldAngleRad/SweepAmplitudeRad without the non-capturing-lambda-as-function-pointer
		// restriction getting in the way.
		struct FStep { const char* Label; int DurationMs; int Kind; };
		// Kind: 0 = level, 1 = nose up, 2 = nose down, 3 = yaw left, 4 = roll right, 5 = pitch sweep
		const std::vector<FStep> Steps = {
			{"level (baseline)", 1000, 0},
			{"NOSE UP (+25 deg) -- forward edge should lift, board tips back", 2000, 1},
			{"level", 500, 0},
			{"NOSE DOWN (-25 deg) -- forward edge should drop, board tips forward", 2000, 2},
			{"level", 500, 0},
			{"YAW LEFT (+25 deg) -- nose should swing toward the rider's left", 2000, 3},
			{"level", 500, 0},
			{"ROLL RIGHT (+25 deg) -- right edge should dip toward the ground", 2000, 4},
			{"level", 500, 0},
			{"slow continuous pitch sweep (~9s, +-25 deg, nose up/down)", 9000, 5},
		};

		uint64_t Seq = 0;
		double SimTimeS = 0.0;
		for (const FStep& Step : Steps)
		{
			std::printf("[fake_sender --rotate] %s\n", Step.Label);
			const int NumPackets = Step.DurationMs / kSendIntervalMs;
			for (int i = 0; i < NumPackets; ++i)
			{
				const double ElapsedS = i * (kSendIntervalMs / 1000.0);
				FBoardState State = BaseState(Seq++, SimTimeS);
				SimTimeS += kSendIntervalMs / 1000.0;

				switch (Step.Kind)
				{
					case 1: SetNoseUp(State, HeldAngleRad); break;
					case 2: SetNoseUp(State, -HeldAngleRad); break; // nose-down is nose-up by a negative angle
					case 3: SetYawLeft(State, HeldAngleRad); break;
					case 4: SetRollRight(State, HeldAngleRad); break;
					case 5:
					{
						const double Phi = SweepAmplitudeRad * std::sin(2.0 * M_PI * ElapsedS / SweepPeriodSeconds);
						SetNoseUp(State, Phi);
						break;
					}
					default: break; // level: identity quat from BaseState
				}

				SendState(Sock, Dest, State);
				std::this_thread::sleep_for(std::chrono::milliseconds(kSendIntervalMs));
			}
		}

		std::printf("[fake_sender --rotate] done\n");
		close(Sock);
	}
}

int main(int argc, char** argv)
{
	bool BadMagic = false;
	bool Rotate = false;
	bool Burst = false;
	bool AuthorityCliff = false;
	bool Carve = false;
	int Count = 50;

	for (int i = 1; i < argc; ++i)
	{
		std::string Arg = argv[i];
		if (Arg == "--bad-magic")
		{
			BadMagic = true;
		}
		else if (Arg == "--rotate")
		{
			Rotate = true;
		}
		else if (Arg == "--burst")
		{
			Burst = true;
		}
		else if (Arg == "--count" && i + 1 < argc)
		{
			Count = std::atoi(argv[++i]);
		}
		else if (Arg == "--rider")
		{
			gRiderTest = true;
		}
		else if (Arg == "--authority-cliff")
		{
			AuthorityCliff = true;
		}
		else if (Arg == "--carve")
		{
			Carve = true;
		}
	}

	if (Carve)
	{
		RunCarveSweep();
	}
	else if (AuthorityCliff)
	{
		RunAuthorityCliff();
	}
	else if (Rotate)
	{
		RunRotationSequence();
	}
	else if (Burst)
	{
		RunBurst(Count);
	}
	else if (BadMagic)
	{
		RunBadMagic();
	}
	else
	{
		RunPositionSweep(Count);
	}

	return 0;
}
