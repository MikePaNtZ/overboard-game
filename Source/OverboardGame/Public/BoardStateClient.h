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

	// Copies out the retained samples, oldest first, up to MaxSamples (default generous enough
	// to return the whole retention window -- see kHistoryRetentionSeconds). Never blocks the
	// caller for more than the time it takes to copy a handful of small structs.
	void GetHistorySnapshot(TArray<FTimestampedBoardState>& OutHistory, int32 MaxSamples = 256) const;

	// FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override; // called by FRunnableThread::Kill(); signals Run() to exit

private:
	FSocket* Socket = nullptr;
	FRunnableThread* Thread = nullptr;
	FThreadSafeBool bRequestStop{false};

	mutable FCriticalSection HistoryLock;
	TArray<FTimestampedBoardState> History; // trimmed by age (and a hard cap) in Run()

	// The real host does not send at a smooth cadence: measured on the actual Controls host,
	// mean inter-packet interval ~2ms (~500 Hz aggregate) but p99 ~15ms / max ~17.6ms -- macOS
	// timer coalescing quantizes short sleeps, so packets arrive in bursts of roughly eight with
	// a ~15-17ms stall between bursts (structural, reproduces on an idle machine; see overboard#162
	// discussion). A history trimmed to a small fixed *count* (the original value here was 8)
	// can easily hold less than one render-delay's worth of real time when a burst lands --
	// exactly the scenario that matters. Trim by AGE instead, generously past the render delay
	// actors are expected to use (default 0.05s, see ABoardActor::RenderDelaySeconds), so the
	// interpolation bracket in ABoardActor::UpdatePoseFromHistory can always find two real
	// samples instead of falling back to "hold the oldest we've got".
	static constexpr double kHistoryRetentionSeconds = 0.25; // 5x the current default render delay
	static constexpr int32 kHistoryHardCap = 512; // safety net against unbounded growth only
};
