#include "BoardStateClient.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Common/UdpSocketBuilder.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogOverboardWire, Log, All);

namespace
{
	constexpr int32 kListenPort = 9601; // host SENDS here, so we BIND/LISTEN
	constexpr int32 kRecvBufferBytes = 512; // state packet is 72 bytes; generous headroom
}

FBoardStateClient::FBoardStateClient() = default;

FBoardStateClient::~FBoardStateClient()
{
	Shutdown();
}

bool FBoardStateClient::StartListening()
{
	if (Thread != nullptr)
	{
		return true; // already running
	}

	Socket = FUdpSocketBuilder(TEXT("OverboardStateSocket"))
		.AsNonBlocking()
		.AsReusable()
		.BoundToAddress(FIPv4Address(127, 0, 0, 1))
		.BoundToPort(kListenPort)
		.WithReceiveBufferSize(64 * 1024)
		.Build();

	if (Socket == nullptr)
	{
		UE_LOG(LogOverboardWire, Error, TEXT("BoardStateClient: failed to bind 127.0.0.1:%d"), kListenPort);
		return false;
	}

	bRequestStop = false;
	Thread = FRunnableThread::Create(this, TEXT("OverboardStateClientThread"));
	if (Thread == nullptr)
	{
		UE_LOG(LogOverboardWire, Error, TEXT("BoardStateClient: failed to start receive thread"));
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
		return false;
	}

	UE_LOG(LogOverboardWire, Log, TEXT("BoardStateClient: listening on 127.0.0.1:%d"), kListenPort);
	return true;
}

void FBoardStateClient::Shutdown()
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

void FBoardStateClient::Stop()
{
	bRequestStop = true;
}

uint32 FBoardStateClient::Run()
{
	TArray<uint8> Buf;
	Buf.SetNumUninitialized(kRecvBufferBytes);

	while (!bRequestStop)
	{
		if (Socket == nullptr)
		{
			break;
		}

		// Non-blocking socket + a short Wait so we neither spin-burn a core nor block the
		// thread indefinitely (which would delay noticing a Stop() request).
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

		OverboardWire::FBoardState Decoded;
		std::string Err;
		if (!OverboardWire::DecodeBoardState(Buf.GetData(), static_cast<size_t>(BytesRead), Decoded, Err))
		{
			// Fail loudly, never misparse a float: log and drop.
			UE_LOG(LogOverboardWire, Warning, TEXT("BoardStateClient: dropping packet: %s"), *FString(Err.c_str()));
			continue;
		}

		FTimestampedBoardState Sample;
		Sample.ArrivalTimeSeconds = FPlatformTime::Seconds();
		Sample.State = Decoded;

		FScopeLock Lock(&HistoryLock);
		History.Add(Sample);
		while (History.Num() > kMaxHistory)
		{
			History.RemoveAt(0);
		}
	}

	return 0;
}

void FBoardStateClient::GetHistorySnapshot(TArray<FTimestampedBoardState>& OutHistory, int32 MaxSamples) const
{
	FScopeLock Lock(&HistoryLock);
	const int32 Start = FMath::Max(0, History.Num() - MaxSamples);
	OutHistory.Reset();
	for (int32 i = Start; i < History.Num(); ++i)
	{
		OutHistory.Add(History[i]);
	}
}
