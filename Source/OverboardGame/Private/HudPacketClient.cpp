#include "HudPacketClient.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Common/UdpSocketBuilder.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogOverboardHud, Log, All);

namespace
{
	constexpr int32 kListenPort = 9603;      // host SENDS the HUD packet here, so we BIND/LISTEN
	constexpr int32 kRecvBufferBytes = 256;  // the HUD packet is 56 bytes; generous headroom

	// Mirrors hud.rs: HUD_MAGIC = b"OBHD" read little-endian, HUD_SCHEMA_VERSION = 1, 56 bytes.
	constexpr uint32 kHudMagic = static_cast<uint32>('O') | (static_cast<uint32>('B') << 8)
		| (static_cast<uint32>('H') << 16) | (static_cast<uint32>('D') << 24);
	constexpr uint16 kHudSchemaVersion = 1;
	constexpr int32 kHudWireSize = 56;

	// Reads one little-endian value at a byte offset. The host is little-endian and so is every
	// platform this game runs on, so a plain copy is correct; the explicit offsets keep the layout
	// in step with the table in hud.rs.
	template <typename T>
	T ReadLe(const uint8* Data, int32 Offset)
	{
		T Value;
		FMemory::Memcpy(&Value, Data + Offset, sizeof(T));
		return Value;
	}
}

FHudPacketClient::FHudPacketClient() = default;

FHudPacketClient::~FHudPacketClient()
{
	Shutdown();
}

bool FHudPacketClient::StartListening()
{
	if (Thread != nullptr)
	{
		return true; // already running
	}

	Socket = FUdpSocketBuilder(TEXT("OverboardHudSocket"))
		.AsNonBlocking()
		.AsReusable()
		.BoundToAddress(FIPv4Address(127, 0, 0, 1))
		.BoundToPort(kListenPort)
		.WithReceiveBufferSize(64 * 1024)
		.Build();

	if (Socket == nullptr)
	{
		UE_LOG(LogOverboardHud, Error, TEXT("HudPacketClient: failed to bind 127.0.0.1:%d"), kListenPort);
		return false;
	}

	bRequestStop = false;
	Thread = FRunnableThread::Create(this, TEXT("OverboardHudClientThread"));
	if (Thread == nullptr)
	{
		UE_LOG(LogOverboardHud, Error, TEXT("HudPacketClient: failed to start receive thread"));
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
		return false;
	}

	UE_LOG(LogOverboardHud, Log, TEXT("HudPacketClient: listening on 127.0.0.1:%d"), kListenPort);
	return true;
}

void FHudPacketClient::Shutdown()
{
	Stop();
	if (Thread != nullptr)
	{
		Thread->Kill(true /* wait for completion */);
		delete Thread;
		Thread = nullptr;
	}
	if (Socket != nullptr)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
	}
}

void FHudPacketClient::Stop()
{
	bRequestStop = true;
}

uint32 FHudPacketClient::Run()
{
	TArray<uint8> Buf;
	Buf.SetNumUninitialized(kRecvBufferBytes);

	while (!bRequestStop)
	{
		if (Socket == nullptr)
		{
			break;
		}

		// Non-blocking socket plus a short Wait, so the thread neither spin-burns a core nor
		// blocks long enough to delay a Stop() request -- the same pattern as FBoardStateClient.
		const bool bHasData = Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(50));
		if (!bHasData)
		{
			continue;
		}

		int32 BytesRead = 0;
		TSharedRef<FInternetAddr> Sender = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
		if (!Socket->RecvFrom(Buf.GetData(), Buf.Num(), BytesRead, *Sender))
		{
			continue;
		}

		// Fail quietly on a short or wrong packet: drop it and keep the newest valid one.
		if (BytesRead < kHudWireSize)
		{
			continue;
		}
		const uint8* Data = Buf.GetData();
		if (ReadLe<uint32>(Data, 0) != kHudMagic || ReadLe<uint16>(Data, 4) != kHudSchemaVersion)
		{
			continue;
		}

		FHudPacket Packet;
		Packet.Flags = ReadLe<uint16>(Data, 6);
		Packet.Seq = ReadLe<uint64>(Data, 8);
		Packet.TimeS = ReadLe<double>(Data, 16);
		Packet.SpeedMS = ReadLe<float>(Data, 24);
		Packet.CurrentA = ReadLe<float>(Data, 28);
		Packet.TorqueNm = ReadLe<float>(Data, 32);
		Packet.TorqueLimitNm = ReadLe<float>(Data, 36);
		Packet.BattSoc = ReadLe<float>(Data, 40);
		Packet.BattV = ReadLe<float>(Data, 44);
		Packet.BattIA = ReadLe<float>(Data, 48);
		Packet.Margin = ReadLe<float>(Data, 52);

		FScopeLock ScopeLock(&Lock);
		Latest = Packet;
		LatestArrivalSeconds = FPlatformTime::Seconds();
		bHaveLatest = true;
	}

	return 0;
}

bool FHudPacketClient::GetLatest(FHudPacket& OutPacket, double& OutArrivalSeconds) const
{
	FScopeLock ScopeLock(&Lock);
	if (!bHaveLatest)
	{
		return false;
	}
	OutPacket = Latest;
	OutArrivalSeconds = LatestArrivalSeconds;
	return true;
}
