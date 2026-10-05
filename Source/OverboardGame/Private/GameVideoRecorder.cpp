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

bool FGameVideoRecorder::Start(const FString& OutPath, float InFps, FIntPoint OutSize)
{
	if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->GetGameViewport())
	{
		UE_LOG(LogGameVideo, Error, TEXT("GameVideoRecorder: no game viewport."));
		return false;
	}
	Size = OutSize;
	Fps = FMath::Max(1.f, InFps);

	const FString Cmd = FString::Printf(
		TEXT("/opt/homebrew/bin/ffmpeg -y -loglevel error -f rawvideo -pix_fmt bgra -s %dx%d -r %.0f -i - ")
		TEXT("-c:v h264_videotoolbox -b:v 12M -pix_fmt yuv420p -movflags +faststart \"%s\""),
		Size.X, Size.Y, Fps, *OutPath);
	Pipe = popen(TCHAR_TO_UTF8(*Cmd), "w");
	if (!Pipe)
	{
		UE_LOG(LogGameVideo, Error, TEXT("GameVideoRecorder: could not start ffmpeg."));
		return false;
	}
	bStopWriter = false;
	Writer = std::thread([this]() { WriterLoop(); });

	TSharedRef<FSceneViewport> ViewportRef = StaticCastSharedRef<FSceneViewport>(GEngine->GameViewport->GetGameViewportWidget()->GetViewportInterface().Pin().ToSharedRef());
	Grabber = MakeUnique<FFrameGrabber>(ViewportRef, Size, PF_B8G8R8A8, 4);
	Grabber->StartCapturingFrames();
	StartSeconds = FPlatformTime::Seconds();
	UE_LOG(LogGameVideo, Log, TEXT("GameVideoRecorder: recording %dx%d at %.0f fps to %s"), Size.X, Size.Y, Fps, *OutPath);
	return true;
}

void FGameVideoRecorder::Enqueue(const TSharedPtr<TArray<FColor>, ESPMode::ThreadSafe>& Frame)
{
	{
		std::lock_guard<std::mutex> Lock(QueueMutex);
		if (Queue.Num() >= kMaxQueued)
		{
			++FramesDropped;
			return;
		}
		Queue.Add(Frame);
	}
	QueueCv.notify_one();
}

void FGameVideoRecorder::WriterLoop()
{
	for (;;)
	{
		TSharedPtr<TArray<FColor>, ESPMode::ThreadSafe> Frame;
		{
			std::unique_lock<std::mutex> Lock(QueueMutex);
			QueueCv.wait(Lock, [this]() { return bStopWriter || Queue.Num() > 0; });
			if (Queue.Num() == 0)
			{
				return; // stopped and drained
			}
			Frame = Queue[0];
			Queue.RemoveAt(0);
		}
		fwrite(Frame->GetData(), sizeof(FColor), Frame->Num(), Pipe);
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
		TSharedPtr<TArray<FColor>, ESPMode::ThreadSafe> New = MakeShared<TArray<FColor>, ESPMode::ThreadSafe>(MoveTemp(Frame.ColorBuffer));
		const int64 Slot = static_cast<int64>((FPlatformTime::Seconds() - StartSeconds) * Fps);
		if (Slot < FramesQueued)
		{
			LastFrame = New; // ahead of the grid: it fills the next slot instead
			continue;
		}
		// Repeat the last frame for every slot the game missed, so the video keeps real time.
		while (FramesQueued < Slot && LastFrame.IsValid())
		{
			Enqueue(LastFrame);
			++FramesQueued;
		}
		LastFrame = New;
		Enqueue(New);
		++FramesQueued;
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
	if (Writer.joinable())
	{
		{
			std::lock_guard<std::mutex> Lock(QueueMutex);
			bStopWriter = true;
		}
		QueueCv.notify_one();
		Writer.join();
	}
	if (Pipe)
	{
		pclose(Pipe);
		Pipe = nullptr;
		UE_LOG(LogGameVideo, Log, TEXT("GameVideoRecorder: wrote %lld frames (%.1f s), %lld dropped by a full queue."), FramesQueued - FramesDropped, (FramesQueued - FramesDropped) / Fps, FramesDropped);
	}
}
