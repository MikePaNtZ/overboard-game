#include "MovingObjectsClient.h"
#include "OverboardPorts.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Common/UdpSocketBuilder.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogOverboardObjects, Log, All);

namespace
{
	// The host SENDS here, so we BIND/LISTEN: OverboardPorts::Objects() (-ObPortBase + 4).
	constexpr int32 kObjsRecvBufferBytes = 2048; // 24 + 64 * 20 = 1304 bytes max; generous headroom

	// Mirrors the OBJS wire: magic 'OBO1' read little-endian, version 1, 24-byte header, 20 bytes
	// per object. See the task contract and crates/sim-host object output.
	constexpr uint32 kObjectsMagic = static_cast<uint32>('O') | (static_cast<uint32>('B') << 8)
		| (static_cast<uint32>('O') << 16) | (static_cast<uint32>('1') << 24);
	constexpr uint16 kObjectsVersion = 1;
	constexpr int32 kHeaderBytes = 24;
	constexpr int32 kObjectBytes = 20;
	constexpr int32 kMaxObjects = 64;

	// Reads one little-endian value at a byte offset. Every platform this game runs on is
	// little-endian, so a plain copy is correct; the explicit offsets keep the layout in step
	// with the wire contract.
	template <typename T>
	// Named ReadObjsLe, not ReadLe: in a unity build this file can share a translation unit with
	// HudPacketClient.cpp, which has its own ReadLe (a redefinition error on some machines).
	T ReadObjsLe(const uint8* Data, int32 Offset)
	{
		T Value;
		FMemory::Memcpy(&Value, Data + Offset, sizeof(T));
		return Value;
	}
}

FMovingObjectsClient::FMovingObjectsClient() = default;

FMovingObjectsClient::~FMovingObjectsClient()
{
	Shutdown();
}

bool FMovingObjectsClient::StartListening()
{
	if (Thread != nullptr)
	{
		return true; // already running
	}

	Socket = FUdpSocketBuilder(TEXT("OverboardObjectsSocket"))
		.AsNonBlocking()
		.AsReusable()
		.BoundToAddress(FIPv4Address(127, 0, 0, 1))
		.BoundToPort(OverboardPorts::Objects())
		.WithReceiveBufferSize(256 * 1024)
		.Build();

	if (Socket == nullptr)
	{
		UE_LOG(LogOverboardObjects, Error, TEXT("MovingObjectsClient: failed to bind 127.0.0.1:%d"), OverboardPorts::Objects());
		return false;
	}

	bRequestStop = false;
	Thread = FRunnableThread::Create(this, TEXT("OverboardObjectsClientThread"));
	if (Thread == nullptr)
	{
		UE_LOG(LogOverboardObjects, Error, TEXT("MovingObjectsClient: failed to start receive thread"));
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
		return false;
	}

	UE_LOG(LogOverboardObjects, Log, TEXT("MovingObjectsClient: listening on 127.0.0.1:%d"), OverboardPorts::Objects());
	return true;
}

void FMovingObjectsClient::Shutdown()
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

void FMovingObjectsClient::Stop()
{
	bRequestStop = true;
}

uint32 FMovingObjectsClient::Run()
{
	TArray<uint8> Buf;
	Buf.SetNumUninitialized(kObjsRecvBufferBytes);

	while (!bRequestStop)
	{
		if (Socket == nullptr)
		{
			break;
		}

		// Non-blocking socket plus a short Wait, the same pattern as FBoardStateClient: neither
		// spin-burn a core nor delay noticing a Stop() request.
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

		const uint8* Data = Buf.GetData();
		if (BytesRead < kHeaderBytes || ReadObjsLe<uint32>(Data, 0) != kObjectsMagic || ReadObjsLe<uint16>(Data, 4) != kObjectsVersion)
		{
			continue; // drop a short or wrong packet, keep the newest valid frame
		}

		const int32 Count = FMath::Min(static_cast<int32>(ReadObjsLe<uint16>(Data, 6)), kMaxObjects);
		if (BytesRead < kHeaderBytes + Count * kObjectBytes)
		{
			UE_LOG(LogOverboardObjects, Warning, TEXT("MovingObjectsClient: short OBJS packet (%d bytes, %d objects); dropping"), BytesRead, Count);
			continue;
		}

		FMovingObjectsFrame Frame;
		Frame.ArrivalTimeSeconds = FPlatformTime::Seconds();
		Frame.Seq = ReadObjsLe<uint64>(Data, 8);
		Frame.SimTime = ReadObjsLe<double>(Data, 16);
		Frame.Objects.Reserve(Count);
		for (int32 i = 0; i < Count; ++i)
		{
			const int32 Off = kHeaderBytes + i * kObjectBytes;
			FMovingObjectSample S;
			S.Id = ReadObjsLe<uint16>(Data, Off + 0);
			S.Kind = Data[Off + 2];
			S.Flags = Data[Off + 3];
			S.Pos[0] = ReadObjsLe<float>(Data, Off + 4);
			S.Pos[1] = ReadObjsLe<float>(Data, Off + 8);
			S.Pos[2] = ReadObjsLe<float>(Data, Off + 12);
			S.Yaw = ReadObjsLe<float>(Data, Off + 16);
			Frame.Objects.Add(S);
		}

		FScopeLock Lock(&HistoryLock);

		// Rising edge of any object's touching bit (bit 0), measured here where every frame is
		// seen, not on the game thread where frames are skipped.
		for (const FMovingObjectSample& S : Frame.Objects)
		{
			const bool bTouchingNow = (S.Flags & 0x1) != 0;
			const bool bTouchingWas = LastTouching.FindRef(S.Id);
			if (bTouchingNow && !bTouchingWas)
			{
				LastContactRiseSeconds = Frame.ArrivalTimeSeconds;
				++ContactRiseCount;
			}
			LastTouching.Add(S.Id, bTouchingNow);
		}

		History.Add(MoveTemp(Frame));

		// Trim by age (plus a hard cap), the same reasoning as FBoardStateClient.
		while (History.Num() > kHistoryHardCap)
		{
			History.RemoveAt(0);
		}
		const double NewestArrival = History.Last().ArrivalTimeSeconds;
		while (History.Num() > 1 && (NewestArrival - History[0].ArrivalTimeSeconds) > kHistoryRetentionSeconds)
		{
			History.RemoveAt(0);
		}
	}

	return 0;
}

void FMovingObjectsClient::GetHistorySnapshot(TArray<FMovingObjectsFrame>& OutHistory) const
{
	FScopeLock Lock(&HistoryLock);
	OutHistory = History;
}

bool FMovingObjectsClient::GetLatestFrame(FMovingObjectsFrame& OutFrame) const
{
	FScopeLock Lock(&HistoryLock);
	if (History.Num() == 0)
	{
		return false;
	}
	OutFrame = History.Last();
	return true;
}

bool FMovingObjectsClient::GetObjectVelocities(TArray<FMovingObjectVel>& OutObjects) const
{
	// Robust velocity: trust a two-sample estimate only when the samples are close in time and in
	// distance. A larger gap means a dropped-packet burst, a path wrap, or a respawn; in that case
	// the demo rider treats the object as unknown and errs toward holding.
	constexpr double kMaxVelDtSeconds = 0.1;   // two OBJS frames at 50 Hz are 0.02 s apart
	constexpr double kMaxVelStepMetres = 1.5;  // 1.5 m in 0.1 s is 15 m/s, above any street car

	FScopeLock Lock(&HistoryLock);
	if (History.Num() == 0)
	{
		return false;
	}
	const FMovingObjectsFrame& Newest = History.Last();
	const FMovingObjectsFrame* Prev = History.Num() >= 2 ? &History[History.Num() - 2] : nullptr;
	const double Dt = Prev ? (Newest.ArrivalTimeSeconds - Prev->ArrivalTimeSeconds) : 0.0;

	OutObjects.Reset();
	OutObjects.Reserve(Newest.Objects.Num());
	for (const FMovingObjectSample& S : Newest.Objects)
	{
		FMovingObjectVel V;
		V.Id = S.Id;
		V.Kind = S.Kind;
		V.X = S.Pos[0];
		V.Y = S.Pos[1];
		V.Yaw = S.Yaw;
		if (Prev && Dt > 0.0 && Dt <= kMaxVelDtSeconds)
		{
			for (const FMovingObjectSample& P : Prev->Objects)
			{
				if (P.Id != S.Id) { continue; }
				const double Dx = static_cast<double>(S.Pos[0]) - P.Pos[0];
				const double Dy = static_cast<double>(S.Pos[1]) - P.Pos[1];
				if (FMath::Sqrt(Dx * Dx + Dy * Dy) <= kMaxVelStepMetres)
				{
					V.Vx = Dx / Dt;
					V.Vy = Dy / Dt;
					V.bVelValid = true;
				}
				break;
			}
		}
		OutObjects.Add(V);
	}
	return true;
}

double FMovingObjectsClient::GetLastContactRiseSeconds() const
{
	FScopeLock Lock(&HistoryLock);
	return LastContactRiseSeconds;
}

int32 FMovingObjectsClient::GetContactRiseCount() const
{
	FScopeLock Lock(&HistoryLock);
	return ContactRiseCount;
}
