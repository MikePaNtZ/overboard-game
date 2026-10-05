// GameVideoRecorder -- records the game viewport, HUD included, to an MP4 (-ObRecordVideo=<path>).
//
// Uses the engine's asynchronous FFrameGrabber (GPU readback over a ring of surfaces, no stall of
// the game thread), scales to the video size on the GPU, and a writer thread pipes the raw frames
// into ffmpeg, which encodes on the Mac hardware encoder
// (VideoToolbox): software x264 at 1080p cost the game two thirds of its frame rate. Frames are taken on a wall-clock grid
// at a fixed rate; when the game misses a slot, the last frame repeats, so the video keeps real
// time. A demo/dev tool only: it reads pixels and touches nothing in the game.

#pragma once

#include "CoreMinimal.h"

#include "FrameGrabber.h"

#include <condition_variable>
#include <mutex>
#include <thread>

class OVERBOARDGAME_API FGameVideoRecorder
{
public:
	~FGameVideoRecorder();

	// OutSize: the video size. The GPU scales the viewport to it before readback, so a 1080p
	// game can record 720p cheaply.
	bool Start(const FString& OutPath, float Fps, FIntPoint OutSize);
	void Tick();   // call once per frame, on the game thread
	void Finish(); // flush and close the file

private:
	TUniquePtr<FFrameGrabber> Grabber;
	FILE* Pipe = nullptr;
	FIntPoint Size = FIntPoint::ZeroValue;
	double StartSeconds = 0.0;
	double Fps = 30.0;
	int64 FramesQueued = 0;
	TSharedPtr<TArray<FColor>, ESPMode::ThreadSafe> LastFrame;

	// The writer thread owns the pipe; the game thread only queues frames (a repeated frame is
	// the same shared buffer, queued again). Bounded: when the writer falls behind, the newest
	// frames are dropped and counted.
	std::thread Writer;
	std::mutex QueueMutex;
	std::condition_variable QueueCv;
	TArray<TSharedPtr<TArray<FColor>, ESPMode::ThreadSafe>> Queue;
	bool bStopWriter = false;
	int64 FramesDropped = 0;
	static constexpr int32 kMaxQueued = 90;

	void Enqueue(const TSharedPtr<TArray<FColor>, ESPMode::ThreadSafe>& Frame);
	void WriterLoop();
};
