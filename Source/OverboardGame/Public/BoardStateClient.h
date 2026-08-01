// BoardStateClient.h
//
// UDP receiver for the OBW1 state stream. Runs its own background thread (FRunnable) so a slow
// or absent host never blocks the game thread -- the game thread only ever takes a short lock
// to read out whatever has arrived so far.
//
// Board actor consumes GetHistorySnapshot() and interpolates ITSELF between two already-received
// samples, one render-delay behind the wall clock. This class never extrapolates; it only ever
// hands back samples it actually received.
#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeBool.h"
#include "OverboardWire.h"

class FSocket;

// One decoded state plus the wall-clock time it arrived, for interpolation.
struct FTimestampedBoardState
{
	double ArrivalTimeSeconds = 0.0;
	OverboardWire::FBoardState State;
};

class OVERBOARDGAME_API FBoardStateClient : public FRunnable
{
public:
	FBoardStateClient();
	virtual ~FBoardStateClient() override;

	// Binds 127.0.0.1:9601 and starts the background receive thread. Safe to call once.
	bool StartListening();

	// Signals the thread to stop, joins it, and closes the socket. Safe to call from the game
	// thread (e.g. actor EndPlay); blocks until the receive thread has exited.
	void Shutdown();

	// Copies out the last few received samples, oldest first, up to MaxSamples. Never blocks
	// the caller for more than the time it takes to copy a handful of small structs.
	void GetHistorySnapshot(TArray<FTimestampedBoardState>& OutHistory, int32 MaxSamples = 4) const;

	// FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override; // called by FRunnableThread::Kill(); signals Run() to exit

private:
	FSocket* Socket = nullptr;
	FRunnableThread* Thread = nullptr;
	FThreadSafeBool bRequestStop{false};

	mutable FCriticalSection HistoryLock;
	TArray<FTimestampedBoardState> History; // ring-ish; trimmed to a small max in Run()

	static constexpr int32 kMaxHistory = 8;
};
