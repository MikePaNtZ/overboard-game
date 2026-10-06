#include "RideCourseElements.h"

#include "BoardActor.h"
#include "OverboardWire.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogRideGame, Log, All);

namespace
{
	// The X7 tyre rolling radius (controls track). Speed = |wheel rate| x radius.
	constexpr double kElementsWheelRadiusM = 0.146;
	// A jump back along the course larger than this is a reset, not a ride.
	constexpr double kResetJumpM = 5.0;
	constexpr double kToastSeconds = 2.5;

	const FLinearColor kGateColor(0.02f, 0.55f, 0.6f);     // teal, the board's underglow colour
	const FLinearColor kFlagRed(0.85f, 0.08f, 0.05f);
	const FLinearColor kFlagBlue(0.05f, 0.2f, 0.85f);
	const FLinearColor kStopColor(1.f, 0.78f, 0.f);
	const FLinearColor kSlowColor(1.f, 0.45f, 0.f);
	const FLinearColor kClimbColor(0.25f, 0.85f, 0.3f);
	const FLinearColor kPoleColor(0.85f, 0.85f, 0.85f);
	const FLinearColor kConeColor(1.f, 0.35f, 0.f);
	const FLinearColor kDebrisColor(0.35f, 0.25f, 0.15f);
	constexpr double kConeBaseM = 0.3;
	constexpr double kConeHeightM = 0.45;
	// A hit: the board's centre comes within this of a cone's axis (half the cone base plus about
	// half the deck width). Scoring only -- MuJoCo decides what the hit does to the board.
	constexpr double kConeHitRadiusM = 0.30;
	constexpr double kDebrisHitMarginM = 0.15;
	const FLinearColor kCheckerColor(0.95f, 0.95f, 0.95f);

	// Formats a lap time as m:ss.s (one tenth), for toasts and the HUD.
	FString FormatLapTime(double Seconds)
	{
		const int32 Tenths = FMath::FloorToInt(FMath::Max(Seconds, 0.0) * 10.0);
		return FString::Printf(TEXT("%d:%02d.%d"), Tenths / 600, (Tenths / 10) % 60, Tenths % 10);
	}
}

ARideCourseElements::ARideCourseElements()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cone(TEXT("/Engine/BasicShapes/Cone.Cone"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	CubeMesh = Cube.Object;
	CylinderMesh = Cylinder.Object;
	ConeMesh = Cone.Object;
	BaseMaterial = Material.Object;
}

void ARideCourseElements::BeginPlay()
{
	Super::BeginPlay();
	SetActorLocation(FVector::ZeroVector);
	SetActorRotation(FRotator::ZeroRotator);
}

bool ARideCourseElements::LoadLayout(const FString& CourseName)
{
	const FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT("tools/play/elements"), CourseName + TEXT(".json"));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		UE_LOG(LogRideGame, Warning, TEXT("RideCourseElements: no layout file at %s -- no game elements."), *Path);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogRideGame, Error, TEXT("RideCourseElements: %s is not valid JSON."), *Path);
		return false;
	}

	// The levels track's 2D courses. "laps" (parking_lot) scores ordered lines plus obstacle hits;
	// "cruise" (embarcadero) has only the start/finish line -- no score panel and no checkpoints.
	// Both use the 2D line loader and the demo_path follower, not the 1D street fields below.
	FString Mode;
	Root->TryGetStringField(TEXT("mode"), Mode);
	if (Mode == TEXT("laps") || Mode == TEXT("cruise"))
	{
		bLapsMode = true;
		bCruiseMode = Mode == TEXT("cruise");
		return LoadLapsLayout(Root);
	}

	// This loader knows the 1D street layout (city_hill: s along the street, a height profile,
	// scores). Any other course without those fields loads with no elements instead of
	// dereferencing a missing field (that would crash the game at load).
	const TSharedPtr<FJsonObject>* ProfilePtr = nullptr;
	const TSharedPtr<FJsonObject>* ScoresPtr = nullptr;
	if (!Root->TryGetObjectField(TEXT("profile"), ProfilePtr) || !Root->TryGetObjectField(TEXT("scores"), ScoresPtr)
		|| !Root->HasTypedField<EJson::Number>(TEXT("start_x_m")))
	{
		UE_LOG(LogRideGame, Warning, TEXT("RideCourseElements: %s is a '%s' course without the 1D street fields; no game elements yet."),
			*Path, Mode.IsEmpty() ? TEXT("unknown") : *Mode);
		return false;
	}
	StartX = Root->GetNumberField(TEXT("start_x_m"));
	LaneHalfWidth = Root->GetNumberField(TEXT("lane_half_width_m"));
	StreetHalfWidth = Root->GetNumberField(TEXT("street_half_width_m"));

	const TSharedPtr<FJsonObject> Profile = *ProfilePtr;
	for (const TSharedPtr<FJsonValue>& V : Profile->GetArrayField(TEXT("s_m"))) { ProfileS.Add(V->AsNumber()); }
	for (const TSharedPtr<FJsonValue>& V : Profile->GetArrayField(TEXT("z_m"))) { ProfileZ.Add(V->AsNumber()); }

	for (const auto& Pair : (*ScoresPtr)->Values)
	{
		Scores.Add(Pair.Key, static_cast<int32>(Pair.Value->AsNumber()));
	}

	for (const TSharedPtr<FJsonValue>& V : Root->GetArrayField(TEXT("elements")))
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FElement E;
		const FString Type = O->GetStringField(TEXT("type"));
		E.Kind = Type == TEXT("gate") ? EKind::Gate
			: Type == TEXT("flag") ? EKind::Flag
			: Type == TEXT("stop_box") ? EKind::StopBox
			: Type == TEXT("slow_zone") ? EKind::SlowZone
			: Type == TEXT("speed_trap") ? EKind::SpeedTrap
			: Type == TEXT("cone") ? EKind::Cone
			: Type == TEXT("debris") ? EKind::Debris
			: Type == TEXT("no_buzz") ? EKind::NoBuzz
			: EKind::Unknown;
		if (E.Kind == EKind::Unknown)
		{
			UE_LOG(LogRideGame, Warning, TEXT("RideCourseElements: skipping element type '%s' (not known to this build)."), *Type);
			continue;
		}
		E.Id = O->GetStringField(TEXT("id"));
		O->TryGetStringField(TEXT("label"), E.Label);
		O->TryGetNumberField(TEXT("s"), E.S);
		O->TryGetNumberField(TEXT("s0"), E.S0);
		O->TryGetNumberField(TEXT("s1"), E.S1);
		O->TryGetNumberField(TEXT("y"), E.Y);
		O->TryGetNumberField(TEXT("half_width"), E.HalfWidth);
		O->TryGetNumberField(TEXT("stop_speed_mps"), E.StopSpeed);
		O->TryGetNumberField(TEXT("tail_pitch_rad"), E.TailPitchRad);
		O->TryGetNumberField(TEXT("max_speed_mps"), E.MaxSpeed);
		O->TryGetNumberField(TEXT("bonus_speed_mps"), E.BonusSpeed);
		O->TryGetNumberField(TEXT("yaw_deg"), E.YawDeg);
		const TArray<TSharedPtr<FJsonValue>>* Size = nullptr;
		if (O->TryGetArrayField(TEXT("size_m"), Size) && Size->Num() == 3)
		{
			E.SizeM = FVector((*Size)[0]->AsNumber(), (*Size)[1]->AsNumber(), (*Size)[2]->AsNumber());
		}
		Elements.Add(E);
	}
	UE_LOG(LogRideGame, Log, TEXT("RideCourseElements: %d elements loaded from %s."), Elements.Num(), *Path);
	return Elements.Num() > 0 && ProfileS.Num() == ProfileZ.Num() && ProfileS.Num() > 1;
}

double ARideCourseElements::HeightAt(double S) const
{
	if (ProfileS.Num() < 2)
	{
		return 0.0;
	}
	if (S <= ProfileS[0]) { return ProfileZ[0]; }
	for (int32 i = 1; i < ProfileS.Num(); ++i)
	{
		if (S <= ProfileS[i])
		{
			const double A = (S - ProfileS[i - 1]) / (ProfileS[i] - ProfileS[i - 1]);
			return FMath::Lerp(ProfileZ[i - 1], ProfileZ[i], A);
		}
	}
	return ProfileZ.Last();
}

FVector ARideCourseElements::ToWorldXY(double X, double Y, double Zm) const
{
	// The same frame ABoardActor uses: MuJoCo (x, y, z) m -> Unreal (100x, -100y, 100z) cm,
	// rotated by the origin yaw and moved to the origin (the level's PlayerStart).
	const FVector Local(100.0 * X, -100.0 * Y, 100.0 * Zm);
	const ABoardActor* B = Board.Get();
	const float Yaw = B ? B->GetWorldOriginYawDeg() : 0.f;
	const FVector Offset = B ? B->GetWorldOriginOffsetCm() : FVector::ZeroVector;
	return Offset + FRotator(0.f, Yaw, 0.f).RotateVector(Local);
}

FVector ARideCourseElements::ToWorld(double S, double Y, double UpM) const
{
	// city_hill 1D path: s = distance along the street (MuJoCo x = StartX - s).
	return ToWorldXY(StartX - S, Y, HeightAt(S) + UpM);
}

bool ARideCourseElements::LoadLapsLayout(const TSharedPtr<FJsonObject>& Root)
{
	Readout.TargetLapSeconds = 0.0;
	Root->TryGetNumberField(TEXT("target_lap_s"), Readout.TargetLapSeconds);

	const TSharedPtr<FJsonObject>* ScoresObj = nullptr;
	if (Root->TryGetObjectField(TEXT("scores"), ScoresObj))
	{
		for (const auto& Pair : (*ScoresObj)->Values)
		{
			Scores.Add(Pair.Key, static_cast<int32>(Pair.Value->AsNumber()));
		}
	}

	for (const TSharedPtr<FJsonValue>& V : Root->GetArrayField(TEXT("elements")))
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		const FString Type = O->GetStringField(TEXT("type"));
		if (Type == TEXT("start_finish") || Type == TEXT("checkpoint"))
		{
			FLapLine L;
			O->TryGetNumberField(TEXT("order"), L.Order);
			O->TryGetStringField(TEXT("id"), L.Id);
			O->TryGetStringField(TEXT("label"), L.Label);
			O->TryGetNumberField(TEXT("x"), L.X);
			O->TryGetNumberField(TEXT("y"), L.Y);
			O->TryGetNumberField(TEXT("z"), L.Z);
			O->TryGetNumberField(TEXT("heading_deg"), L.HeadingDeg);
			O->TryGetNumberField(TEXT("half_width"), L.HalfWidth);
			LapLines.Add(L);
		}
		else if (Type == TEXT("cone") || Type == TEXT("box"))
		{
			FLapObstacle Ob;
			Ob.Kind = Type == TEXT("cone") ? EKind::Cone : EKind::Debris;
			O->TryGetNumberField(TEXT("x"), Ob.X);
			O->TryGetNumberField(TEXT("y"), Ob.Y);
			O->TryGetNumberField(TEXT("z"), Ob.Z);
			O->TryGetNumberField(TEXT("yaw_deg"), Ob.YawDeg);
			O->TryGetBoolField(TEXT("draw"), Ob.bDraw);
			const TArray<TSharedPtr<FJsonValue>>* Size = nullptr;
			if (O->TryGetArrayField(TEXT("size_m"), Size) && Size->Num() == 3)
			{
				Ob.SizeM = FVector((*Size)[0]->AsNumber(), (*Size)[1]->AsNumber(), (*Size)[2]->AsNumber());
			}
			LapObstacles.Add(Ob);
		}
	}

	LapLines.Sort([](const FLapLine& A, const FLapLine& B) { return A.Order < B.Order; });
	UE_LOG(LogRideGame, Log, TEXT("RideCourseElements: laps layout, %d lines, %d obstacles, target %.1f s."),
		LapLines.Num(), LapObstacles.Num(), Readout.TargetLapSeconds);
	return LapLines.Num() > 0;
}

void ARideCourseElements::AddBox(const FVector& CentreWorld, const FVector& SizeM, const FLinearColor& Color, float YawDeg)
{
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
	C->SetStaticMesh(CubeMesh);
	C->SetupAttachment(RootComponent);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->RegisterComponent();
	C->SetWorldLocationAndRotation(CentreWorld, FRotator(0.f, YawDeg, 0.f));
	C->SetWorldScale3D(SizeM); // the engine cube is 1 m on a side
	UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	M->SetVectorParameterValue(TEXT("Color"), Color);
	C->SetMaterial(0, M);
}

void ARideCourseElements::AddPole(const FVector& BaseWorld, double HeightM, double RadiusM, const FLinearColor& Color)
{
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
	C->SetStaticMesh(CylinderMesh);
	C->SetupAttachment(RootComponent);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->RegisterComponent();
	// The engine cylinder is 1 m tall and 1 m across, pivot at its centre.
	C->SetWorldLocation(BaseWorld + FVector(0.0, 0.0, 50.0 * HeightM));
	C->SetWorldScale3D(FVector(2.0 * RadiusM, 2.0 * RadiusM, HeightM));
	UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	M->SetVectorParameterValue(TEXT("Color"), Color);
	C->SetMaterial(0, M);
}

void ARideCourseElements::AddLabel(const FVector& World, const FString& Text, const FLinearColor& Color, float SizeCm, float YawDeg)
{
	UTextRenderComponent* T = NewObject<UTextRenderComponent>(this);
	T->SetupAttachment(RootComponent);
	T->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	T->RegisterComponent();
	T->SetWorldLocationAndRotation(World, FRotator(0.f, YawDeg, 0.f));
	T->SetText(FText::FromString(Text));
	T->SetTextRenderColor(Color.ToFColor(true));
	T->SetWorldSize(SizeCm);
	T->SetHorizontalAlignment(EHTA_Center);
	T->SetVerticalAlignment(EVRTA_TextCenter);
}

void ARideCourseElements::BuildLapVisuals()
{
	// Each line is a gate: two thin poles at +-half_width, a banner with the label, and a painted
	// ground line at z (the start/finish is checkered). The gate runs along the lateral axis, which
	// is perpendicular to the line's heading (direction of travel). Obstacles are NOT drawn here --
	// the level draws them; the game only scores a hit.
	const float OriginYaw = Board.IsValid() ? Board->GetWorldOriginYawDeg() : 0.f;
	for (const FLapLine& L : LapLines)
	{
		const double H = FMath::DegreesToRadians(L.HeadingDeg);
		// Lateral unit vector in MuJoCo xy (left of travel): (-sin h, cos h).
		const double Lx = -FMath::Sin(H), Ly = FMath::Cos(H);
		// A box with local X along heading and local Y along the lateral axis: UE yaw = origin - heading.
		const float LineYaw = OriginYaw - static_cast<float>(L.HeadingDeg);
		const bool bStart = L.Order == 0;
		const FLinearColor LineColor = bStart ? kCheckerColor : kGateColor;

		AddPole(ToWorldXY(L.X + L.HalfWidth * Lx, L.Y + L.HalfWidth * Ly, L.Z), 2.8, 0.08, kPoleColor);
		AddPole(ToWorldXY(L.X - L.HalfWidth * Lx, L.Y - L.HalfWidth * Ly, L.Z), 2.8, 0.08, kPoleColor);
		// The banner spans the gate at the top of the poles.
		AddBox(ToWorldXY(L.X, L.Y, L.Z + 2.8), FVector(0.15, 2.0 * L.HalfWidth + 0.3, 0.5), LineColor, LineYaw);
		// A text render reads from its +X side; LineYaw points along travel, so turn the label
		// 180 deg to face the oncoming rider (it read mirrored in the Level 1 demo video).
		AddLabel(ToWorldXY(L.X, L.Y, L.Z + 3.2), L.Label, FLinearColor::White, 34.f, LineYaw + 180.f);
		// The painted line on the ground.
		AddBox(ToWorldXY(L.X, L.Y, L.Z + 0.01), FVector(0.3, 2.0 * L.HalfWidth, 0.02), LineColor, LineYaw);
	}
	bVisualsBuilt = true;
	UE_LOG(LogRideGame, Log, TEXT("RideCourseElements: lap visuals built (%d gates)."), LapLines.Num());
}

void ARideCourseElements::BuildVisuals()
{
	if (bLapsMode)
	{
		BuildLapVisuals();
		return;
	}
	// The rider travels toward MuJoCo -X; labels face the oncoming rider (+X).
	const float FaceYaw = Board.IsValid() ? Board->GetWorldOriginYawDeg() : 0.f;
	const double PostY = StreetHalfWidth - 0.6; // inside the kerb, outside the lane

	for (const FElement& E : Elements)
	{
		switch (E.Kind)
		{
		case EKind::Gate:
		{
			constexpr double H = 2.8;
			AddPole(ToWorld(E.S, PostY, 0.0), H, 0.08, kGateColor);
			AddPole(ToWorld(E.S, -PostY, 0.0), H, 0.08, kGateColor);
			AddBox(ToWorld(E.S, 0.0, H), FVector(0.15, 2.0 * PostY + 0.3, 0.5), kGateColor, FaceYaw);
			AddLabel(ToWorld(E.S, 0.0, H) + FRotator(0.f, FaceYaw, 0.f).RotateVector(FVector(9.0, 0.0, 0.0)), E.Label, FLinearColor::White, 34.f, FaceYaw);
			break;
		}
		case EKind::Flag:
		{
			const FLinearColor Color = E.Y > 0.0 ? kFlagRed : kFlagBlue;
			AddPole(ToWorld(E.S, E.Y, 0.0), 1.5, 0.025, kPoleColor);
			// The panel hangs on the side the rider must pass, so it reads as "go around this way".
			const double Out = E.Y > 0.0 ? 0.25 : -0.25;
			AddBox(ToWorld(E.S, E.Y + Out, 1.25), FVector(0.02, 0.45, 0.4), Color, FaceYaw);
			break;
		}
		case EKind::StopBox:
		{
			const double L = E.S1 - E.S0;
			const double Mid = 0.5 * (E.S0 + E.S1);
			AddBox(ToWorld(E.S0, 0.0, 0.01), FVector(0.15, 2.0 * E.HalfWidth, 0.02), kStopColor, FaceYaw);
			AddBox(ToWorld(E.S1, 0.0, 0.01), FVector(0.15, 2.0 * E.HalfWidth, 0.02), kStopColor, FaceYaw);
			AddBox(ToWorld(Mid, E.HalfWidth, 0.01), FVector(L, 0.15, 0.02), kStopColor, FaceYaw);
			AddBox(ToWorld(Mid, -E.HalfWidth, 0.01), FVector(L, 0.15, 0.02), kStopColor, FaceYaw);
			AddPole(ToWorld(E.S0, -PostY, 0.0), 1.8, 0.05, kPoleColor);
			AddLabel(ToWorld(E.S0, -PostY, 2.1), E.Label, kStopColor, 40.f, FaceYaw);
			break;
		}
		case EKind::SlowZone:
		{
			AddBox(ToWorld(E.S0, 0.0, 0.01), FVector(0.3, 2.0 * LaneHalfWidth, 0.02), kSlowColor, FaceYaw);
			AddBox(ToWorld(E.S1, 0.0, 0.01), FVector(0.3, 2.0 * LaneHalfWidth, 0.02), kSlowColor, FaceYaw);
			AddPole(ToWorld(E.S0, PostY, 0.0), 1.8, 0.05, kPoleColor);
			AddLabel(ToWorld(E.S0, PostY, 2.1), E.Label, kSlowColor, 40.f, FaceYaw);
			break;
		}
		case EKind::Cone:
		{
			// The engine cone is 1 m tall and 1 m across, pivot at its centre.
			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
			C->SetStaticMesh(ConeMesh);
			C->SetupAttachment(RootComponent);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->RegisterComponent();
			C->SetWorldLocation(ToWorld(E.S, E.Y, 0.0) + FVector(0.0, 0.0, 50.0 * kConeHeightM));
			C->SetWorldScale3D(FVector(kConeBaseM, kConeBaseM, kConeHeightM));
			UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(BaseMaterial, this);
			M->SetVectorParameterValue(TEXT("Color"), kConeColor);
			C->SetMaterial(0, M);
			break;
		}
		case EKind::Debris:
		{
			// MuJoCo yaw is about +Z, counter-clockwise from above; Unreal's Y is mirrored.
			AddBox(ToWorld(E.S, E.Y, 0.5 * E.SizeM.Z), E.SizeM, kDebrisColor, FaceYaw - static_cast<float>(E.YawDeg));
			break;
		}
		case EKind::SpeedTrap:
		{
			AddBox(ToWorld(E.S0, 0.0, 0.01), FVector(0.3, 2.0 * LaneHalfWidth, 0.02), kGateColor, FaceYaw);
			AddBox(ToWorld(E.S1, 0.0, 0.01), FVector(0.3, 2.0 * LaneHalfWidth, 0.02), kGateColor, FaceYaw);
			AddPole(ToWorld(E.S0, PostY, 0.0), 1.8, 0.05, kPoleColor);
			AddLabel(ToWorld(E.S0, PostY, 2.1), E.Label, kGateColor, 40.f, FaceYaw);
			break;
		}
		case EKind::NoBuzz:
		{
			AddPole(ToWorld(E.S0, -PostY, 0.0), 1.8, 0.05, kPoleColor);
			AddLabel(ToWorld(E.S0, -PostY, 2.1), E.Label, kClimbColor, 40.f, FaceYaw);
			break;
		}
		}
	}
	bVisualsBuilt = true;
	UE_LOG(LogRideGame, Log, TEXT("RideCourseElements: visuals built."));
}

void ARideCourseElements::ResetRun()
{
	for (FElement& E : Elements)
	{
		E.bDone = E.bFailed = E.bTailSeen = false;
		E.PeakSpeed = 0.0;
	}
	Readout = FRideGameReadout();
}

void ARideCourseElements::Event(const FString& Text)
{
	Readout.Toast = Text;
	Readout.ToastAgeSeconds = 0.0;
	UE_LOG(LogRideGame, Log, TEXT("RideGame: %s (score %d)"), *Text, Readout.Score);
}

void ARideCourseElements::Award(const FString& Key, const FString& Text, int32 Multiplier)
{
	const int32 Points = Scores.FindRef(Key) * Multiplier;
	Readout.Score += Points;
	Event(FString::Printf(TEXT("%s %s%d"), *Text, Points >= 0 ? TEXT("+") : TEXT(""), Points));
}

void ARideCourseElements::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Readout.ToastAgeSeconds += DeltaSeconds;
	if (Readout.ToastAgeSeconds > kToastSeconds)
	{
		Readout.Toast.Reset();
	}

	if (!Board.IsValid())
	{
		for (TActorIterator<ABoardActor> It(GetWorld()); It; ++It)
		{
			Board = *It;
			break;
		}
		if (!Board.IsValid())
		{
			return;
		}
	}
	if (!bVisualsBuilt)
	{
		BuildVisuals();
	}

	OverboardWire::FBoardState State;
	if (!Board->GetLatestState(State))
	{
		return;
	}
	if (bLapsMode)
	{
		TickLaps(State, Board->IsPhysicsHandoff());
		return;
	}
	if (!bLoggedFrame)
	{
		bLoggedFrame = true;
		const APlayerController* PC = GetWorld()->GetFirstPlayerController();
		const FVector Cam = PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
		UE_LOG(LogRideGame, Log, TEXT("RideCourseElements: first sample -- MuJoCo pos (%.2f, %.2f, %.2f) m, board actor at %s, START gate at %s, camera at %s, origin %s yaw %.1f"),
			State.Pos[0], State.Pos[1], State.Pos[2], *Board->GetActorLocation().ToString(), *ToWorld(14.0, 0.0, 0.0).ToString(),
			*Cam.ToString(), *Board->GetWorldOriginOffsetCm().ToString(), Board->GetWorldOriginYawDeg());
	}
	const double S = StartX - State.Pos[0];
	const double Y = State.Pos[1];
	const double Speed = FMath::Abs(State.WheelRateRadS) * kElementsWheelRadiusM;
	// The ride ends at the handoff (bit 4). Bit 2 alone is |pitch| > 20 deg, which a tail-brake
	// stop reaches on purpose.
	const bool bDown = Board->IsPhysicsHandoff();
	const bool bBuzz = (State.Flags & (OverboardWire::EStateFlags::RiderWarningPulsed | OverboardWire::EStateFlags::RiderWarningSolid)) != 0;
	const double Now = GetWorld()->GetTimeSeconds();

	// A reset puts the board back at the course start.
	if (S < LastS - kResetJumpM)
	{
		ResetRun();
	}
	const double PrevS = LastS;
	LastS = S;
	auto Crossed = [PrevS, S](double At) { return PrevS < At && S >= At; };

	if (bDown)
	{
		if (Readout.bRunActive)
		{
			Readout.bRunActive = false;
			Event(TEXT("OVERBOARD"));
		}
		return;
	}

	if (Readout.bRunActive)
	{
		Readout.ElapsedSeconds = Now - RunStartSeconds;
	}

	Readout.ZoneLabel.Reset();
	for (FElement& E : Elements)
	{
		const bool bInside = S >= E.S0 && S <= E.S1;
		switch (E.Kind)
		{
		case EKind::Gate:
			if (!Crossed(E.S))
			{
				break;
			}
			if (E.Id == TEXT("start"))
			{
				ResetRun();
				Readout.bRunActive = true;
				RunStartSeconds = Now;
				Event(TEXT("GO"));
			}
			else if (Readout.bRunActive && E.Id == TEXT("finish"))
			{
				Readout.bRunActive = false;
				Readout.bFinished = true;
				Readout.ElapsedSeconds = Now - RunStartSeconds;
				Award(TEXT("finish"), FString::Printf(TEXT("FINISH %.1f s"), Readout.ElapsedSeconds));
			}
			else if (Readout.bRunActive)
			{
				Event(FString::Printf(TEXT("%s %.1f s"), *E.Label, Now - RunStartSeconds));
			}
			break;

		case EKind::Flag:
			if (Readout.bRunActive && !E.bDone && Crossed(E.S))
			{
				E.bDone = true;
				// Pass on the flag's OUTER side: the same sign as the flag, and further out.
				const bool bPass = FMath::Sign(Y) == FMath::Sign(E.Y) && FMath::Abs(Y) > FMath::Abs(E.Y);
				if (bPass) { Award(TEXT("flag"), TEXT("FLAG")); }
				else { Event(TEXT("FLAG MISSED")); }
			}
			break;

		case EKind::StopBox:
			if (!Readout.bRunActive || E.bDone)
			{
				break;
			}
			// The tail drag starts on the approach: count it from 8 m before the box.
			if (S >= E.S0 - 8.0 && S <= E.S1)
			{
				E.bTailSeen |= State.PitchRad > E.TailPitchRad; // nose up = tail pad down
			}
			if (bInside && FMath::Abs(Y) <= E.HalfWidth)
			{
				Readout.ZoneLabel = E.Label;
				if (Speed < E.StopSpeed)
				{
					E.bDone = true;
					Award(TEXT("stop"), TEXT("STOPPED"));
					if (E.bTailSeen)
					{
						Award(TEXT("tail_stop_bonus"), TEXT("TAIL STOP"));
					}
				}
			}
			else if (S > E.S1)
			{
				E.bDone = true;
				Event(TEXT("NO STOP"));
			}
			break;

		case EKind::SlowZone:
			if (!Readout.bRunActive || E.bDone)
			{
				break;
			}
			if (bInside)
			{
				Readout.ZoneLabel = E.Label;
				if (Speed > E.MaxSpeed && !E.bFailed)
				{
					E.bFailed = true;
					Award(TEXT("slow_penalty"), TEXT("TOO FAST"));
				}
			}
			else if (S > E.S1)
			{
				E.bDone = true;
				if (!E.bFailed) { Award(TEXT("slow_clean"), TEXT("SLOW ZONE CLEAN")); }
			}
			break;

		case EKind::SpeedTrap:
			if (!Readout.bRunActive || E.bDone)
			{
				break;
			}
			if (bInside)
			{
				Readout.ZoneLabel = FString::Printf(TEXT("%s  %.1f MPH"), *E.Label, Speed * 2.23694);
				E.PeakSpeed = FMath::Max(E.PeakSpeed, Speed);
			}
			else if (S > E.S1)
			{
				E.bDone = true;
				const FString Text = FString::Printf(TEXT("TOP SPEED %.1f MPH"), E.PeakSpeed * 2.23694);
				if (E.PeakSpeed >= E.BonusSpeed) { Award(TEXT("speed_trap"), Text); }
				else { Event(Text); }
			}
			break;

		case EKind::Cone:
			if (Readout.bRunActive && !E.bDone && FMath::Abs(S - E.S) < kConeHitRadiusM && FMath::Abs(Y - E.Y) < kConeHitRadiusM)
			{
				E.bDone = true;
				Award(TEXT("obstacle_hit"), TEXT("CONE HIT"));
			}
			break;

		case EKind::Debris:
		{
			// The board's centre inside the box footprint (in the box's own frame), plus a margin.
			const double Rad = FMath::DegreesToRadians(E.YawDeg);
			const double Ds = -(S - E.S); // course s runs along MuJoCo -X
			const double Dy = Y - E.Y;
			const double Lx = FMath::Cos(Rad) * Ds + FMath::Sin(Rad) * Dy;
			const double Ly = -FMath::Sin(Rad) * Ds + FMath::Cos(Rad) * Dy;
			if (Readout.bRunActive && !E.bDone && FMath::Abs(Lx) < 0.5 * E.SizeM.X + kDebrisHitMarginM
				&& FMath::Abs(Ly) < 0.5 * E.SizeM.Y + kDebrisHitMarginM)
			{
				E.bDone = true;
				Award(TEXT("obstacle_hit"), TEXT("DEBRIS HIT"));
			}
			break;
		}

		case EKind::NoBuzz:
			if (!Readout.bRunActive || E.bDone)
			{
				break;
			}
			if (bInside)
			{
				Readout.ZoneLabel = E.Label;
				if (bBuzz && !E.bFailed)
				{
					E.bFailed = true;
					Event(TEXT("BUZZ - CLIMB BONUS LOST"));
				}
			}
			else if (S > E.S1)
			{
				E.bDone = true;
				if (!E.bFailed) { Award(TEXT("no_buzz"), TEXT("NO-BUZZ CLIMB")); }
			}
			break;
		}
	}
}

bool ARideCourseElements::LineCrossed(const FLapLine& L, double X0, double Y0, double X1, double Y1) const
{
	// The board crosses the line when its along-heading coordinate goes from < 0 to >= 0 within
	// |lateral| <= half_width (headless_pilot.py Lines.cross).
	const double H = FMath::DegreesToRadians(L.HeadingDeg);
	const double Tx = FMath::Cos(H), Ty = FMath::Sin(H);
	const double A0 = (X0 - L.X) * Tx + (Y0 - L.Y) * Ty;
	const double A1 = (X1 - L.X) * Tx + (Y1 - L.Y) * Ty;
	if (!(A0 < 0.0 && A1 >= 0.0))
	{
		return false;
	}
	const double Lat = -(X1 - L.X) * Ty + (Y1 - L.Y) * Tx;
	return FMath::Abs(Lat) <= L.HalfWidth;
}

void ARideCourseElements::TickLaps(const OverboardWire::FBoardState& State, bool bDown)
{
	// The clock is the wire's sim time, not the game clock: in -nullrhi the game clock runs ~3.5x
	// fast, and the lap time must be real seconds (the reference pilot times on st["t"] too).
	const double Now = State.SimTimeS;
	const double X = State.Pos[0];
	const double Y = State.Pos[1];

	Readout.bLaps = true;
	Readout.bCruise = bCruiseMode;

	// A reset (a teleport over 5 m in one sample) or a fall (the handoff) aborts the current lap.
	const double Jump = bHavePrevPoint
		? FMath::Sqrt(FMath::Square(X - PrevX) + FMath::Square(Y - PrevY)) : 0.0;
	const bool bAbort = (bHavePrevPoint && Jump > kResetJumpM) || bDown;
	if (bAbort && bLapActive)
	{
		bLapActive = false;
		Event(TEXT("LAP ABORTED"));
	}
	if (bDown)
	{
		// The host freezes the pose during a handoff; do not score crossings, and do not advance
		// the previous point, so the teleport back on reset reads as a reset next tick.
		Readout.LapNumber = CompletedLaps;
		return;
	}

	if (bHavePrevPoint && Jump <= kResetJumpM)
	{
		for (int32 i = 0; i < LapLines.Num(); ++i)
		{
			if (!LineCrossed(LapLines[i], PrevX, PrevY, X, Y))
			{
				continue;
			}
			if (i == 0)
			{
				if (bLapActive)
				{
					// Close the lap: any checkpoint not yet reached is missed.
					TArray<FString> Missed = LapMissedIds;
					for (int32 j = LapNext; j < LapLines.Num(); ++j)
					{
						Missed.Add(LapLines[j].Id);
					}
					const double LapTime = Now - LapStartSeconds;
					const bool bClean = Missed.Num() == 0;
					++CompletedLaps;
					Readout.CompletedLaps = CompletedLaps;
					Readout.LastLapSeconds = LapTime;
					Readout.bLastLapClean = bClean;
					Readout.LastLapMissed = FString::Join(Missed, TEXT(","));
					Event(FString::Printf(TEXT("LAP %d  %s  %s"), CompletedLaps, *FormatLapTime(LapTime),
						bClean ? TEXT("CLEAN") : TEXT("MISSED")));
					if (bClean && (BestCleanSeconds <= 0.0 || LapTime < BestCleanSeconds))
					{
						BestCleanSeconds = LapTime;
						Readout.BestCleanSeconds = BestCleanSeconds;
						Event(TEXT("BEST LAP"));
					}
				}
				bLapActive = true;
				LapStartSeconds = Now;
				LapNext = 1;
				LapMissedIds.Reset();
				// Each lap scores its own hits: a cone hit on lap 1 must score again on lap 2.
				for (FLapObstacle& Ob : LapObstacles)
				{
					Ob.bDone = false;
				}
			}
			else if (bLapActive && i >= LapNext)
			{
				for (int32 j = LapNext; j < i; ++j)
				{
					LapMissedIds.Add(LapLines[j].Id);
					Event(FString::Printf(TEXT("MISSED %s"), *LapLines[j].Id.ToUpper()));
				}
				LapNext = i + 1;
				Event(LapLines[i].Label);
			}
		}
	}

	// A hit scores obstacle_hit once; it does not abort the lap and does not make it un-CLEAN
	// (only a missed checkpoint does). MuJoCo decides what the hit does to the board.
	if (bLapActive)
	{
		for (FLapObstacle& Ob : LapObstacles)
		{
			if (Ob.bDone)
			{
				continue;
			}
			bool bHit = false;
			if (Ob.Kind == EKind::Cone)
			{
				bHit = FMath::Abs(X - Ob.X) < kConeHitRadiusM && FMath::Abs(Y - Ob.Y) < kConeHitRadiusM;
			}
			else
			{
				const double Rad = FMath::DegreesToRadians(Ob.YawDeg);
				const double Dx = X - Ob.X, Dy = Y - Ob.Y;
				const double Lx = FMath::Cos(Rad) * Dx + FMath::Sin(Rad) * Dy;
				const double Ly = -FMath::Sin(Rad) * Dx + FMath::Cos(Rad) * Dy;
				bHit = FMath::Abs(Lx) < 0.5 * Ob.SizeM.X + kDebrisHitMarginM
					&& FMath::Abs(Ly) < 0.5 * Ob.SizeM.Y + kDebrisHitMarginM;
			}
			if (bHit)
			{
				Ob.bDone = true;
				Award(TEXT("obstacle_hit"), Ob.Kind == EKind::Cone ? TEXT("CONE HIT") : TEXT("OBSTACLE HIT"));
			}
		}
	}

	Readout.bRunActive = bLapActive;
	Readout.LapNumber = bLapActive ? CompletedLaps + 1 : CompletedLaps;
	Readout.LapTimeSeconds = bLapActive ? (Now - LapStartSeconds) : 0.0;
	Readout.MissedList = FString::Join(LapMissedIds, TEXT(","));
	if (LapLines.Num() > 0)
	{
		Readout.NextCheckpointLabel = (bLapActive && LapNext < LapLines.Num())
			? LapLines[LapNext].Label : LapLines[0].Label;
	}

	PrevX = X;
	PrevY = Y;
	bHavePrevPoint = true;
}
