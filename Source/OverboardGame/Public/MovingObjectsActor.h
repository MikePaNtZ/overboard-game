// MovingObjectsActor.h
//
// Draws the Level 2 phase B traffic: one visual per moving object (car, pedestrian, cyclist).
// THE RULE (ADR-0009): this actor computes no physics. sim-host (MuJoCo) moves every object on a
// scripted path and decides every contact; this actor only receives the OBJS poses (through
// FMovingObjectsClient) and draws them, one render-delay behind the wall clock and interpolated,
// exactly as ABoardActor draws the board. StateOut is untouched.
//
// The object TABLE (id -> kind, full box size) comes from the course objects.json (the same file
// sim-host reads). The visual for each id is scaled to fit INSIDE that box, so what the player
// sees equals what the board can hit.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MovingObjectsClient.h"
#include "MovingObjectsActor.generated.h"

class ABoardActor;
class UStaticMesh;
class UMaterialInterface;

UCLASS()
class OVERBOARDGAME_API AMovingObjectsActor : public AActor
{
	GENERATED_BODY()

public:
	AMovingObjectsActor();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	// Loads the object table from <ObjectsFilePath>, starts the OBJS client, and builds one
	// visual per id. False if the file is missing or empty (the caller then destroys the actor).
	bool LoadObjects(const FString& ObjectsFilePath);

	// The newest OBJS frame (raw MuJoCo poses). False before the first packet. For the demo
	// rider's give-way, which needs the current positions, not the render-delayed draw pose.
	bool GetLatestFrame(FMovingObjectsFrame& OutFrame) const;

	// The newest objects with estimated velocities, for the demo rider's predictive give-way.
	bool GetObjectVelocities(TArray<FMovingObjectVel>& OutObjects) const;

	// FPlatformTime::Seconds() of the last object touching-bit 0 -> 1 rise (any object), and the
	// running count. For the HUD "CONTACT" toast and the self-test's contact count.
	double GetLastContactRiseSeconds() const;
	int32 GetContactRiseCount() const;

protected:
	UPROPERTY(VisibleAnywhere, Category = "Objects")
	TObjectPtr<USceneComponent> SceneRoot;

	// One render-delay behind the wall clock, so each object draws from two real samples and
	// never extrapolates. Matches ABoardActor::RenderDelaySeconds (see its comment for the tune).
	UPROPERTY(EditAnywhere, Category = "Objects")
	float RenderDelaySeconds = 0.012f;

private:
	// The static table for one object, read from objects.json once.
	struct FObjectDef
	{
		uint16 Id = 0;
		uint8 Kind = 0;           // 0 car, 1 pedestrian, 2 cyclist
		FVector SizeM = FVector(1.f, 1.f, 1.f); // full box size (lx, ly, lz), metres
	};

	TArray<FObjectDef> Defs;
	TMap<uint16, int32> IdToSlot; // object id -> index into Defs / Slots

	// One scene component per object; the kind's mesh(es) attach under it. The slot carries the
	// interpolated world pose; the children carry only the kind's shape, sized to the box.
	UPROPERTY(Transient)
	TArray<TObjectPtr<USceneComponent>> Slots;

	TUniquePtr<FMovingObjectsClient> Client;

	// The board, for the shared level origin (offset + yaw). The whole frame is rotated by the
	// board's WorldOriginYawDeg and moved to its WorldOriginOffsetCm, so objects line up with the
	// board and the course elements. Cached on the first tick that finds it.
	TWeakObjectPtr<ABoardActor> Board;

	// Cached engine shapes and material for the proxies and the fit math.
	UPROPERTY(Transient) TObjectPtr<UStaticMesh> CubeMesh;
	UPROPERTY(Transient) TObjectPtr<UStaticMesh> CylinderMesh;
	UPROPERTY(Transient) TObjectPtr<UStaticMesh> SphereMesh;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> BaseMaterial;

	void BuildVisual(USceneComponent* Slot, const FObjectDef& Def);
	void BuildCar(USceneComponent* Slot, const FObjectDef& Def);
	void BuildPedestrian(USceneComponent* Slot, const FObjectDef& Def);
	void BuildCyclist(USceneComponent* Slot, const FObjectDef& Def);

	// Adds a coloured engine-cube box of SizeM (metres), centred at CentreLocalCm in the slot's
	// frame. The engine cube is 1 m on a side, so the scale is SizeM directly.
	void AddBox(USceneComponent* Slot, const FVector& SizeM, const FVector& CentreLocalCm, const FLinearColor& Color);
	// Adds a coloured engine cylinder (1 m tall, 1 m across) scaled to DiameterM x HeightM.
	void AddCylinder(USceneComponent* Slot, float DiameterM, float HeightM, const FVector& CentreLocalCm, const FLinearColor& Color, const FRotator& Rot = FRotator::ZeroRotator);
	void AddSphere(USceneComponent* Slot, float DiameterM, const FVector& CentreLocalCm, const FLinearColor& Color);

	// Attaches RealMesh under Slot, scaled uniformly to fit inside BoxM and centred on the slot.
	// Logs a warning (and keeps the fit scale) if the scaled mesh still exceeds the box.
	void AddFittedMesh(USceneComponent* Slot, UStaticMesh* RealMesh, const FVector& BoxM, const FObjectDef& Def);

	// Applies the interpolated pose of every object to its slot.
	bool bLoggedFirstFrame = false;
	void UpdatePosesFromHistory();

	// Where the level origin sits, from the board (zero if no board yet).
	FVector OriginOffsetCm() const;
	float OriginYawDeg() const;
};
