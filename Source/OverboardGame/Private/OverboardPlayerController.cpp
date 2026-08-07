#include "OverboardPlayerController.h"

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
#include "Logging/LogMacros.h"
#include "Misc/CommandLine.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogOverboardInput, Log, All);

namespace
{
	constexpr int32 kHostPort = 9602; // host LISTENS here, so we SEND to it
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
	HostAddr->SetPort(kHostPort);

	if (!SendSocket || !bValidIp)
	{
		UE_LOG(LogOverboardInput, Error, TEXT("AOverboardPlayerController: failed to set up send socket to 127.0.0.1:%d"), kHostPort);
	}
}

void AOverboardPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
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

	// Built at runtime rather than as .uasset data assets -- still no editor session available
	// to author Input Mapping Context assets as of W2 either. See class header for the full
	// mapping (gamepad AND keyboard) and why right-stick-X / A-D / Left-Right legitimately drive
	// two wire channels at once.
	MappingContext = NewObject<UInputMappingContext>(this, TEXT("OverboardMappingContext"));

	// Maps two positive keys and two negative keys (Negate modifier) onto one Axis1D action --
	// the standard Enhanced Input pattern for a digital key pair driving an analogue-shaped
	// action. Both WASD and the arrow keys are mapped, per the CEO's "direction pad" ask.
	auto MapDigitalAxisPair = [this](UInputAction* Action, FKey PositiveKey1, FKey PositiveKey2, FKey NegativeKey1, FKey NegativeKey2)
	{
		MappingContext->MapKey(Action, PositiveKey1);
		MappingContext->MapKey(Action, PositiveKey2);
		MappingContext->MapKey(Action, NegativeKey1).Modifiers.Add(NewObject<UInputModifierNegate>(this));
		MappingContext->MapKey(Action, NegativeKey2).Modifiers.Add(NewObject<UInputModifierNegate>(this));
	};

	IA_WeightShiftForeAft = NewObject<UInputAction>(this, TEXT("IA_WeightShiftForeAft"));
	IA_WeightShiftForeAft->ValueType = EInputActionValueType::Axis1D;
	MappingContext->MapKey(IA_WeightShiftForeAft, EKeys::Gamepad_LeftY); // gamepad path unchanged
	MapDigitalAxisPair(IA_WeightShiftForeAft, EKeys::W, EKeys::Up, EKeys::S, EKeys::Down);

	IA_WeightShiftLateral = NewObject<UInputAction>(this, TEXT("IA_WeightShiftLateral"));
	IA_WeightShiftLateral->ValueType = EInputActionValueType::Axis1D;
	MappingContext->MapKey(IA_WeightShiftLateral, EKeys::Gamepad_RightX); // gamepad path unchanged
	MapDigitalAxisPair(IA_WeightShiftLateral, EKeys::D, EKeys::Right, EKeys::A, EKeys::Left);

	// NON-PHYSICAL game steering channel -- see class comment. Deliberately the same physical
	// inputs as weight_shift_lateral above (lean-to-steer): separate UInputActions bound to the
	// same keys, each producing its own (here, identical) raw value that gets shaped/sent
	// independently.
	IA_Steer = NewObject<UInputAction>(this, TEXT("IA_Steer"));
	IA_Steer->ValueType = EInputActionValueType::Axis1D;
	MappingContext->MapKey(IA_Steer, EKeys::Gamepad_RightX); // gamepad path unchanged
	MapDigitalAxisPair(IA_Steer, EKeys::D, EKeys::Right, EKeys::A, EKeys::Left);

	IA_Arm = NewObject<UInputAction>(this, TEXT("IA_Arm"));
	IA_Arm->ValueType = EInputActionValueType::Boolean;
	MappingContext->MapKey(IA_Arm, EKeys::Gamepad_FaceButton_Bottom); // gamepad path unchanged
	MappingContext->MapKey(IA_Arm, EKeys::SpaceBar);

	IA_Reset = NewObject<UInputAction>(this, TEXT("IA_Reset"));
	IA_Reset->ValueType = EInputActionValueType::Boolean;
	MappingContext->MapKey(IA_Reset, EKeys::Gamepad_FaceButton_Right); // gamepad path unchanged
	MappingContext->MapKey(IA_Reset, EKeys::R);

	IA_Quit = NewObject<UInputAction>(this, TEXT("IA_Quit"));
	IA_Quit->ValueType = EInputActionValueType::Boolean;
	MappingContext->MapKey(IA_Quit, EKeys::Escape);

	// AddMappingContext does NOT happen here -- see ReceivedPlayer() and the header comment on
	// it. This used to be here and silently did nothing whenever GetLocalPlayer() was null at
	// this point, which is exactly what killed both the keyboard AND the gamepad.

	if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent))
	{
		EIC->BindAction(IA_WeightShiftForeAft, ETriggerEvent::Triggered, this, &AOverboardPlayerController::OnWeightShiftForeAft);
		EIC->BindAction(IA_WeightShiftForeAft, ETriggerEvent::Completed, this, &AOverboardPlayerController::OnWeightShiftForeAft);
		EIC->BindAction(IA_WeightShiftLateral, ETriggerEvent::Triggered, this, &AOverboardPlayerController::OnWeightShiftLateral);
		EIC->BindAction(IA_WeightShiftLateral, ETriggerEvent::Completed, this, &AOverboardPlayerController::OnWeightShiftLateral);
		EIC->BindAction(IA_Steer, ETriggerEvent::Triggered, this, &AOverboardPlayerController::OnSteer);
		EIC->BindAction(IA_Steer, ETriggerEvent::Completed, this, &AOverboardPlayerController::OnSteer);
		EIC->BindAction(IA_Arm, ETriggerEvent::Started, this, &AOverboardPlayerController::OnArm);
		EIC->BindAction(IA_Arm, ETriggerEvent::Completed, this, &AOverboardPlayerController::OnArm);
		EIC->BindAction(IA_Reset, ETriggerEvent::Started, this, &AOverboardPlayerController::OnReset);
		EIC->BindAction(IA_Reset, ETriggerEvent::Completed, this, &AOverboardPlayerController::OnReset);
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

// Verbose-level instrumentation on every handler (overboard#162, COO's explicit debugging ask):
// "log the actual value arriving ... if the handler never fires, it is mapping; if it fires with
// zero, it is the action/trigger." Verbose is filtered out by default (no spam in a normal
// session) but is exactly what's needed the next time input goes dark for a reason nobody
// logged -- raise LogOverboardInput to Verbose (`Log LogOverboardInput Verbose` in the in-game
// console, or -LogCmds="LogOverboardInput Verbose" on the command line) to see it.
void AOverboardPlayerController::OnWeightShiftForeAft(const FInputActionValue& Value) { CurrentForeAft = Value.Get<float>(); UE_LOG(LogOverboardInput, Verbose, TEXT("OnWeightShiftForeAft: %.3f"), CurrentForeAft); }
void AOverboardPlayerController::OnWeightShiftLateral(const FInputActionValue& Value) { CurrentLateral = Value.Get<float>(); UE_LOG(LogOverboardInput, Verbose, TEXT("OnWeightShiftLateral: %.3f"), CurrentLateral); }
void AOverboardPlayerController::OnSteer(const FInputActionValue& Value) { CurrentSteer = Value.Get<float>(); UE_LOG(LogOverboardInput, Verbose, TEXT("OnSteer: %.3f"), CurrentSteer); }
void AOverboardPlayerController::OnArm(const FInputActionValue& Value) { bArmHeld = Value.Get<bool>(); UE_LOG(LogOverboardInput, Verbose, TEXT("OnArm: %s"), bArmHeld ? TEXT("true") : TEXT("false")); }
void AOverboardPlayerController::OnReset(const FInputActionValue& Value) { bResetHeld = Value.Get<bool>(); UE_LOG(LogOverboardInput, Verbose, TEXT("OnReset: %s"), bResetHeld ? TEXT("true") : TEXT("false")); }

void AOverboardPlayerController::OnQuit(const FInputActionValue& Value)
{
	if (Value.Get<bool>())
	{
		UE_LOG(LogOverboardInput, Log, TEXT("AOverboardPlayerController: Escape pressed, quitting."));
		ConsoleCommand(TEXT("quit"));
	}
}

void AOverboardPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	CheckForAutoResetOnFall();

	// Ramp toward whatever Enhanced Input reported this frame -- see KeyboardRampSpeed's comment.
	// For the gamepad, Current* already moves gradually, so this is a near no-op; for the
	// keyboard, Current* snaps between 0 and +-1 and this is what turns that into a ramp.
	SmoothedForeAft = FMath::FInterpTo(SmoothedForeAft, CurrentForeAft, DeltaTime, KeyboardRampSpeed);
	SmoothedLateral = FMath::FInterpTo(SmoothedLateral, CurrentLateral, DeltaTime, KeyboardRampSpeed);
	SmoothedSteer = FMath::FInterpTo(SmoothedSteer, CurrentSteer, DeltaTime, KeyboardRampSpeed);

	// Send at frame rate (#162 W2 dispatch) -- no accumulator/throttle. The wire has no framing
	// beyond seq, so sending every Tick is both the simplest thing and what was asked for; if
	// this ever needs decoupling from render rate, that's a deliberate follow-up, not a default.
	SendInputPacket();
}

void AOverboardPlayerController::CheckForAutoResetOnFall()
{
	const AOverboardGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AOverboardGameMode>() : nullptr;
	const ABoardActor* Board = GameMode ? GameMode->GetSpawnedBoard() : nullptr;
	if (!Board)
	{
		return; // board not spawned yet -- nothing to check
	}

	// ADR-0012: never auto-reset out of a physics handoff.
	//
	// A handoff almost always coincides with FALLEN, so without this the rising edge below would
	// fire Reset on the very next tick, the host would clear its latch, and the crash would end
	// roughly one frame after it began -- the player would see the board twitch and respawn
	// rather than tumble. Reset during a handoff is the PLAYER's call, and bResetHeld still
	// carries it: this suppresses the automatic one only.
	//
	// bWasFallenLastTick is still tracked through the handoff so that clearing it does not
	// immediately re-trigger on an edge that was never serviced.
	const bool bIsFallenNow = Board->IsFallen();
	if (Board->IsPhysicsHandoff())
	{
		bWasFallenLastTick = bIsFallenNow;
		return;
	}

	if (bIsFallenNow && !bWasFallenLastTick)
	{
		// Rising edge only -- fires once per fall, not held for as long as Fallen stays set.
		bAutoResetPending = true;
		UE_LOG(LogOverboardInput, Log, TEXT("AOverboardPlayerController: board fell, sending one Reset."));
	}
	bWasFallenLastTick = bIsFallenNow;
}

float AOverboardPlayerController::ShapeAxis(float Raw) const
{
	const float Abs = FMath::Abs(Raw);
	if (Abs < StickDeadzone)
	{
		return 0.f;
	}
	// Rescale [Deadzone, 1] -> [0, 1] so the curve starts at 0 right past the deadzone edge
	// instead of jumping straight to a nonzero command the instant the stick leaves centre.
	const float Rescaled = (Abs - StickDeadzone) / (1.f - StickDeadzone);
	const float Curved = FMath::Pow(Rescaled, ResponseCurveExponent);
	return FMath::Sign(Raw) * Curved;
}

void AOverboardPlayerController::SendInputPacket()
{
	if (!SendSocket || !HostAddr.IsValid())
	{
		return;
	}

	OverboardWire::FInputPacket Packet;
	Packet.Seq = SendSeq++; // monotonic for the life of the socket, regardless of arm/reset state
	const bool bSendReset = bResetHeld || bAutoResetPending;
	bAutoResetPending = false; // one-shot: consumed the instant it's sent, never held
	Packet.Flags = (bArmHeld ? OverboardWire::EInputFlags::Arm : 0) | (bSendReset ? OverboardWire::EInputFlags::Reset : 0);
	// Ramped (Smoothed*, not Current* -- see KeyboardRampSpeed), then deadzone + curve shape,
	// then clamp is the final, unconditional step before the wire -- do not rely on the host to
	// sanitise, even though it does (#162 W2 dispatch).
	Packet.WeightShiftForeAft = FMath::Clamp(ShapeAxis(SmoothedForeAft), -1.f, 1.f);
	Packet.WeightShiftLateral = FMath::Clamp(ShapeAxis(SmoothedLateral), -1.f, 1.f);
	Packet.Steer = FMath::Clamp(ShapeAxis(SmoothedSteer), -1.f, 1.f); // NON-PHYSICAL

	uint8 Buf[OverboardWire::kInputPacketWireSize];
	OverboardWire::EncodeInputPacket(Packet, Buf);

	int32 BytesSent = 0;
	SendSocket->SendTo(Buf, sizeof(Buf), BytesSent, *HostAddr);
}

void AOverboardPlayerController::RunInputSelfTest()
{
	UE_LOG(LogOverboardInput, Log, TEXT("=== OverboardInputSelfTest: BEGIN ==="));

	if (!PlayerInput)
	{
		UE_LOG(LogOverboardInput, Error, TEXT("OverboardInputSelfTest: FAIL -- PlayerInput is null, cannot inject a key event."));
		return;
	}

	// Keyboard phase: a real 'W pressed' event through the actual Enhanced Input pipeline --
	// mapping context lookup, trigger evaluation, the Negate-modified paired key, our handler,
	// the ramp, the real wire send -- via the engine's own simulated-input mechanism, not a
	// hand-rolled shortcut that could pass while the real path stays broken.
	UE_LOG(LogOverboardInput, Log, TEXT("OverboardInputSelfTest: [1/2 keyboard] baseline -- CurrentForeAft=%.3f SmoothedForeAft=%.3f"), CurrentForeAft, SmoothedForeAft);
	PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W, IE_Pressed, 1.0f));
	UE_LOG(LogOverboardInput, Log, TEXT("OverboardInputSelfTest: [1/2 keyboard] injected 'W pressed'. Checking again shortly (processed on a later frame, not synchronously)."));

	FTimerHandle CheckKeyboardHandle;
	GetWorldTimerManager().SetTimer(CheckKeyboardHandle, [this]()
	{
		const bool bHandlerFired = !FMath::IsNearlyZero(CurrentForeAft);
		const bool bReachedWire = !FMath::IsNearlyZero(SmoothedForeAft);
		UE_LOG(LogOverboardInput, Log, TEXT("OverboardInputSelfTest: [1/2 keyboard] after settle -- CurrentForeAft=%.3f (handler %s) SmoothedForeAft=%.3f (ramped/wire value %s)"),
			CurrentForeAft, bHandlerFired ? TEXT("FIRED") : TEXT("NEVER FIRED -- mapping context problem"),
			SmoothedForeAft, bReachedWire ? TEXT("NON-ZERO -- PASS") : TEXT("still zero -- FAIL"));
		UE_LOG(LogOverboardInput, Log, TEXT("=== OverboardInputSelfTest KEYBOARD: %s ==="), bReachedWire ? TEXT("PASS") : TEXT("FAIL"));

		if (PlayerInput)
		{
			PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W, IE_Released, 0.0f));
		}

		// Gamepad phase, per the COO's explicit ask to confirm this fix covers both paths --
		// same mapping context, same InputComponent/PlayerInput classes, so the same root cause
		// should have broken both, and the same fix should cover both. Confirmed, not assumed.
		FTimerHandle GamepadInjectHandle;
		GetWorldTimerManager().SetTimer(GamepadInjectHandle, [this]()
		{
			UE_LOG(LogOverboardInput, Log, TEXT("OverboardInputSelfTest: [2/2 gamepad] baseline -- CurrentForeAft=%.3f SmoothedForeAft=%.3f"), CurrentForeAft, SmoothedForeAft);
			if (PlayerInput)
			{
				PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::Gamepad_LeftY, IE_Axis, 0.6f));
			}
			UE_LOG(LogOverboardInput, Log, TEXT("OverboardInputSelfTest: [2/2 gamepad] injected Gamepad_LeftY=0.6. Checking again shortly."));

			FTimerHandle CheckGamepadHandle;
			GetWorldTimerManager().SetTimer(CheckGamepadHandle, [this]()
			{
				const bool bHandlerFired = !FMath::IsNearlyZero(CurrentForeAft);
				const bool bReachedWire = !FMath::IsNearlyZero(SmoothedForeAft);
				UE_LOG(LogOverboardInput, Log, TEXT("OverboardInputSelfTest: [2/2 gamepad] after settle -- CurrentForeAft=%.3f (handler %s) SmoothedForeAft=%.3f (ramped/wire value %s)"),
					CurrentForeAft, bHandlerFired ? TEXT("FIRED") : TEXT("NEVER FIRED"),
					SmoothedForeAft, bReachedWire ? TEXT("NON-ZERO -- PASS") : TEXT("still zero -- FAIL"));
				UE_LOG(LogOverboardInput, Log, TEXT("=== OverboardInputSelfTest GAMEPAD: %s ==="), bReachedWire ? TEXT("PASS") : TEXT("FAIL"));

				if (PlayerInput)
				{
					PlayerInput->InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::Gamepad_LeftY, IE_Axis, 0.0f));
				}

				// Self-test mode is a one-shot headless check, never a normal play session --
				// quit afterward rather than leaving an -unattended process running indefinitely
				// in the background (bit a verification pass once already: four stray instances
				// kept sending packets for minutes after their self-tests had long finished).
				FTimerHandle QuitHandle;
				GetWorldTimerManager().SetTimer(QuitHandle, [this]()
				{
					UE_LOG(LogOverboardInput, Log, TEXT("OverboardInputSelfTest: done, quitting."));
					ConsoleCommand(TEXT("quit"));
				}, 1.0f, false);
			}, 0.5f, false);
		}, 0.5f, false);
	}, 0.5f, false);
}
