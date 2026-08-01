// OverboardPlayerController.h
//
// W1 scope only: read a gamepad via Enhanced Input and send the OBI1 input packet, to prove the
// reverse channel (player -> host) carries at all. Full stick-to-channel mapping tuning is W2.
//
// steer is explicitly a NON-PHYSICAL game channel (the simulated wheel is a cylinder and cannot
// carve) -- see the wire spec and the repo README's "what is deliberately not physical" section.
// Nothing produced via this channel may be presented as a controls result.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "OverboardPlayerController.generated.h"

class UInputMappingContext;
class UInputAction;
struct FInputActionValue;
class FSocket;
class FInternetAddr;

UCLASS()
class OVERBOARDGAME_API AOverboardPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AOverboardPlayerController();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;

protected:
	// How often we send the input packet, in Hz. Host is authoritative on cadence handling; we
	// just need to prove the channel carries for W1.
	UPROPERTY(EditAnywhere, Category = "Board|Networking")
	float SendRateHz = 60.f;

private:
	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> MappingContext;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_WeightShiftForeAft;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_WeightShiftLateral;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Steer; // NON-PHYSICAL game channel
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Arm;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> IA_Reset;

	float CurrentForeAft = 0.f;
	float CurrentLateral = 0.f;
	float CurrentSteer = 0.f;
	bool bArmHeld = false;
	bool bResetHeld = false;

	FSocket* SendSocket = nullptr;
	TSharedPtr<FInternetAddr> HostAddr;
	uint64 SendSeq = 0;
	float TimeSinceLastSend = 0.f;

	void OnWeightShiftForeAft(const FInputActionValue& Value);
	void OnWeightShiftLateral(const FInputActionValue& Value);
	void OnSteer(const FInputActionValue& Value);
	void OnArm(const FInputActionValue& Value);
	void OnReset(const FInputActionValue& Value);

	void SendInputPacket();
};
