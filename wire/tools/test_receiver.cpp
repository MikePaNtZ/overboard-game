// test_receiver: binds/listens on 127.0.0.1:9601 exactly like the real UE client will, decodes
// each datagram through the same OverboardWire::DecodeBoardState used by the game, and prints
// what it got (or the loud failure message on a bad packet). Proves the receive+parse path
// end-to-end over a real socket, standalone, without the editor.
//
// Usage: test_receiver [--count N]   (default: run until 20 packets received or 5s idle)
#include "../OverboardWire.h"
#include "../CoordinateTransform.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace OverboardWire;

int main(int argc, char** argv)
{
	int WantCount = 20;
	for (int i = 1; i < argc; ++i)
	{
		if (std::string(argv[i]) == "--count" && i + 1 < argc)
		{
			WantCount = std::atoi(argv[++i]);
		}
	}

	int Sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (Sock < 0)
	{
		std::perror("socket");
		return 1;
	}

	sockaddr_in Addr{};
	Addr.sin_family = AF_INET;
	Addr.sin_port = htons(9601);
	Addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(Sock, reinterpret_cast<sockaddr*>(&Addr), sizeof(Addr)) < 0)
	{
		std::perror("bind 127.0.0.1:9601");
		close(Sock);
		return 1;
	}

	// 5 second idle timeout so this doesn't hang forever if nothing shows up.
	timeval Tv{};
	Tv.tv_sec = 5;
	setsockopt(Sock, SOL_SOCKET, SO_RCVTIMEO, &Tv, sizeof(Tv));

	std::printf("test_receiver: listening on 127.0.0.1:9601\n");

	int Received = 0;
	int Dropped = 0;
	while (Received < WantCount)
	{
		uint8_t Buf[512];
		ssize_t N = recv(Sock, Buf, sizeof(Buf), 0);
		if (N < 0)
		{
			std::printf("test_receiver: idle timeout, stopping (received=%d dropped=%d)\n", Received, Dropped);
			break;
		}

		FBoardState State;
		std::string Err;
		if (!DecodeBoardState(Buf, static_cast<size_t>(N), State, Err))
		{
			std::printf("DROP: %s\n", Err.c_str());
			++Dropped;
			continue;
		}

		FUeTransform Ue = MuJoCoToUnreal(State.Pos, State.Quat);
		std::printf(
			"seq=%llu t=%.3f pos_m=(%.3f,%.3f,%.3f) -> pos_cm_ue=(%.1f,%.1f,%.1f)\n",
			static_cast<unsigned long long>(State.Seq), State.SimTimeS,
			State.Pos[0], State.Pos[1], State.Pos[2],
			Ue.PosCm[0], Ue.PosCm[1], Ue.PosCm[2]);
		++Received;
	}

	close(Sock);
	return 0;
}
