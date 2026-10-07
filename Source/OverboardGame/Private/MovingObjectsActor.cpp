#include "MovingObjectsActor.h"
#include "BoardActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"
#include "EngineUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonSerializer.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogMovingObjects, Log, All);

namespace
{
	// MuJoCo (x, y, z) metres -> Unreal (100x, -100y, 100z) centimetres, the same frame
	// ABoardActor and ARideCourseElements use. The board's WorldOriginYaw/Offset are applied on
	// top in OriginYaw/OriginOffset, so objects line up with the board.
	FVector MuJoCoToLocalCm(float X, float Y, float Z)
	{
		return FVector(100.f * X, -100.f * Y, 100.f * Z);
	}

	// A few City Sample car bodies (static meshes), copied into Content/Vehicle (gitignored) by
	// tools/metahuman/copy_vault_closure.py. If none resolve -- the usual headless/CI case -- the
	// car falls back to a clean car-shaped proxy. Varying the mesh by id keeps the street from
	// looking like one cloned car.
	const TCHAR* kCarMeshPaths[] = {
		TEXT("/Game/Vehicle/vehCar_vehicle03/Mesh/SM_vehCar_vehicle03.SM_vehCar_vehicle03"),
		TEXT("/Game/Vehicle/vehCar_vehicle05/Mesh/SM_vehCar_vehicle05.SM_vehCar_vehicle05"),
		TEXT("/Game/Vehicle/vehCar_vehicle07/Mesh/SM_vehCar_vehicle07.SM_vehCar_vehicle07"),
		TEXT("/Game/Vehicle/vehVan_vehicle09/Mesh/SM_vehVan_vehicle09.SM_vehVan_vehicle09"),
	};
}

AMovingObjectsActor::AMovingObjectsActor()
{
	PrimaryActorTick.bCanEverTick = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = SceneRoot;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	CubeMesh = Cube.Succeeded() ? Cube.Object : nullptr;
	CylinderMesh = Cylinder.Succeeded() ? Cylinder.Object : nullptr;
	SphereMesh = Sphere.Succeeded() ? Sphere.Object : nullptr;
	BaseMaterial = Material.Succeeded() ? Material.Object : nullptr;
}

bool AMovingObjectsActor::LoadObjects(const FString& ObjectsFilePath)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *ObjectsFilePath))
	{
		UE_LOG(LogMovingObjects, Warning, TEXT("MovingObjectsActor: cannot read %s"), *ObjectsFilePath);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogMovingObjects, Warning, TEXT("MovingObjectsActor: cannot parse %s"), *ObjectsFilePath);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Objects = nullptr;
	if (!Root->TryGetArrayField(TEXT("objects"), Objects) || Objects->Num() == 0)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& V : *Objects)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		if (!O.IsValid())
		{
			continue;
		}
		FObjectDef Def;
		Def.Id = static_cast<uint16>(O->GetIntegerField(TEXT("id")));
		const FString KindStr = O->GetStringField(TEXT("kind"));
		Def.Kind = KindStr == TEXT("pedestrian") ? 1 : (KindStr == TEXT("cyclist") ? 2 : 0);
		const TArray<TSharedPtr<FJsonValue>>* Size = nullptr;
		if (O->TryGetArrayField(TEXT("size"), Size) && Size->Num() == 3)
		{
			Def.SizeM = FVector((*Size)[0]->AsNumber(), (*Size)[1]->AsNumber(), (*Size)[2]->AsNumber());
		}
		IdToSlot.Add(Def.Id, Defs.Num());
		Defs.Add(Def);
	}

	// One slot per object, with the kind's visual under it.
	for (const FObjectDef& Def : Defs)
	{
		USceneComponent* Slot = NewObject<USceneComponent>(this);
		Slot->SetupAttachment(SceneRoot);
		Slot->RegisterComponent();
		BuildVisual(Slot, Def);
		Slots.Add(Slot);
	}

	Client = MakeUnique<FMovingObjectsClient>();
	Client->StartListening();

	UE_LOG(LogMovingObjects, Log, TEXT("MovingObjectsActor: %d objects from %s"), Defs.Num(), *ObjectsFilePath);
	return true;
}

void AMovingObjectsActor::BeginPlay()
{
	Super::BeginPlay();
}

void AMovingObjectsActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Client.IsValid())
	{
		Client->Shutdown();
		Client.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void AMovingObjectsActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Board.IsValid())
	{
		for (TActorIterator<ABoardActor> It(GetWorld()); It; ++It)
		{
			Board = *It;
			break;
		}
	}
	UpdatePosesFromHistory();
}

FVector AMovingObjectsActor::OriginOffsetCm() const
{
	return Board.IsValid() ? Board->GetWorldOriginOffsetCm() : FVector::ZeroVector;
}

float AMovingObjectsActor::OriginYawDeg() const
{
	return Board.IsValid() ? Board->GetWorldOriginYawDeg() : 0.f;
}

void AMovingObjectsActor::UpdatePosesFromHistory()
{
	if (!Client.IsValid())
	{
		return;
	}
	TArray<FMovingObjectsFrame> History;
	Client->GetHistorySnapshot(History);
	if (History.Num() == 0)
	{
		return; // nothing received yet -- hold the spawn pose, do not guess
	}

	if (!bLoggedFirstFrame)
	{
		bLoggedFirstFrame = true;
		UE_LOG(LogMovingObjects, Log, TEXT("OBJRECV %d objects in the first OBJS frame"), History.Last().Objects.Num());
	}

	const double RenderTime = FPlatformTime::Seconds() - static_cast<double>(RenderDelaySeconds);

	// Find the bracket [Lo, Lo+1] of frames around RenderTime. One frame behind, never
	// extrapolate: clamp to an end if RenderTime is outside the window. Same rule as ABoardActor.
	int32 Lo = -1;
	for (int32 i = 0; i < History.Num() - 1; ++i)
	{
		if (History[i].ArrivalTimeSeconds <= RenderTime && RenderTime <= History[i + 1].ArrivalTimeSeconds)
		{
			Lo = i;
			break;
		}
	}

	const FQuat OriginYaw(FRotator(0.f, OriginYawDeg(), 0.f));
	const FVector Offset = OriginOffsetCm();

	auto FindSample = [](const FMovingObjectsFrame& Frame, uint16 Id, FMovingObjectSample& Out) -> bool
	{
		for (const FMovingObjectSample& S : Frame.Objects)
		{
			if (S.Id == Id) { Out = S; return true; }
		}
		return false;
	};

	auto Place = [&](int32 SlotIdx, const FVector& LocalCm, float YawMujocoRad)
	{
		const FVector World = Offset + OriginYaw.RotateVector(LocalCm);
		// UE yaw = -MuJoCo yaw, then rotated by the level origin yaw.
		const float WorldYaw = OriginYawDeg() - FMath::RadiansToDegrees(YawMujocoRad);
		Slots[SlotIdx]->SetWorldLocationAndRotation(World, FRotator(0.f, WorldYaw, 0.f));
	};

	for (int32 SlotIdx = 0; SlotIdx < Defs.Num(); ++SlotIdx)
	{
		const uint16 Id = Defs[SlotIdx].Id;

		if (Lo >= 0)
		{
			const FMovingObjectsFrame& A = History[Lo];
			const FMovingObjectsFrame& B = History[Lo + 1];
			FMovingObjectSample SA, SB;
			const bool bHaveA = FindSample(A, Id, SA);
			const bool bHaveB = FindSample(B, Id, SB);
			if (bHaveA && bHaveB)
			{
				const double Span = B.ArrivalTimeSeconds - A.ArrivalTimeSeconds;
				const float Alpha = Span > 0.0 ? FMath::Clamp(static_cast<float>((RenderTime - A.ArrivalTimeSeconds) / Span), 0.f, 1.f) : 0.f;
				const FVector PosA = MuJoCoToLocalCm(SA.Pos[0], SA.Pos[1], SA.Pos[2]);
				const FVector PosB = MuJoCoToLocalCm(SB.Pos[0], SB.Pos[1], SB.Pos[2]);
				const float YawA = SA.Yaw;
				const float YawB = SA.Yaw + FMath::UnwindRadians(SB.Yaw - SA.Yaw);
				Place(SlotIdx, FMath::Lerp(PosA, PosB, Alpha), FMath::Lerp(YawA, YawB, Alpha));
				continue;
			}
			if (bHaveA) { Place(SlotIdx, MuJoCoToLocalCm(SA.Pos[0], SA.Pos[1], SA.Pos[2]), SA.Yaw); continue; }
			if (bHaveB) { Place(SlotIdx, MuJoCoToLocalCm(SB.Pos[0], SB.Pos[1], SB.Pos[2]), SB.Yaw); continue; }
		}

		// Outside the window (or the id was missing from the bracket): clamp to the nearest end.
		const FMovingObjectsFrame& Clamp = (RenderTime < History[0].ArrivalTimeSeconds) ? History[0] : History.Last();
		FMovingObjectSample S;
		if (FindSample(Clamp, Id, S))
		{
			Place(SlotIdx, MuJoCoToLocalCm(S.Pos[0], S.Pos[1], S.Pos[2]), S.Yaw);
		}
	}
}

bool AMovingObjectsActor::GetLatestFrame(FMovingObjectsFrame& OutFrame) const
{
	return Client.IsValid() && Client->GetLatestFrame(OutFrame);
}

bool AMovingObjectsActor::GetObjectVelocities(TArray<FMovingObjectVel>& OutObjects) const
{
	return Client.IsValid() && Client->GetObjectVelocities(OutObjects);
}

double AMovingObjectsActor::GetLastContactRiseSeconds() const
{
	return Client.IsValid() ? Client->GetLastContactRiseSeconds() : 0.0;
}

int32 AMovingObjectsActor::GetContactRiseCount() const
{
	return Client.IsValid() ? Client->GetContactRiseCount() : 0;
}

// ---- Visuals ---------------------------------------------------------------------------------

void AMovingObjectsActor::BuildVisual(USceneComponent* Slot, const FObjectDef& Def)
{
	switch (Def.Kind)
	{
	case 1: BuildPedestrian(Slot, Def); break;
	case 2: BuildCyclist(Slot, Def); break;
	default: BuildCar(Slot, Def); break;
	}
}

void AMovingObjectsActor::BuildCar(USceneComponent* Slot, const FObjectDef& Def)
{
	// Prefer a real City Sample car body if one is present in Content; else a clean proxy.
	UStaticMesh* Real = nullptr;
	const int32 N = UE_ARRAY_COUNT(kCarMeshPaths);
	const int32 Pick = N > 0 ? (Def.Id % N) : 0;
	for (int32 k = 0; k < N && Real == nullptr; ++k)
	{
		const int32 Idx = (Pick + k) % N;
		Real = LoadObject<UStaticMesh>(nullptr, kCarMeshPaths[Idx]);
	}
	if (Real)
	{
		AddFittedMesh(Slot, Real, Def.SizeM, Def);
		return;
	}

	// Proxy: a lower body box filling the footprint, plus a shorter cabin box on top. The pair
	// reads as a car and stays inside the MuJoCo box by construction.
	const FLinearColor Body(0.15f, 0.22f, 0.45f, 1.f);
	const FLinearColor Cabin(0.55f, 0.75f, 0.9f, 1.f);
	const float BodyHM = Def.SizeM.Z * 0.55f;
	const float CabinHM = Def.SizeM.Z - BodyHM;
	AddBox(Slot, FVector(Def.SizeM.X, Def.SizeM.Y, BodyHM), FVector(0.f, 0.f, 100.f * (BodyHM * 0.5f - Def.SizeM.Z * 0.5f)), Body);
	AddBox(Slot, FVector(Def.SizeM.X * 0.55f, Def.SizeM.Y * 0.9f, CabinHM), FVector(0.f, 0.f, 100.f * (BodyHM + CabinHM * 0.5f - Def.SizeM.Z * 0.5f)), Cabin);
}

void AMovingObjectsActor::BuildPedestrian(USceneComponent* Slot, const FObjectDef& Def)
{
	// No mannequin/crowd content is reliably present headless, so a capsule proxy: a body
	// cylinder plus a head sphere, sized to the box. See the task note on the animation fallback.
	const FLinearColor Body(0.85f, 0.45f, 0.1f, 1.f);
	const FLinearColor Head(0.95f, 0.8f, 0.65f, 1.f);
	const float HeadDiaM = FMath::Min(Def.SizeM.X, Def.SizeM.Y) * 0.8f;
	const float BodyHM = Def.SizeM.Z - HeadDiaM;
	const float BodyDiaM = FMath::Min(Def.SizeM.X, Def.SizeM.Y);
	AddCylinder(Slot, BodyDiaM, BodyHM, FVector(0.f, 0.f, 100.f * (BodyHM * 0.5f - Def.SizeM.Z * 0.5f)), Body);
	AddSphere(Slot, HeadDiaM, FVector(0.f, 0.f, 100.f * (Def.SizeM.Z * 0.5f - HeadDiaM * 0.5f)), Head);
}

void AMovingObjectsActor::BuildCyclist(USceneComponent* Slot, const FObjectDef& Def)
{
	// A bike + rider proxy: two thin wheel discs along the length, a frame box, and a rider box.
	const FLinearColor Frame(0.1f, 0.1f, 0.12f, 1.f);
	const FLinearColor Rider(0.15f, 0.55f, 0.2f, 1.f);
	const float WheelDiaM = FMath::Min(Def.SizeM.Z * 0.45f, Def.SizeM.X * 0.4f);
	const float WheelZ = 100.f * (WheelDiaM * 0.5f - Def.SizeM.Z * 0.5f);
	const float WheelX = 100.f * (Def.SizeM.X * 0.35f);
	// The engine cylinder's axis is local Z; roll it 90 deg about X so the disc faces sideways.
	AddCylinder(Slot, WheelDiaM, Def.SizeM.Y * 0.1f, FVector(+WheelX, 0.f, WheelZ), Frame, FRotator(90.f, 0.f, 0.f));
	AddCylinder(Slot, WheelDiaM, Def.SizeM.Y * 0.1f, FVector(-WheelX, 0.f, WheelZ), Frame, FRotator(90.f, 0.f, 0.f));
	// Frame bar between the wheels.
	AddBox(Slot, FVector(Def.SizeM.X * 0.7f, Def.SizeM.Y * 0.15f, WheelDiaM * 0.5f), FVector(0.f, 0.f, WheelZ + 100.f * WheelDiaM * 0.3f), Frame);
	// Rider sitting above the frame.
	const float RiderHM = Def.SizeM.Z - WheelDiaM;
	AddBox(Slot, FVector(Def.SizeM.X * 0.3f, Def.SizeM.Y * 0.6f, RiderHM), FVector(0.f, 0.f, 100.f * (WheelDiaM + RiderHM * 0.5f - Def.SizeM.Z * 0.5f)), Rider);
}

void AMovingObjectsActor::AddBox(USceneComponent* Slot, const FVector& SizeM, const FVector& CentreLocalCm, const FLinearColor& Color)
{
	if (!CubeMesh) { return; }
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
	C->SetStaticMesh(CubeMesh);
	C->SetupAttachment(Slot);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->RegisterComponent();
	C->SetRelativeLocation(CentreLocalCm);
	C->SetWorldScale3D(SizeM); // the engine cube is 1 m on a side
	if (BaseMaterial)
	{
		UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(BaseMaterial, this);
		M->SetVectorParameterValue(TEXT("Color"), Color);
		C->SetMaterial(0, M);
	}
}

void AMovingObjectsActor::AddCylinder(USceneComponent* Slot, float DiameterM, float HeightM, const FVector& CentreLocalCm, const FLinearColor& Color, const FRotator& Rot)
{
	if (!CylinderMesh) { return; }
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
	C->SetStaticMesh(CylinderMesh);
	C->SetupAttachment(Slot);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->RegisterComponent();
	C->SetRelativeLocationAndRotation(CentreLocalCm, Rot);
	C->SetWorldScale3D(FVector(DiameterM, DiameterM, HeightM)); // engine cylinder is 1 m tall, 1 m across
	if (BaseMaterial)
	{
		UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(BaseMaterial, this);
		M->SetVectorParameterValue(TEXT("Color"), Color);
		C->SetMaterial(0, M);
	}
}

void AMovingObjectsActor::AddSphere(USceneComponent* Slot, float DiameterM, const FVector& CentreLocalCm, const FLinearColor& Color)
{
	if (!SphereMesh) { return; }
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
	C->SetStaticMesh(SphereMesh);
	C->SetupAttachment(Slot);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->RegisterComponent();
	C->SetRelativeLocation(CentreLocalCm);
	C->SetWorldScale3D(FVector(DiameterM)); // engine sphere is 1 m across
	if (BaseMaterial)
	{
		UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(BaseMaterial, this);
		M->SetVectorParameterValue(TEXT("Color"), Color);
		C->SetMaterial(0, M);
	}
}

void AMovingObjectsActor::AddFittedMesh(USceneComponent* Slot, UStaticMesh* RealMesh, const FVector& BoxM, const FObjectDef& Def)
{
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
	C->SetStaticMesh(RealMesh);
	C->SetupAttachment(Slot);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->RegisterComponent();

	const FBox Bounds = RealMesh->GetBoundingBox();
	const FVector MeshSizeCm = Bounds.GetSize();
	const FVector BoxCm = BoxM * 100.f;
	// Uniform scale so the mesh fits inside the box on every axis (keep the real proportions).
	const float Scale = FMath::Min3(
		MeshSizeCm.X > 1.f ? BoxCm.X / MeshSizeCm.X : 1.f,
		MeshSizeCm.Y > 1.f ? BoxCm.Y / MeshSizeCm.Y : 1.f,
		MeshSizeCm.Z > 1.f ? BoxCm.Z / MeshSizeCm.Z : 1.f);
	C->SetWorldScale3D(FVector(Scale));
	// Centre the mesh's bounds on the slot origin (its native origin may be at a wheel contact).
	C->SetRelativeLocation(-Bounds.GetCenter() * Scale);

	const FVector ScaledHalf = MeshSizeCm * Scale * 0.5f;
	const FVector BoxHalf = BoxCm * 0.5f;
	if (ScaledHalf.X > BoxHalf.X + 1.f || ScaledHalf.Y > BoxHalf.Y + 1.f || ScaledHalf.Z > BoxHalf.Z + 1.f)
	{
		UE_LOG(LogMovingObjects, Warning,
			TEXT("MovingObjectsActor: id %d mesh exceeds its box (scaled %.0f/%.0f/%.0f cm vs box %.0f/%.0f/%.0f cm)"),
			Def.Id, 2.f * ScaledHalf.X, 2.f * ScaledHalf.Y, 2.f * ScaledHalf.Z, BoxCm.X, BoxCm.Y, BoxCm.Z);
	}
}
