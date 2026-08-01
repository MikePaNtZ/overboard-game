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
		uint8_t Buf[kStatePacketWireSize];
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
		return State;
	}

	// ---- The same physically-named attitudes wire/tests/test_wire.cpp asserts on
	// MuJoCoToUnreal(), so the on-screen check and the CI check are testing the same claims.
	// See the derivation comments there for why each is the raw MuJoCo quaternion it is.
	void SetNoseUp(FBoardState& S, double PhiRad)   { S.Quat[0] = static_cast<float>(std::cos(PhiRad / 2.0)); S.Quat[1] = 0.f; S.Quat[2] = static_cast<float>(-std::sin(PhiRad / 2.0)); S.Quat[3] = 0.f; }
	void SetYawLeft(FBoardState& S, double PhiRad)  { S.Quat[0] = static_cast<float>(std::cos(PhiRad / 2.0)); S.Quat[1] = 0.f; S.Quat[2] = 0.f; S.Quat[3] = static_cast<float>(std::sin(PhiRad / 2.0)); }
	void SetRollRight(FBoardState& S, double PhiRad){ S.Quat[0] = static_cast<float>(std::cos(PhiRad / 2.0)); S.Quat[1] = static_cast<float>(std::sin(PhiRad / 2.0)); S.Quat[2] = 0.f; S.Quat[3] = 0.f; }

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
		uint8_t Buf[kStatePacketWireSize];
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
	}

	if (Rotate)
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
