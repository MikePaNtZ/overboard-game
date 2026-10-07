#include "OverboardPlayerController.h"
#include "OverboardPorts.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputMappingContext.h"
#include "InputAction.h"
#include "InputTriggers.h"
#include "InputModifiers.h"
#include "InputKeyEventArgs.h"
#include "EnhancedPlayerInput.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "OverboardWire.h"
#include "OverboardGameMode.h"
#include "BoardActor.h"
#include "OverboardCameraPawn.h"
#include "RideCourseElements.h"
#include "MovingObjectsActor.h"
#include "EngineUtils.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "UnrealClient.h"
#include <stdio.h>
#include "Components/SkeletalMeshComponent.h"
#include "Logging/LogMacros.h"
#include "Misc/CommandLine.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogOverboardInput, Log, All);

namespace
{
	// The host LISTENS for input on OverboardPorts::Input() (-ObPortBase + 2, default 9602).
}

AOverboardPlayerController::AOverboardPlayerController()
{
	PrimaryActorTick.bCanEverTick = true;

	// SAME BUG, second instance (overboard#162): APlayerController::InitInputSystem() creates
	// PlayerInput via UInputSettings::GetDefaultPlayerInputClass(), which has the identical
	// TSoftClassPtr-falls-back-if-not-already-resolved fragility as GetDefaultInputComponentClass()
	// (see SetupInputComponent's comment for the one that was actually caught first). If
	// PlayerInput silently falls back to plain UPlayerInput instead of UEnhancedPlayerInput, none
	// of Enhanced Input's mapping-context/trigger evaluation can run AT ALL, no matter how correct
	// the InputComponent/mapping/binding setup is -- that machinery lives in UEnhancedPlayerInput
	// specifically. OverridePlayerInputClass is the engine's own purpose-built escape hatch for
	// this ("used instead of the Input Settings' DefaultPlayerInputClass"), set here rather than
	// relying on the same fragile global resolution a second time.
	OverridePlayerInputClass = UEnhancedPlayerInput::StaticClass();
}

void AOverboardPlayerController::BeginPlay()
{
	Super::BeginPlay();

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	SendSocket = SocketSubsystem->CreateSocket(NAME_DGram, TEXT("OverboardInputSocket"), false);
	if (SendSocket)
	{
		SendSocket->SetNonBlocking(true);
	}

	HostAddr = SocketSubsystem->CreateInternetAddr();
	bool bValidIp = false;
	HostAddr->SetIp(TEXT("127.0.0.1"), bValidIp);
	HostAddr->SetPort(OverboardPorts::Input());

	if (!SendSocket || !bValidIp)
	{
		UE_LOG(LogOverboardInput, Error, TEXT("AOverboardPlayerController: failed to set up send socket to 127.0.0.1:%d"), OverboardPorts::Input());
	}

	SpawnCourseElements();

	FString RideLogPath;
	if (FParse::Value(FCommandLine::Get(), TEXT("ObRideLog="), RideLogPath))
	{
		RideLog = fopen(TCHAR_TO_UTF8(*RideLogPath), "w");
		if (RideLog)
		{
			fprintf(RideLog, "t_s,pad_lean,pad_steer,l2,sent_fore_aft,sent_steer,arm,reset,x_m,y_m,speed_mps,pitch_rad,yaw_rad,current_a,flags,handoff\n");
			RideLogStartSeconds = FPlatformTime::Seconds();
			UE_LOG(LogOverboardInput, Log, TEXT("AOverboardPlayerController: ride log to %s"), *RideLogPath);
		}
	}

	// -ObRiderSkill=raw: the old direct stick-to-body mapping, with no rider body layer.
	FString Skill;
	bRawRider = FParse::Value(FCommandLine::Get(), TEXT("ObRiderSkill="), Skill) && Skill.Equals(TEXT("raw"), ESearchCase::IgnoreCase);

	if (FParse::Param(FCommandLine::Get(), TEXT("ObDemoRider")))
	{
		Demo = MakeUnique<FDemoRider>();
		Demo->LoadCourse(ResolveCourseName());
		DemoStartSeconds = FPlatformTime::Seconds();
		UE_LOG(LogOverboardInput, Log, TEXT("AOverboardPlayerController: demo rider ON -- a script plays the pad."));
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("ObRecordVideo="), RecordVideoPath))
	{
		FParse::Value(FCommandLine::Get(), TEXT("ObRecordFps="), RecordFps);
	}
}

void AOverboardPlayerController::UpdateDemo(const ABoardActor* Board, float DeltaTime)
{
	OverboardWire::FBoardState State;
	const bool bHave = Board && Board->GetLatestState(State);
	const bool bDown = Board && Board->IsPhysicsHandoff(); // a fall is the handoff, see CheckForAutoResetOnFall
	const ARideCourseElements* Game = nullptr;
	for (TActorIterator<ARideCourseElements> It(GetWorld()); It; ++It)
	{
		Game = *It;
		break;
	}

	// The newest objects with velocities, so the demo rider can predict the crossings' traffic.
	TArray<FMovingObjectVel> ObjVels;
	bool bHaveObjects = false;
	for (TActorIterator<AMovingObjectsActor> It(GetWorld()); It; ++It)
	{
		bHaveObjects = It->GetObjectVelocities(ObjVels);
		break;
	}

	const FDemoPadOutput Pad = Demo->Update(FPlatformTime::Seconds() - DemoStartSeconds, DeltaTime, bHave, State, bDown,
		Game ? &Game->GetReadout() : nullptr, bHaveObjects ? &ObjVels : nullptr);

	// The demo outputs the value that goes on the wire (it is its own "shaping"), so it
	// overrides the shaped pad path instead of feeding the dead zone and curve.
	bDemoOverride = true;
	DemoForeAft = Pad.Lean;
	DemoSteer = Pad.Steer;
	TailBrake = Pad.TailBrake;
	if (Pad.bArm) { bArmedOnce = true; }
	bArmHeld = Pad.bArm;
	bResetHeld = Pad.bReset;
	bKickPending |= Pad.bKick;
	if (Pad.bFinished && (!RecordVideoPath.IsEmpty() || Demo->IsLapsMode()))
	{
		UE_LOG(LogOverboardInput, Log, TEXT("AOverboardPlayerController: demo finished; closing the video and quitting."));
		if (Recorder)
		{
			Recorder->Finish();
		}
		RecordVideoPath.Reset();
		ConsoleCommand(TEXT("quit"));
	}
}

FString AOverboardPlayerController::ResolveCourseName() const
{
	// -ObCourse=<name> picks the layout in tools/play/elements/; OB_CityHill gets city_hill.
	FString Course;
	if (!FParse::Value(FCommandLine::Get(), TEXT("ObCourse="), Course))
	{
		const FString MapName = GetWorld() ? GetWorld()->GetMapName() : FString();
		if (MapName.Contains(TEXT("CityHill")))
		{
			Course = TEXT("city_hill");
		}
	}
	return Course;
}

void AOverboardPlayerController::SpawnCourseElements()
{
	const FString Course = ResolveCourseName();
	if (Course.IsEmpty() || Course == TEXT("none"))
	{
		return;
	}
	ARideCourseElements* Elements = GetWorld()->SpawnActor<ARideCourseElements>();
	if (Elements && !Elements->LoadLayout(Course))
	{
		Elements->Destroy();
	}

	// Level 2 phase B traffic: spawn the moving-objects actor if the course has an objects file.
	const FString ObjectsFile = ResolveObjectsFile(Course);
	if (!ObjectsFile.IsEmpty() && FPaths::FileExists(ObjectsFile))
	{
		AMovingObjectsActor* Objects = GetWorld()->SpawnActor<AMovingObjectsActor>();
		if (Objects && !Objects->LoadObjects(ObjectsFile))
		{
			Objects->Destroy();
		}
	}
}

FString AOverboardPlayerController::ResolveObjectsFile(const FString& Course) const
{
	// -ObObjects=<path> wins. Else derive from the course data directory: the same objects.json
	// sim-host reads. The default course dir matches tools/play/levels/embarcadero.env's COURSE.
	FString Path;
	if (FParse::Value(FCommandLine::Get(), TEXT("ObObjects="), Path))
	{
		return Path;
	}
	if (Course.IsEmpty() || Course == TEXT("none"))
	{
		return FString();
	}
	const FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	if (Home.IsEmpty())
	{
		return FString();
	}
	return FPaths::Combine(Home, TEXT("projects/overboard-viz/out/carve-lab/data/courses"), Course, TEXT("objects.json"));
}

void AOverboardPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Rumble.Shutdown();
	if (RideLog)
	{
		fclose(RideLog);
		RideLog = nullptr;
	}
	if (Recorder)
	{
		Recorder->Finish();
	}
	if (SendSocket)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(SendSocket);
		SendSocket = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

void AOverboardPlayerController::SetupInputComponent()
{
	// THE ACTUAL ROOT CAUSE (overboard#162), found by running the self-test below and reading
	// its own log, not by reasoning: Super::SetupInputComponent() creates InputComponent via
	// UInputSettings::GetDefaultInputComponentClass(), which resolves a TSoftClassPtr and
	// silently falls back to plain UInputComponent -- not UEnhancedInputComponent -- if that
	// soft pointer isn't already resolved in memory at this exact point. Confirmed happening in
	// -game mode despite Config/DefaultEngine.ini's DefaultInputComponentClass being correctly
	// set (verified separately): the self-test's own log showed "InputComponent is not an
	// EnhancedInputComponent", which meant every EIC->BindAction call below was silently
	// skipped, no delegate was ever bound to any action, and OnWeightShiftForeAft etc. could
	// never fire -- regardless of whether the mapping context was ever added correctly. This is
	// what actually killed both the keyboard AND the gamepad, not (only) the AddMappingContext
	// timing below. Pre-creating InputComponent as the correct type here sidesteps the fragile
	// config resolution entirely instead of chasing why it sometimes doesn't resolve --
	// Super::SetupInputComponent() only creates it `if (InputComponent == NULL)`, so this wins.
	if (!InputComponent)
	{
		InputComponent = NewObject<UEnhancedInputComponent>(this, TEXT("PC_InputComponent0"));
		InputComponent->RegisterComponent();
		UE_LOG(LogOverboardInput, Log, TEXT("AOverboardPlayerController: pre-created InputComponent as UEnhancedInputComponent explicitly."));
	}

	Super::SetupInputComponent();

	// Built at runtime, not as .uasset data assets, so the whole mapping is readable in one place.
	// The pad and the keyboard drive SEPARATE actions: the pad value goes to the wire unfiltered,
	// and only the digital keys are ramped (see the class header).
	MappingContext = NewObject<UInputMappingContext>(this, TEXT("OverboardMappingContext"));

	auto MapDigitalAxisPair = [this](UInputAction* Action, FKey PositiveKey1, FKey PositiveKey2, FKey NegativeKey1, FKey NegativeKey2)
	{
		MappingContext->MapKey(Action, PositiveKey1);
		MappingContext->MapKey(Action, PositiveKey2);
		MappingContext->MapKey(Action, NegativeKey1).Modifiers.Add(NewObject<UInputModifierNegate>(this));
		MappingContext->MapKey(Action, NegativeKey2).Modifiers.Add(NewObject<UInputModifierNegate>(this));
	};
	auto MakeAction = [this](const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* Action = NewObject<UInputAction>(this, Name);
		Action->ValueType = Type;
		return Action;
	};

	IA_LeanPad = MakeAction(TEXT("IA_LeanPad"), EInputActionValueType::Axis1D);
	MappingContext->MapKey(IA_LeanPad, EKeys::Gamepad_LeftY);

	IA_LeanKeys = MakeAction(TEXT("IA_LeanKeys"), EInputActionValueType::Axis1D);
	MapDigitalAxisPair(IA_LeanKeys, EKeys::W, EKeys::Up, EKeys::S, EKeys::Down);

	IA_SteerPad = MakeAction(TEXT("IA_SteerPad"), EInputActionValueType::Axis1D);
	MappingContext->MapKey(IA_SteerPad, EKeys::Gamepad_RightX);

	IA_SteerKeys = MakeAction(TEXT("IA_SteerKeys"), EInputActionValueType::Axis1D);
	MapDigitalAxisPair(IA_SteerKeys, EKeys::D, EKeys::Right, EKeys::A, EKeys::Left);

	// L2 analog (0..1). Left Shift is the keyboard equivalent (full pull).
	IA_TailBrake = MakeAction(TEXT("IA_TailBrake"), EInputActionValueType::Axis1D);
	MappingContext->MapKey(IA_TailBrake, EKeys::Gamepad_LeftTriggerAxis);
	MappingContext->MapKey(IA_TailBrake, EKeys::LeftShift);

	IA_Arm = MakeAction(TEXT("IA_Arm"), EInputActionValueType::Boolean);
	MappingContext->MapKey(IA_Arm, EKeys::Gamepad_FaceButton_Bottom); // Cross
	MappingContext->MapKey(IA_Arm, EKeys::SpaceBar);

	IA_Reset = MakeAction(TEXT("IA_Reset"), EInputActionValueType::Boolean);
	MappingContext->MapKey(IA_Reset, EKeys::Gamepad_FaceButton_Right); // Circle
	MappingContext->MapKey(IA_Reset, EKeys::R);

	// Options on a DualSense is Apple's buttonMenu, which UE maps to Gamepad_Special_Right.
	IA_CameraCycle = MakeAction(TEXT("IA_CameraCycle"), EInputActionValueType::Boolean);
	MappingContext->MapKey(IA_CameraCycle, EKeys::Gamepad_Special_Right);
	MappingContext->MapKey(IA_CameraCycle, EKeys::C);

	IA_Quit = MakeAction(TEXT("IA_Quit"), EInputActionValueType::Boolean);
	MappingContext->MapKey(IA_Quit, EKeys::Escape);

	// AddMappingContext does NOT happen here -- see ReceivedPlayer().

	if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent))
	{
		auto BindAxis = [EIC, this](UInputAction* Action, void (AOverboardPlayerController::*Handler)(const FInputActionValue&))
		{
			EIC->BindAction(Action, ETriggerEvent::Triggered, this, Handler);
			EIC->BindAction(Action, ETriggerEvent::Completed, this, Handler);
		};
		auto BindButton = [EIC, this](UInputAction* Action, void (AOverboardPlayerController::*Handler)(const FInputActionValue&))
		{
			EIC->BindAction(Action, ETriggerEvent::Started, this, Handler);
			EIC->BindAction(Action, ETriggerEvent::Completed, this, Handler);
		};
		BindAxis(IA_LeanPad, &AOverboardPlayerController::OnLeanPad);
		BindAxis(IA_LeanKeys, &AOverboardPlayerController::OnLeanKeys);
		BindAxis(IA_SteerPad, &AOverboardPlayerController::OnSteerPad);
		BindAxis(IA_SteerKeys, &AOverboardPlayerController::OnSteerKeys);
		BindAxis(IA_TailBrake, &AOverboardPlayerController::OnTailBrake);
		BindButton(IA_Arm, &AOverboardPlayerController::OnArm);
		BindButton(IA_Reset, &AOverboardPlayerController::OnReset);
		EIC->BindAction(IA_CameraCycle, ETriggerEvent::Started, this, &AOverboardPlayerController::OnCameraCycle);
		EIC->BindAction(IA_Quit, ETriggerEvent::Started, this, &AOverboardPlayerController::OnQuit);
		UE_LOG(LogOverboardInput, Log, TEXT("AOverboardPlayerController: action bindings registered on the EnhancedInputComponent."));
	}
	else
	{
		UE_LOG(LogOverboardInput, Error, TEXT("AOverboardPlayerController: InputComponent is not an EnhancedInputComponent -- check project Enhanced Input settings (DefaultPlayerInputClass/DefaultInputComponentClass in DefaultEngine.ini). No key will ever do anything."));
	}
}

void AOverboardPlayerController::ReceivedPlayer()
{
	Super::ReceivedPlayer();

	// THE FIX (overboard#162) -- see the header comment on this override for the full story.
	// Every branch below logs, on success or failure: a silent null here is exactly what cost a
	// whole cycle last time, and this is the third class of silent-null bug tonight (the
	// FObjectFinder crash, the flat-material load, now this).
	ULocalPlayer* LocalPlayer = GetLocalPlayer();
	if (!LocalPlayer)
	{
		UE_LOG(LogOverboardInput, Error, TEXT("AOverboardPlayerController::ReceivedPlayer: GetLocalPlayer() is STILL null -- mapping context cannot be added, every key/stick will read as zero. This should not happen (ReceivedPlayer is called right after Player is assigned) -- if it does, something upstream of Enhanced Input is broken."));
		return;
	}

	UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
	if (!Subsystem)
	{
		UE_LOG(LogOverboardInput, Error, TEXT("AOverboardPlayerController::ReceivedPlayer: LocalPlayer has no UEnhancedInputLocalPlayerSubsystem -- check DefaultPlayerInputClass/DefaultInputComponentClass in DefaultEngine.ini and that the EnhancedInput plugin is enabled. Mapping context NOT added; every key/stick will read as zero."));
		return;
	}

	if (!MappingContext)
	{
		UE_LOG(LogOverboardInput, Error, TEXT("AOverboardPlayerController::ReceivedPlayer: MappingContext is null -- SetupInputComponent has not run yet or failed. Mapping context NOT added."));
		return;
	}

	Subsystem->AddMappingContext(MappingContext, 0);
	UE_LOG(LogOverboardInput, Log, TEXT("AOverboardPlayerController::ReceivedPlayer: mapping context added successfully. Keyboard and gamepad input should now reach the wire."));

	if (FParse::Param(FCommandLine::Get(), TEXT("OverboardInputSelfTest")))
	{
		FTimerHandle SelfTestHandle;
		GetWorldTimerManager().SetTimer(SelfTestHandle, this, &AOverboardPlayerController::RunInputSelfTest, 1.5f, false);
	}
}

// Verbose-level instrumentation on every handler (overboard#162): raise LogOverboardInput to
// Verbose (`Log LogOverboardInput Verbose` in the console) to see each value as it arrives.
void AOverboardPlayerController::OnLeanPad(const FInputActionValue& Value) { PadLean = Value.Get<float>(); UE_LOG(LogOverboardInput, Verbose, TEXT("OnLeanPad: %.3f"), PadLean); }
void AOverboardPlayerController::OnLeanKeys(const FInputActionValue& Value) { KeyLean = Value.Get<float>(); UE_LOG(LogOverboardInput, Verbose, TEXT("OnLeanKeys: %.3f"), KeyLean); }
void AOverboardPlayerController::OnSteerPad(const FInputActionValue& Value) { PadSteer = Value.Get<float>(); UE_LOG(LogOverboardInput, Verbose, TEXT("OnSteerPad: %.3f"), PadSteer); }
void AOverboardPlayerController::OnSteerKeys(const FInputActionValue& Value) { KeySteer = Value.Get<float>(); UE_LOG(LogOverboardInput, Verbose, TEXT("OnSteerKeys: %.3f"), KeySteer); }
void AOverboardPlayerController::OnTailBrake(const FInputActionValue& Value) { TailBrake = Value.Get<float>(); UE_LOG(LogOverboardInput, Verbose, TEXT("OnTailBrake: %.3f"), TailBrake); }
void AOverboardPlayerController::OnArm(const FInputActionValue& Value) { bArmHeld = Value.Get<bool>(); bArmedOnce |= bArmHeld; UE_LOG(LogOverboardInput, Verbose, TEXT("OnArm: %s"), bArmHeld ? TEXT("true") : TEXT("false")); }
void AOverboardPlayerController::OnReset(const FInputActionValue& Value) { bResetHeld = Value.Get<bool>(); UE_LOG(LogOverboardInput, Verbose, TEXT("OnReset: %s"), bResetHeld ? TEXT("true") : TEXT("false")); }

void AOverboardPlayerController::OnCameraCycle(const FInputActionValue& Value)
{
	if (AOverboardCameraPawn* CameraPawn = Cast<AOverboardCameraPawn>(GetPawn()))
	{
		CameraPawn->CycleView();
	}
	else
	{
		UE_LOG(LogOverboardInput, Log, TEXT("AOverboardPlayerController: camera cycle pressed, but the possessed pawn is not an AOverboardCameraPawn."));
	}
}

void AOverboardPlayerController::OnQuit(const FInputActionValue& Value)
{
	if (Value.Get<bool>())
	{
		UE_LOG(LogOverboardInput, Log, TEXT("AOverboardPlayerController: Escape pressed, quitting."));
		ConsoleCommand(TEXT("quit"));
	}
}

const ABoardActor* AOverboardPlayerController::FindBoard() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	if (const AOverboardGameMode* GameMode = World->GetAuthGameMode<AOverboardGameMode>())
	{
		if (const ABoardActor* Board = GameMode->GetSpawnedBoard())
		{
			return Board;
		}
	}
	// A level can place its board by hand rather than through the game mode.
	for (TActorIterator<ABoardActor> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

void AOverboardPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	const ABoardActor* Board = FindBoard();
	if (Board)
	{
		CheckForAutoResetOnFall(Board);
	}
	UpdateRumble(Board);
	ProbeWipeout(Board);
	if (Demo)
	{
		UpdateDemo(Board, DeltaTime);
	}
	if (!RecordVideoPath.IsEmpty())
	{
		if (!Recorder)
		{
			Recorder = MakeUnique<FGameVideoRecorder>();
			FIntPoint VideoSize(1280, 720);
			FParse::Value(FCommandLine::Get(), TEXT("ObRecordW="), VideoSize.X);
			FParse::Value(FCommandLine::Get(), TEXT("ObRecordH="), VideoSize.Y);
			if (!Recorder->Start(RecordVideoPath, RecordFps, VideoSize))
			{
				RecordVideoPath.Reset();
			}
		}
		if (Recorder)
		{
			Recorder->Tick();
		}
	}

	// Send at frame rate -- no accumulator/throttle. sim-host zeroes an input older than 100 ms,
	// so the frame rate must stay well above 10 Hz.
	SendInputPacket(DeltaTime);
}

void AOverboardPlayerController::CheckForAutoResetOnFall(const ABoardActor* Board)
{
	// What counts as a fall. With --terrain the ride ends at the ADR-0012 handoff latch (bit 4:
	// a nose strike or a tilt past 35 deg), and the player resets it (Circle / R) -- never
	// automatically, or the crash would end one frame after it began.
	//
	// StateOut bit 2 ("fallen") is only |pitch| > 20 deg, and a wanted tail-brake drag reaches
	// that nose-up (the tail strike is ~20.4 deg). Resetting on it ended every tail-brake stop.
	// So bit 2 alone resets only if it stays on for FallenAloneResetSeconds with no handoff: the
	// stuck case of a sim run without terrain, where no handoff ever comes.
	if (Board->IsPhysicsHandoff() || !Board->IsFallen())
	{
		FallenAloneSeconds = 0.0;
		return;
	}
	const double Before = FallenAloneSeconds;
	FallenAloneSeconds += GetWorld()->GetDeltaSeconds();
	if (Before < kFallenAloneResetSeconds && FallenAloneSeconds >= kFallenAloneResetSeconds)
	{
		bAutoResetPending = true;
		UE_LOG(LogOverboardInput, Log, TEXT("AOverboardPlayerController: fallen flag on for %.1f s with no handoff, sending one Reset."), kFallenAloneResetSeconds);
	}
}

void AOverboardPlayerController::ProbeWipeout(const ABoardActor* Board)
{
	// Logs where the board and the rider come to rest after a handoff, in the MuJoCo frame (m),
	// so tools/play/wipeouts/compare_wipeout.py can check the Unreal ragdoll against MuJoCo's
	// fall. Read-only: it reads component positions and changes nothing.
	const bool bHandoff = Board && Board->IsPhysicsHandoff();
	if (!bHandoff)
	{
		WipeoutStartSeconds = -1.0;
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (WipeoutStartSeconds < 0.0)
	{
		WipeoutStartSeconds = Now;
		WipeoutNextLog = Now;
	}
	if (Now < WipeoutNextLog || Now - WipeoutStartSeconds > 8.5)
	{
		return;
	}
	WipeoutNextLog += (Now - WipeoutStartSeconds < 1.0) ? 0.1 : 0.5;

	const FQuat InvYaw = FRotator(0.f, Board->GetWorldOriginYawDeg(), 0.f).Quaternion().Inverse();
	auto ToMuJoCo = [&](const FVector& WorldCm)
	{
		const FVector L = InvYaw.RotateVector(WorldCm - Board->GetWorldOriginOffsetCm());
		return FVector(L.X / 100.0, -L.Y / 100.0, L.Z / 100.0);
	};
	// The board body that simulates, and the rider body that ragdolls (whichever mesh simulates).
	FVector BoardPos = ToMuJoCo(Board->GetActorLocation());
	FVector RiderPos = FVector::ZeroVector;
	double RiderSpeed = 0.0;
	bool bRider = false;
	TArray<UPrimitiveComponent*> Prims;
	Board->GetComponents<UPrimitiveComponent>(Prims);
	for (const UPrimitiveComponent* P : Prims)
	{
		if (const USkeletalMeshComponent* Sk = Cast<USkeletalMeshComponent>(P))
		{
			// A ragdoll simulates from the pelvis down (its root bone does not, so ask "any"); a
			// rider who stepped off does not simulate but is detached from the board.
			const bool bRagdoll = Sk->IsAnySimulatingPhysics();
			const bool bSteppedOff = !bRagdoll && Sk->IsVisible() && Sk->GetAttachParent() == nullptr && Sk->GetNumBones() > 0;
			if (!bRagdoll && !bSteppedOff)
			{
				continue;
			}
			const FName Pelvis = Sk->GetBoneIndex(TEXT("pelvis")) != INDEX_NONE ? FName(TEXT("pelvis")) : Sk->GetBoneName(0);
			RiderPos = ToMuJoCo(Sk->GetBoneLocation(Pelvis));
			RiderSpeed = const_cast<USkeletalMeshComponent*>(Sk)->GetPhysicsLinearVelocity(Pelvis).Size() / 100.0;
			bRider = true;
		}
		else if (P->IsSimulatingPhysics())
		{
			BoardPos = ToMuJoCo(P->GetComponentLocation());
		}
	}
	UE_LOG(LogOverboardInput, Log, TEXT("WipeoutProbe: t %.1f board %.2f %.2f %.2f rider %s %.2f %.2f %.2f speed %.2f"),
		Now - WipeoutStartSeconds, BoardPos.X, BoardPos.Y, BoardPos.Z, bRider ? TEXT("ragdoll") : TEXT("none"),
		RiderPos.X, RiderPos.Y, RiderPos.Z, RiderSpeed);
}

void AOverboardPlayerController::WriteRideLog(float SentForeAft, float SentSteer)
{
	// -ObRideLog=<csv>: one row per frame, so a ride can be diagnosed afterwards (what the thumbs
	// did, what the rider body sent, what the board did). Read-only.
	if (!RideLog)
	{
		return;
	}
	OverboardWire::FBoardState State;
	const ABoardActor* Board = FindBoard();
	const bool bHave = Board && Board->GetLatestState(State);
	fprintf(RideLog, "%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%.3f,%.3f,%.3f,%.4f,%.4f,%.2f,%u,%d\n",
		FPlatformTime::Seconds() - RideLogStartSeconds, PadLean, PadSteer, TailBrake, SentForeAft, SentSteer,
		bArmHeld ? 1 : 0, bResetHeld ? 1 : 0,
		bHave ? State.Pos[0] : 0.f, bHave ? State.Pos[1] : 0.f, bHave ? State.WheelRateRadS * 0.146f : 0.f,
		bHave ? State.PitchRad : 0.f, bHave ? State.YawRad : 0.f, bHave ? State.MotorCurrentA : 0.f,
		bHave ? static_cast<unsigned>(State.Flags) : 0u, Board && Board->IsPhysicsHandoff() ? 1 : 0);
}

void AOverboardPlayerController::UpdateRumble(const ABoardActor* Board)
{
	const double Now = FPlatformTime::Seconds();
	float Level = 0.f;
	if (Board)
	{
		const uint16 Flags = Board->GetLatestFlags();
		const bool bDown = Board->IsPhysicsHandoff(); // a fall is the handoff, see CheckForAutoResetOnFall
		if (bDown && !bWasDownLastTick)
		{
			FallJoltUntilSeconds = Now + RumbleFallSeconds;
		}
		bWasDownLastTick = bDown;

		if (Now < FallJoltUntilSeconds)
		{
			Level = RumbleFallLevel;
		}
		else if (bDown)
		{
			Level = 0.f; // the ride is over; the warning no longer means anything
		}
		else if (Flags & OverboardWire::EStateFlags::RiderWarningSolid)
		{
			Level = RumbleSolidLevel;
		}
		else if (Flags & OverboardWire::EStateFlags::RiderWarningPulsed)
		{
			Level = WarningPulseOn(Now) ? RumblePulsedLevel : 0.f;
		}
	}
	Rumble.SetLevel(this, Level);
}

float AOverboardPlayerController::ShapeLean(float Raw) const
{
	const float Abs = FMath::Abs(Raw);
	if (Abs < StickDeadzone)
	{
		return 0.f;
	}
	// Rescale [Deadzone, 1] -> [0, 1] so the curve starts at 0 right past the dead-zone edge.
	const float X = FMath::Min((Abs - StickDeadzone) / (1.f - StickDeadzone), 1.f);
	const float Curved = (1.f - LeanCubicBlend) * X + LeanCubicBlend * X * X * X;
	return FMath::Sign(Raw) * Curved;
}

float AOverboardPlayerController::ShapeSteer(float Raw) const
{
	// The same dead zone and curve as the lean: a small carve answers, a full stick is a full carve.
	return ShapeLean(Raw);
}

void AOverboardPlayerController::SendInputPacket(float DeltaTime)
{
	// Keyboard ramps only. A pad value is already gradual and goes to the wire unfiltered.
	SmoothedKeyLean = FMath::FInterpTo(SmoothedKeyLean, KeyLean, DeltaTime, KeyboardRampSpeed);
	SmoothedKeySteer = FMath::FInterpTo(SmoothedKeySteer, KeySteer, DeltaTime, KeyboardRampSpeed);

	float ForeAft = FMath::Clamp(bDemoOverride ? DemoForeAft : ShapeLean(PadLean) + SmoothedKeyLean, -1.f, 1.f);
	float Steer = FMath::Clamp(bDemoOverride ? DemoSteer : ShapeSteer(PadSteer) + SmoothedKeySteer, -1.f, 1.f);

	// The rider's body (not the demo, not -ObRiderSkill=raw). A thumb moves a stick end to end in
	// ~50 ms; a rider shifts weight over ~0.6 s and swings the hips over ~0.4 s, and carves less
	// sharply at speed. Without this, every thumb twitch at 8 m/s reached the board at once and
	// the carve felt twitchy (Mike, first ride, 2026-10-05). Input only: no board physics here.
	if (!bDemoOverride && !bRawRider)
	{
		constexpr float kMaxCarveG = 0.35f;
		float SpeedMps = 0.f;
		OverboardWire::FBoardState State;
		if (const ABoardActor* Board = FindBoard(); Board && Board->GetLatestState(State))
		{
			SpeedMps = FMath::Abs(State.WheelRateRadS) * 0.146f;
		}
		// Cap the carve by sideways acceleration. Since controls fc30fab (camber steer force-limited
		// like a motorcycle tyre) full stick asks for 0.6 g at speed and 0.35 g reversals hold at
		// 8 m/s (controls test); the game allows kMaxCarveG = 0.35 g, the value verified there. Full stick
		// curvature in the sim: min(0.25 1/m, 0.6 g / v^2) x fade(v), so the stick cap is the ratio.
		const float V2 = FMath::Max(SpeedMps * SpeedMps, 0.01f);
		// The sim's full law (controls lean_steer.rs): the turn also fades in with speed, none below
		// 0.8 m/s and full from 3.0 m/s.
		const float Fade = FMath::Clamp((SpeedMps - 0.8f) / 2.2f, 0.f, 1.f);
		const float FullStickKappa = FMath::Max(FMath::Min(0.25f, 0.6f * 9.81f / V2) * Fade, 1e-4f);
		const float MaxSteer = FMath::Min(1.f, (kMaxCarveG * 9.81f / V2) / FullStickKappa);
		Steer = FMath::Clamp(Steer, -MaxSteer, MaxSteer);
		BodySteer = FMath::FInterpConstantTo(BodySteer, Steer, DeltaTime, CarveRatePerS);
		BodyLean = FMath::FInterpConstantTo(BodyLean, ForeAft, DeltaTime, LeanRatePerS);
		ForeAft = BodyLean;
		Steer = BodySteer;
	}

	// Tail brake (L2): fore_aft = min(stick, -L2). A full pull is always -1, whatever the stick.
	const float BrakeTravel = FMath::Clamp(TailBrake, 0.f, 1.f);
	if (BrakeTravel > TriggerDeadzone)
	{
		const float Brake = (BrakeTravel - TriggerDeadzone) / (1.f - TriggerDeadzone);
		ForeAft = FMath::Min(ForeAft, -Brake);
		if (!bDemoOverride && !bRawRider)
		{
			// A hard lean back is a fast, deliberate move: let the brake lead the body, and do not
			// let the body lag pull the lean forward again when L2 is released.
			BodyLean = FMath::Min(BodyLean, FMath::FInterpConstantTo(BodyLean, -Brake, DeltaTime, BrakeRatePerS));
			ForeAft = FMath::Min(ForeAft, BodyLean);
		}
	}

	LastSentForeAft = ForeAft;
	LastSentSteer = Steer;
	WriteRideLog(ForeAft, Steer);

	if (!SendSocket || !HostAddr.IsValid())
	{
		return;
	}

	OverboardWire::FInputPacket Packet;
	Packet.Seq = SendSeq++; // monotonic for the life of the socket, regardless of arm/reset state
	const bool bSendReset = bResetHeld || bAutoResetPending;
	bAutoResetPending = false; // one-shot: consumed the instant it's sent, never held
	Packet.Flags = (bArmHeld ? OverboardWire::EInputFlags::Arm : 0) | (bSendReset ? OverboardWire::EInputFlags::Reset : 0)
		| (bKickPending ? OverboardWire::EInputFlags::Kick : 0);
	bKickPending = false; // rising edge on the host: send it in one packet only
	Packet.WeightShiftForeAft = ForeAft;
	// Under --lean-steer the rider model sets the lateral ballast from steer; the controls track
	// asks for 0 here so that a later change cannot count the lean twice.
	Packet.WeightShiftLateral = 0.f;
	Packet.Steer = Steer;

	uint8 Buf[OverboardWire::kInputPacketWireSize];
	OverboardWire::EncodeInputPacket(Packet, Buf);

	int32 BytesSent = 0;
	SendSocket->SendTo(Buf, sizeof(Buf), BytesSent, *HostAddr);
}

void AOverboardPlayerController::RunInputSelfTest()
{
	// Headless check of the real path: simulated key events go through the mapping context,
	// the triggers, the handlers and the shaping, and the test reads the value that went into the
	// last wire packet. Three phases, 0.5 s apart, then quit.
	UE_LOG(LogOverboardInput, Log, TEXT("=== OverboardInputSelfTest: BEGIN ==="));
	if (!PlayerInput)
	{
		UE_LOG(LogOverboardInput, Error, TEXT("OverboardInputSelfTest: FAIL -- PlayerInput is null, cannot inject a key event."));
		return;
	}

	auto Inject = [this](FKey Key, EInputEvent Event, float Value)
	{
		if (PlayerInput)
		{
			PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(Key, Event, Value));
		}
	};
	auto Report = [this](const TCHAR* Phase, bool bPass)
	{
		UE_LOG(LogOverboardInput, Log, TEXT("=== OverboardInputSelfTest %s: %s (fore_aft=%.3f steer=%.3f) ==="),
			Phase, bPass ? TEXT("PASS") : TEXT("FAIL"), LastSentForeAft, LastSentSteer);
	};

	// 1. Keyboard: W must ramp the sent lean above zero.
	Inject(EKeys::W, IE_Pressed, 1.f);
	FTimerHandle H1;
	GetWorldTimerManager().SetTimer(H1, [this, Inject, Report]()
	{
		Report(TEXT("KEYBOARD W"), LastSentForeAft > 0.05f);
		Inject(EKeys::W, IE_Released, 0.f);
		SmoothedKeyLean = 0.f; // the key ramp would otherwise decay into phase 2's reading

		// 2. Pad: left stick Y = 0.6 must send the shaped value, unfiltered.
		Inject(EKeys::Gamepad_LeftY, IE_Axis, 0.6f);
		FTimerHandle H2;
		GetWorldTimerManager().SetTimer(H2, [this, Inject, Report]()
		{
			const float Expected = ShapeLean(0.6f);
			Report(TEXT("PAD LEFT Y 0.6"), FMath::IsNearlyEqual(LastSentForeAft, Expected, 0.05f));

			// 3. L2 full pull with the stick still forward must send -1 (min rule).
			Inject(EKeys::Gamepad_LeftTriggerAxis, IE_Axis, 1.f);
			FTimerHandle H3;
			GetWorldTimerManager().SetTimer(H3, [this, Inject, Report]()
			{
				Report(TEXT("L2 FULL WITH STICK FORWARD"), FMath::IsNearlyEqual(LastSentForeAft, -1.f, 0.01f));
				Inject(EKeys::Gamepad_LeftTriggerAxis, IE_Axis, 0.f);
				Inject(EKeys::Gamepad_LeftY, IE_Axis, 0.f);

				// A self-test is a one-shot headless check: quit, so no stray instance keeps sending.
				FTimerHandle QuitHandle;
				GetWorldTimerManager().SetTimer(QuitHandle, [this]()
				{
					UE_LOG(LogOverboardInput, Log, TEXT("OverboardInputSelfTest: done, quitting."));
					ConsoleCommand(TEXT("quit"));
				}, 0.5f, false);
			}, 0.5f, false);
		}, 0.5f, false);
	}, 1.0f, false);
}
