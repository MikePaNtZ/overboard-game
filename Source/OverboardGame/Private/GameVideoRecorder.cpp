#include "GameVideoRecorder.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "FrameGrabber.h"
#include "HAL/PlatformTime.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"

#include <stdio.h>

DEFINE_LOG_CATEGORY_STATIC(LogGameVideo, Log, All);

FGameVideoRecorder::~FGameVideoRecorder()
{
	Finish();
}

bool FGameVideoRecorder::Start(const FString& OutPath, float InFps)
{
	if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->GetGameViewport())
	{
		UE_LOG(LogGameVideo, Error, TEXT("GameVideoRecorder: no game viewport."));
		return false;
	}
	FSceneViewport* Viewport = GEngine->GameViewport->GetGameViewport();
	Size = Viewport->GetSizeXY();
	Fps = FMath::Max(1.f, InFps);

	const FString Cmd = FString::Printf(
		TEXT("/opt/homebrew/bin/ffmpeg -y -loglevel error -f rawvideo -pix_fmt bgra -s %dx%d -r %.0f -i - ")
		TEXT("-c:v libx264 -pix_fmt yuv420p -crf 20 -movflags +faststart \"%s\""),
		Size.X, Size.Y, Fps, *OutPath);
	Pipe = popen(TCHAR_TO_UTF8(*Cmd), "w");
	if (!Pipe)
	{
		UE_LOG(LogGameVideo, Error, TEXT("GameVideoRecorder: could not start ffmpeg."));
		return false;
	}

	TSharedRef<FSceneViewport> ViewportRef = StaticCastSharedRef<FSceneViewport>(GEngine->GameViewport->GetGameViewportWidget()->GetViewportInterface().Pin().ToSharedRef());
	Grabber = MakeUnique<FFrameGrabber>(ViewportRef, Size, PF_B8G8R8A8, 4);
	Grabber->StartCapturingFrames();
	StartSeconds = FPlatformTime::Seconds();
	UE_LOG(LogGameVideo, Log, TEXT("GameVideoRecorder: recording %dx%d at %.0f fps to %s"), Size.X, Size.Y, Fps, *OutPath);
	return true;
}

void FGameVideoRecorder::WriteUpTo(int64 SlotIndex)
{
	// Repeat the last frame for every slot the game missed, then the new one is written by the caller.
	while (FramesWritten < SlotIndex && LastFrame.Num() > 0)
	{
		fwrite(LastFrame.GetData(), sizeof(FColor), LastFrame.Num(), Pipe);
		++FramesWritten;
	}
}

void FGameVideoRecorder::Tick()
{
	if (!Grabber || !Pipe)
	{
		return;
	}
	Grabber->CaptureThisFrame(FFramePayloadPtr());

	for (FCapturedFrameData& Frame : Grabber->GetCapturedFrames())
	{
		if (Frame.BufferSize != Size || Frame.ColorBuffer.Num() != Size.X * Size.Y)
		{
			continue;
		}
		const int64 Slot = static_cast<int64>((FPlatformTime::Seconds() - StartSeconds) * Fps);
		if (Slot < FramesWritten)
		{
			LastFrame = MoveTemp(Frame.ColorBuffer); // ahead of the grid: keep it for the next slot
			continue;
		}
		WriteUpTo(Slot);
		LastFrame = MoveTemp(Frame.ColorBuffer);
		fwrite(LastFrame.GetData(), sizeof(FColor), LastFrame.Num(), Pipe);
		++FramesWritten;
	}
}

void FGameVideoRecorder::Finish()
{
	if (Grabber)
	{
		Grabber->StopCapturingFrames();
		Grabber->Shutdown();
		Grabber.Reset();
	}
	if (Pipe)
	{
		pclose(Pipe);
		Pipe = nullptr;
		UE_LOG(LogGameVideo, Log, TEXT("GameVideoRecorder: wrote %lld frames (%.1f s)."), FramesWritten, FramesWritten / Fps);
	}
}
