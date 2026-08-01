// fake_sender: emits real OBW1 packets over UDP to 127.0.0.1:9601, standing in for the
// controls host so the receive+decode path can be proven end-to-end without the real host
// (which the Controls session is building against ADR-0010 right now and cannot hand us).
//
// Usage:
//   fake_sender                 sends N well-formed packets, pos sweeping in X, one per 20ms
//   fake_sender --bad-magic     sends one corrupt-magic packet, to prove the receiver drops it
//   fake_sender --count N       override packet count (default 50)
//
// macOS/BSD sockets only, no UE dependency.
#include "../OverboardWire.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace OverboardWire;

int main(int argc, char** argv)
{
	bool BadMagic = false;
	int Count = 50;

	for (int i = 1; i < argc; ++i)
	{
		std::string Arg = argv[i];
		if (Arg == "--bad-magic")
		{
			BadMagic = true;
		}
		else if (Arg == "--count" && i + 1 < argc)
		{
			Count = std::atoi(argv[++i]);
		}
	}

	int Sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (Sock < 0)
	{
		std::perror("socket");
		return 1;
	}

	sockaddr_in Dest{};
	Dest.sin_family = AF_INET;
	Dest.sin_port = htons(9601);
	inet_pton(AF_INET, "127.0.0.1", &Dest.sin_addr);

	for (int i = 0; i < Count; ++i)
	{
		FBoardState State;
		State.Flags = EStateFlags::Armed | EStateFlags::Valid;
		State.Seq = static_cast<uint64_t>(i);
		State.SimTimeS = i * 0.02;
		// Sweep X so a receiver can visibly see motion; everything else held simple.
		State.Pos[0] = 0.01f * i;
		State.Pos[1] = 0.f;
		State.Pos[2] = 0.5f;
		State.Quat[0] = 1.f; State.Quat[1] = 0.f; State.Quat[2] = 0.f; State.Quat[3] = 0.f;
		State.WheelAngleRad = 0.f;
		State.WheelRateRadS = 0.f;
		State.PitchRad = 0.f;
		State.YawRad = 0.f;
		State.MotorCurrentA = 0.f;

		uint8_t Buf[kStatePacketWireSize];
		EncodeBoardState(State, Buf);

		if (BadMagic && i == 0)
		{
			Buf[0] = 0xDE; // corrupt the magic byte
			std::printf("sending 1 corrupt-magic packet\n");
		}

		ssize_t Sent = sendto(Sock, Buf, sizeof(Buf), 0, reinterpret_cast<sockaddr*>(&Dest), sizeof(Dest));
		if (Sent != static_cast<ssize_t>(sizeof(Buf)))
		{
			std::perror("sendto");
		}

		if (BadMagic)
		{
			break; // one corrupt packet is the whole point of this mode
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	if (!BadMagic)
	{
		std::printf("sent %d OBW1 packets to 127.0.0.1:9601\n", Count);
	}

	close(Sock);
	return 0;
}
