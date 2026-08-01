#include "OverboardPlayerController.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputMappingContext.h"
#include "InputAction.h"
#include "InputTriggers.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "OverboardWire.h"
#include "OverboardGameMode.h"
#include "BoardActor.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogOverboardInput, Log, All);

namespace
{
	constexpr int32 kHostPort = 9602; // host LISTENS here, so we SEND to it
}

AOverboardPlayerController::AOverboardPlayerController()
{
	PrimaryActorTick.bCanEverTick = true;
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
	Super::SetupInputComponent();

	// Built at runtime rather than as .uasset data assets -- still no editor session available
	// to author Input Mapping Context assets as of W2 either. See class header for the W2
	// mapping and why right-stick-X legitimately drives two wire channels at once.
	MappingContext = NewObject<UInputMappingContext>(this, TEXT("OverboardMappingContext"));

	IA_WeightShiftForeAft = NewObject<UInputAction>(this, TEXT("IA_WeightShiftForeAft"));
	IA_WeightShiftForeAft->ValueType = EInputActionValueType::Axis1D;
	MappingContext->MapKey(IA_WeightShiftForeAft, EKeys::Gamepad_LeftY);

	IA_WeightShiftLateral = NewObject<UInputAction>(this, TEXT("IA_WeightShiftLateral"));
	IA_WeightShiftLateral->ValueType = EInputActionValueType::Axis1D;
	MappingContext->MapKey(IA_WeightShiftLateral, EKeys::Gamepad_RightX);

	// NON-PHYSICAL game steering channel -- see class comment. Deliberately the same physical
	// axis as weight_shift_lateral above (lean-to-steer): two UInputActions bound to one key,
	// each producing its own (here, identical) raw value that gets shaped/sent independently.
	IA_Steer = NewObject<UInputAction>(this, TEXT("IA_Steer"));
	IA_Steer->ValueType = EInputActionValueType::Axis1D;
	MappingContext->MapKey(IA_Steer, EKeys::Gamepad_RightX);

	IA_Arm = NewObject<UInputAction>(this, TEXT("IA_Arm"));
	IA_Arm->ValueType = EInputActionValueType::Boolean;
	MappingContext->MapKey(IA_Arm, EKeys::Gamepad_FaceButton_Bottom);

	IA_Reset = NewObject<UInputAction>(this, TEXT("IA_Reset"));
	IA_Reset->ValueType = EInputActionValueType::Boolean;
	MappingContext->MapKey(IA_Reset, EKeys::Gamepad_FaceButton_Right);

	if (ULocalPlayer* LocalPlayer = GetLocalPlayer())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			Subsystem->AddMappingContext(MappingContext, 0);
		}
	}

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
	}
	else
	{
		UE_LOG(LogOverboardInput, Error, TEXT("AOverboardPlayerController: InputComponent is not an EnhancedInputComponent -- check project Enhanced Input settings."));
	}
}

void AOverboardPlayerController::OnWeightShiftForeAft(const FInputActionValue& Value) { CurrentForeAft = Value.Get<float>(); }
void AOverboardPlayerController::OnWeightShiftLateral(const FInputActionValue& Value) { CurrentLateral = Value.Get<float>(); }
void AOverboardPlayerController::OnSteer(const FInputActionValue& Value) { CurrentSteer = Value.Get<float>(); }
void AOverboardPlayerController::OnArm(const FInputActionValue& Value) { bArmHeld = Value.Get<bool>(); }
void AOverboardPlayerController::OnReset(const FInputActionValue& Value) { bResetHeld = Value.Get<bool>(); }

void AOverboardPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	CheckForAutoResetOnFall();

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

	const bool bIsFallenNow = Board->IsFallen();
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
	// Deadzone + curve shape the raw stick, then clamp is the final, unconditional step before
	// the wire -- do not rely on the host to sanitise, even though it does (#162 W2 dispatch).
	Packet.WeightShiftForeAft = FMath::Clamp(ShapeAxis(CurrentForeAft), -1.f, 1.f);
	Packet.WeightShiftLateral = FMath::Clamp(ShapeAxis(CurrentLateral), -1.f, 1.f);
	Packet.Steer = FMath::Clamp(ShapeAxis(CurrentSteer), -1.f, 1.f); // NON-PHYSICAL

	uint8 Buf[OverboardWire::kInputPacketWireSize];
	OverboardWire::EncodeInputPacket(Packet, Buf);

	int32 BytesSent = 0;
	SendSocket->SendTo(Buf, sizeof(Buf), BytesSent, *HostAddr);
}
