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
	constexpr double kWheelRadiusM = 0.146;
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
}

ARideCourseElements::ARideCourseElements()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	CubeMesh = Cube.Object;
	CylinderMesh = Cylinder.Object;
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

	StartX = Root->GetNumberField(TEXT("start_x_m"));
	LaneHalfWidth = Root->GetNumberField(TEXT("lane_half_width_m"));
	StreetHalfWidth = Root->GetNumberField(TEXT("street_half_width_m"));

	const TSharedPtr<FJsonObject> Profile = Root->GetObjectField(TEXT("profile"));
	for (const TSharedPtr<FJsonValue>& V : Profile->GetArrayField(TEXT("s_m"))) { ProfileS.Add(V->AsNumber()); }
	for (const TSharedPtr<FJsonValue>& V : Profile->GetArrayField(TEXT("z_m"))) { ProfileZ.Add(V->AsNumber()); }

	for (const auto& Pair : Root->GetObjectField(TEXT("scores"))->Values)
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
			: EKind::NoBuzz;
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

FVector ARideCourseElements::ToWorld(double S, double Y, double UpM) const
{
	// The same frame ABoardActor uses: MuJoCo (x, y, z) m -> Unreal (100x, -100y, 100z) cm,
	// rotated by the origin yaw and moved to the origin (the level's PlayerStart).
	const double X = StartX - S;
	const double Z = HeightAt(S) + UpM;
	const FVector Local(100.0 * X, -100.0 * Y, 100.0 * Z);
	const ABoardActor* B = Board.Get();
	const float Yaw = B ? B->GetWorldOriginYawDeg() : 0.f;
	const FVector Offset = B ? B->GetWorldOriginOffsetCm() : FVector::ZeroVector;
	return Offset + FRotator(0.f, Yaw, 0.f).RotateVector(Local);
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

void ARideCourseElements::BuildVisuals()
{
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
	const double Speed = FMath::Abs(State.WheelRateRadS) * kWheelRadiusM;
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
			if (bInside && FMath::Abs(Y) <= E.HalfWidth)
			{
				Readout.ZoneLabel = E.Label;
				E.bTailSeen |= State.PitchRad > E.TailPitchRad; // nose up = tail pad down
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
