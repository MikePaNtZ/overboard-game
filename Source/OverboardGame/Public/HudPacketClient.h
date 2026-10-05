// HudPacketClient.h
//
// UDP receiver for the optional HUD telemetry packet (OBHD). sim-host sends this second packet
// when it runs with --hud-out-addr; it carries what a rider's display shows and the state-out
// wire does not: battery charge, pack voltage and the authority margin. The layout mirrors
// crates/sim-host/src/hud.rs (magic 'OBHD', schema 1, 56 bytes, little-endian).
//
// Runs its own background thread (FRunnable), the same pattern as FBoardStateClient, so a slow or
// absent host never blocks the game thread. It keeps only the newest valid packet plus the wall
// clock time it arrived, so the HUD can treat an old packet as stale.
#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeBool.h"

class FSocket;

// One decoded HUD packet. Field order and units match hud.rs HudOut.
struct FHudPacket
{
	uint16 Flags = 0;         // bits 0-1 warning level; the HUD reads the board's own flags, not these
	uint64 Seq = 0;
	double TimeS = 0.0;
	float SpeedMS = 0.f;      // board speed, m/s
	float CurrentA = 0.f;     // motor current, A
	float TorqueNm = 0.f;     // motor torque, N.m
	float TorqueLimitNm = 0.f;// motor torque limit, N.m
	float BattSoc = 0.f;      // battery charge, 0..1
	float BattV = 0.f;        // pack voltage, V
	float BattIA = 0.f;       // pack current, A
	float Margin = 0.f;       // authority used, 0..1 (can exceed 1)
};

class OVERBOARDGAME_API FHudPacketClient : public FRunnable
{
public:
	FHudPacketClient();
	virtual ~FHudPacketClient() override;

	// Binds 127.0.0.1:9603 and starts the background receive thread. Safe to call once.
	bool StartListening();

	// Signals the thread to stop, joins it, and closes the socket. Safe to call from the game
	// thread (e.g. HUD EndPlay); blocks until the receive thread has exited.
	void Shutdown();

	// Copies out the newest valid packet and its arrival time (FPlatformTime::Seconds()). Returns
	// false before any valid packet has arrived, so the HUD hides the battery row and skips the
	// margin-edge warming until a real source exists.
	bool GetLatest(FHudPacket& OutPacket, double& OutArrivalSeconds) const;

	// FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	FSocket* Socket = nullptr;
	FRunnableThread* Thread = nullptr;
	FThreadSafeBool bRequestStop{false};

	mutable FCriticalSection Lock;
	FHudPacket Latest;
	double LatestArrivalSeconds = 0.0;
	bool bHaveLatest = false;
};
