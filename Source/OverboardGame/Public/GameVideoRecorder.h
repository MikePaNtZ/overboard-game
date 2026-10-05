// GameVideoRecorder -- records the game viewport, HUD included, to an MP4 (-ObRecordVideo=<path>).
//
// Uses the engine's asynchronous FFrameGrabber (GPU readback over a ring of surfaces, no stall of
// the game thread) and pipes the raw frames into ffmpeg. Frames are taken on a wall-clock grid
// at a fixed rate; when the game misses a slot, the last frame repeats, so the video keeps real
// time. A demo/dev tool only: it reads pixels and touches nothing in the game.

#pragma once

#include "CoreMinimal.h"

#include "FrameGrabber.h"

class OVERBOARDGAME_API FGameVideoRecorder
{
public:
	~FGameVideoRecorder();

	bool Start(const FString& OutPath, float Fps);
	void Tick();   // call once per frame
	void Finish(); // flush and close the file

private:
	TUniquePtr<FFrameGrabber> Grabber;
	FILE* Pipe = nullptr;
	FIntPoint Size = FIntPoint::ZeroValue;
	double StartSeconds = 0.0;
	double Fps = 20.0;
	int64 FramesWritten = 0;
	TArray<FColor> LastFrame;

	void WriteUpTo(int64 SlotIndex);
};
