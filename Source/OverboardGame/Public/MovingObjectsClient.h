// MovingObjectsClient.h
//
// UDP receiver for the OBJS stream (Level 2 phase B traffic). sim-host sends this packet at 50 Hz
// when it runs with --objects-out-addr; it carries the pose of every moving object (car,
// pedestrian, cyclist) that MuJoCo scripts and moves. THE RULE (ADR-0009): Unreal computes no
// physics. sim-host owns every object path and every contact; this class only decodes the poses.
//
// Runs its own background thread (FRunnable), the same pattern as FBoardStateClient and
// FHudPacketClient, so a slow or absent host never blocks the game thread. It retains a short
// window of whole decoded frames, so AMovingObjectsActor can interpolate each object one
// render-delay behind the wall clock, exactly as ABoardActor does for the board.
#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeBool.h"

class FSocket;

// One object's pose, decoded from the 20-byte per-object record. Pos is the raw MuJoCo frame
// (metres, body centre); Yaw is radians about the MuJoCo world vertical.
struct FMovingObjectSample
{
	uint16 Id = 0;
	uint8 Kind = 0;      // 0 car, 1 pedestrian, 2 cyclist
	uint8 Flags = 0;     // bit 0 = touching the board or the rider
	float Pos[3] = {0.f, 0.f, 0.f};
	float Yaw = 0.f;
};

// One whole OBJS packet plus the wall-clock time it arrived, for interpolation.
struct FMovingObjectsFrame
{
	double ArrivalTimeSeconds = 0.0;
	double SimTime = 0.0;
	uint64 Seq = 0;
	TArray<FMovingObjectSample> Objects;
};

// One object's current MuJoCo-frame position and an estimated world velocity, for the demo
// rider's predictive give-way. Vel comes from the two newest OBJS frames. bVelValid is false
// when the two samples are too far apart in time or in distance (a path wrap or a respawn), so
// the rider never reads a bogus velocity as a reason to go.
struct FMovingObjectVel
{
	uint16 Id = 0;
	uint8 Kind = 0;      // 0 car, 1 pedestrian, 2 cyclist
	double X = 0.0;      // MuJoCo world x, metres
	double Y = 0.0;      // MuJoCo world y, metres
	double Vx = 0.0;     // metres/second
	double Vy = 0.0;
	double Yaw = 0.0;    // MuJoCo heading, radians (the +X body axis points along travel)
	bool bVelValid = false;
};

class OVERBOARDGAME_API FMovingObjectsClient : public FRunnable
{
public:
	FMovingObjectsClient();
	virtual ~FMovingObjectsClient() override;

	// Binds 127.0.0.1:OverboardPorts::Objects() and starts the background receive thread.
	bool StartListening();

	// Signals the thread to stop, joins it, and closes the socket. Safe from the game thread.
	void Shutdown();

	// Copies out the retained frames, oldest first. Never blocks for more than a short copy.
	void GetHistorySnapshot(TArray<FMovingObjectsFrame>& OutHistory) const;

	// Copies out the newest frame only. False before any valid packet has arrived. For the demo
	// rider's give-way (it needs the current positions, not a render-delayed pose).
	bool GetLatestFrame(FMovingObjectsFrame& OutFrame) const;

	// Fills OutObjects with every object in the newest frame, each with a velocity estimated from
	// the previous frame. bVelValid follows the robust-velocity rule (see FMovingObjectVel). False
	// before any frame has arrived. For the demo rider's predictive give-way.
	bool GetObjectVelocities(TArray<FMovingObjectVel>& OutObjects) const;

	// FPlatformTime::Seconds() at which an object's touching bit last went 0 -> 1 (any object),
	// and the running count of such rises. 0 / 0 before any contact. For the HUD "CONTACT" toast
	// and the test's contact count.
	double GetLastContactRiseSeconds() const;
	int32 GetContactRiseCount() const;

	// FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	FSocket* Socket = nullptr;
	FRunnableThread* Thread = nullptr;
	FThreadSafeBool bRequestStop{false};

	mutable FCriticalSection HistoryLock;
	TArray<FMovingObjectsFrame> History; // trimmed by age (and a hard cap) in Run()

	// Per-id touching bit from the previous frame, so Run() can find the 0 -> 1 rising edge.
	TMap<uint16, bool> LastTouching;
	double LastContactRiseSeconds = 0.0;
	int32 ContactRiseCount = 0;

	// Same retention reasoning as FBoardStateClient: trim by AGE, generously past the render
	// delay an actor uses, so the interpolation bracket can always find two real frames. The
	// OBJS stream is a steady 50 Hz (20 ms), so 0.25 s holds about 12 frames.
	static constexpr double kHistoryRetentionSeconds = 0.25;
	static constexpr int32 kHistoryHardCap = 64;
};
