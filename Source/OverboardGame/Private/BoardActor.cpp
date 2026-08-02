#include "BoardActor.h"

#include "Components/StaticMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "CoordinateTransform.h"
#include "StlLoader.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogOverboardMesh, Log, All);

ABoardActor::ABoardActor()
{
	PrimaryActorTick.bCanEverTick = true;

	// Identity-scale actor root. BoxMesh and MeshAssemblyRoot attach here as SIBLINGS -- see the
	// header comment on SceneRoot for the scale bug this exists to prevent from recurring.
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	BoxMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BoxMesh"));
	BoxMesh->SetupAttachment(SceneRoot);

	// Placeholder for the real board model. As of W3 the real geometry (below) is the primary
	// visual; this stays as the fallback if it fails to load at runtime -- see class header.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeFinder.Succeeded())
	{
		BoxMesh->SetStaticMesh(CubeFinder.Object);
	}
	// Board-ish proportions rather than a 1m cube: long, thin, low. This scale is BoxMesh's own
	// and must never propagate anywhere else -- it very nearly did once (see SceneRoot comment).
	BoxMesh->SetRelativeScale3D(FVector(0.7f, 0.25f, 0.08f));

	// THE RULE: this application computes no board physics. Collision and gravity are off --
	// the transform comes entirely from wire state.
	BoxMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	BoxMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	BoxMesh->SetSimulatePhysics(false);
	BoxMesh->SetEnableGravity(false);
	BoxMesh->SetMobility(EComponentMobility::Movable);

	// --- W3 real mesh: built at runtime from STL, see mesh/README.md ------------------------
	// Sibling of BoxMesh, both attached directly to the identity-scale SceneRoot -- NOT to
	// BoxMesh or to each other, so the placeholder's cosmetic scale cannot reach this hierarchy.
	MeshAssemblyRoot = CreateDefaultSubobject<USceneComponent>(TEXT("MeshAssemblyRoot"));
	MeshAssemblyRoot->SetupAttachment(SceneRoot);
	MeshAssemblyRoot->SetVisibility(false, true); // hidden until TryBuildRealMesh succeeds

	auto MakePart = [this](const TCHAR* Name) -> UProceduralMeshComponent*
	{
		UProceduralMeshComponent* Part = CreateDefaultSubobject<UProceduralMeshComponent>(Name);
		Part->SetupAttachment(MeshAssemblyRoot);
		return Part;
	};
	FrontEnclosureMesh = MakePart(TEXT("FrontEnclosureMesh"));
	RearEnclosureMesh = MakePart(TEXT("RearEnclosureMesh"));
	FrontBumperMesh = MakePart(TEXT("FrontBumperMesh"));
	RearBumperMesh = MakePart(TEXT("RearBumperMesh"));
	FrontFootpadMesh = MakePart(TEXT("FrontFootpadMesh"));
	RearFootpadMesh = MakePart(TEXT("RearFootpadMesh"));
	ElectronicsPlatformMesh = MakePart(TEXT("ElectronicsPlatformMesh"));

	WheelMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WheelMesh"));
	WheelMesh->SetupAttachment(MeshAssemblyRoot);

	// --- Pint skin: a third sibling of SceneRoot, never a child of the other two -------------
	// Same isolation rule as MeshAssemblyRoot: nothing inherits BoxMesh's cosmetic scale.
	PintAssemblyRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PintAssemblyRoot"));
	PintAssemblyRoot->SetupAttachment(SceneRoot);
	PintAssemblyRoot->SetVisibility(false, true); // shown in BeginPlay only if all parts resolved

	// No scale conversion and no rotation, and both are measured facts rather than assumptions.
	// The prepared FBX is authored in metres with its origin ON THE AXLE, and Unreal's FBX import
	// lands it at centimetre scale directly: the tyre measures 29.08 cm across in world units at
	// scale 1.0, against the 2 x 14.54 cm the MJCF specifies -- a correction factor of 0.99992,
	// i.e. none. Blender's -Z-forward/Y-up export already applied the handedness conversion, so
	// unlike the raw Openwheel STLs (see mesh/README.md) this needs no manual Y-mirror.
	auto MakePintPart = [this](const TCHAR* Name, const TCHAR* AssetPath) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* Part = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Part->SetupAttachment(PintAssemblyRoot);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetCollisionResponseToAllChannels(ECR_Ignore);
		Part->SetSimulatePhysics(false);
		Part->SetEnableGravity(false);
		Part->SetMobility(EComponentMobility::Movable);
		return Part;
	};
	PintFrameMesh     = MakePintPart(TEXT("PintFrameMesh"),     TEXT(""));
	PintWheelTireMesh = MakePintPart(TEXT("PintWheelTireMesh"), TEXT(""));
	PintWheelHubMesh  = MakePintPart(TEXT("PintWheelHubMesh"),  TEXT(""));

	// FObjectFinder is legal HERE and only here -- this is a constructor. It asserts if used from
	// BeginPlay, which is exactly the crash the ground plane hit (see OverboardGameMode.cpp).
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PintFrameFinder(TEXT("/Game/ThirdParty/PintPreview/OneWheelPint_prepared_OW_Frame.OneWheelPint_prepared_OW_Frame"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PintTireFinder(TEXT("/Game/ThirdParty/PintPreview/OneWheelPint_prepared_OW_Wheel_Tire.OneWheelPint_prepared_OW_Wheel_Tire"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PintHubFinder(TEXT("/Game/ThirdParty/PintPreview/OneWheelPint_prepared_OW_Wheel_Hub.OneWheelPint_prepared_OW_Wheel_Hub"));
	bPintSkinLoaded = PintFrameFinder.Succeeded() && PintTireFinder.Succeeded() && PintHubFinder.Succeeded();
	if (bPintSkinLoaded)
	{
		PintFrameMesh->SetStaticMesh(PintFrameFinder.Object);
		PintWheelTireMesh->SetStaticMesh(PintTireFinder.Object);
		PintWheelHubMesh->SetStaticMesh(PintHubFinder.Object);
	}
}

void ABoardActor::BeginPlay()
{
	Super::BeginPlay();

	bRealMeshLoaded = TryBuildRealMesh();
	if (bRealMeshLoaded)
	{
		BoxMesh->SetVisibility(false, true);
		MeshAssemblyRoot->SetVisibility(true, true);
		UE_LOG(LogOverboardMesh, Log, TEXT("ABoardActor: real board mesh loaded, placeholder box hidden."));
	}
	else
	{
		// Fallback stays visible -- an invisible board is worse than an ugly one (see class header).
		UE_LOG(LogOverboardMesh, Error, TEXT("ABoardActor: real board mesh failed to load (see prior errors); falling back to the placeholder box."));
	}

	// The Pint skin replaces whatever is visible above -- it is drawn INSTEAD OF the Openwheel
	// geometry, not alongside it, or the two chassis interpenetrate. The Openwheel components are
	// only hidden, never destroyed: they are still the geometry MuJoCo simulates, and flipping
	// bUsePintSkin off has to bring back exactly what was there before.
	if (bUsePintSkin && bPintSkinLoaded)
	{
		BoxMesh->SetVisibility(false, true);
		MeshAssemblyRoot->SetVisibility(false, true);
		PintAssemblyRoot->SetVisibility(true, true);
		UE_LOG(LogOverboardMesh, Log, TEXT("ABoardActor: Pint skin visible; Openwheel geometry hidden (cosmetic only -- MuJoCo still simulates Openwheel)."));
	}
	else if (bUsePintSkin)
	{
		UE_LOG(LogOverboardMesh, Warning, TEXT("ABoardActor: Pint skin requested but its meshes did not resolve; keeping Openwheel geometry."));
	}

	StateClient = MakeUnique<FBoardStateClient>();
	if (!StateClient->StartListening())
	{
		UE_LOG(LogTemp, Error, TEXT("ABoardActor: BoardStateClient failed to start; board will not move."));
	}
}

void ABoardActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (StateClient.IsValid())
	{
		StateClient->Shutdown();
		StateClient.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void ABoardActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdatePoseFromHistory();
}

void ABoardActor::UpdatePoseFromHistory()
{
	if (!StateClient.IsValid())
	{
		return;
	}

	// Ask for the client's whole retained window (see FBoardStateClient::kHistoryRetentionSeconds,
	// currently 0.25s -- comfortably more than RenderDelaySeconds below, including margin for the
	// real host's bursty ~500Hz cadence rather than a smooth one). Requesting only a handful of
	// samples here would silently reintroduce the same "history shorter than the render delay"
	// gap that GetHistorySnapshot's own default already guards against.
	TArray<FTimestampedBoardState> History;
	StateClient->GetHistorySnapshot(History);
	if (History.Num() == 0)
	{
		return; // nothing received yet -- hold current pose, do not guess
	}

	// Newest raw sample, not the interpolated render pose -- see IsFallen()'s comment on why.
	bLatestSampleFallen = (History.Last().State.Flags & OverboardWire::EStateFlags::Fallen) != 0;

	const double RenderTime = FPlatformTime::Seconds() - static_cast<double>(RenderDelaySeconds);

	// Find the bracket [i, i+1] such that History[i].Arrival <= RenderTime <= History[i+1].Arrival.
	// One buffer behind, never extrapolate: if RenderTime is older than everything we have, hold
	// the oldest sample; if it's newer than everything we have (we're waiting on more data than
	// RenderDelaySeconds accounts for), hold the newest sample rather than projecting forward.
	int32 LowerIndex = -1;
	for (int32 i = 0; i < History.Num() - 1; ++i)
	{
		if (History[i].ArrivalTimeSeconds <= RenderTime && RenderTime <= History[i + 1].ArrivalTimeSeconds)
		{
			LowerIndex = i;
			break;
		}
	}

	OverboardWire::FUeTransform TransformToApply;

	if (LowerIndex >= 0)
	{
		const FTimestampedBoardState& A = History[LowerIndex];
		const FTimestampedBoardState& B = History[LowerIndex + 1];
		const double Span = B.ArrivalTimeSeconds - A.ArrivalTimeSeconds;
		const float Alpha = Span > 0.0 ? FMath::Clamp(static_cast<float>((RenderTime - A.ArrivalTimeSeconds) / Span), 0.f, 1.f) : 0.f;

		const OverboardWire::FUeTransform UeA = OverboardWire::MuJoCoToUnreal(A.State.Pos, A.State.Quat);
		const OverboardWire::FUeTransform UeB = OverboardWire::MuJoCoToUnreal(B.State.Pos, B.State.Quat);

		const FVector PosA(UeA.PosCm[0], UeA.PosCm[1], UeA.PosCm[2]);
		const FVector PosB(UeB.PosCm[0], UeB.PosCm[1], UeB.PosCm[2]);
		const FQuat QuatA(UeA.QuatWXYZ[1], UeA.QuatWXYZ[2], UeA.QuatWXYZ[3], UeA.QuatWXYZ[0]); // UE FQuat ctor is (X,Y,Z,W)
		const FQuat QuatB(UeB.QuatWXYZ[1], UeB.QuatWXYZ[2], UeB.QuatWXYZ[3], UeB.QuatWXYZ[0]);

		// Rotate MuJoCo's whole frame about the world vertical, THEN translate. Order matters:
		// translate-then-rotate would swing the board around the level origin on a lever arm the
		// length of its distance from it, which at this level's spawn is several hundred metres.
		const FQuat OriginYaw(FRotator(0.f, WorldOriginYawDeg, 0.f));
		SetActorLocation(OriginYaw.RotateVector(FMath::Lerp(PosA, PosB, Alpha)) + WorldOriginOffsetCm);
		SetActorRotation(OriginYaw * FQuat::Slerp(QuatA, QuatB, Alpha));
		return;
	}

	// No bracket found: RenderTime falls outside the whole history window. Clamp to whichever
	// end it's outside of instead of extrapolating.
	const FTimestampedBoardState& Clamp = (RenderTime < History[0].ArrivalTimeSeconds) ? History[0] : History.Last();
	TransformToApply = OverboardWire::MuJoCoToUnreal(Clamp.State.Pos, Clamp.State.Quat);
	const FQuat ClampOriginYaw(FRotator(0.f, WorldOriginYawDeg, 0.f));
	SetActorLocation(ClampOriginYaw.RotateVector(
		FVector(TransformToApply.PosCm[0], TransformToApply.PosCm[1], TransformToApply.PosCm[2])) + WorldOriginOffsetCm);
	SetActorRotation(ClampOriginYaw * FQuat(TransformToApply.QuatWXYZ[1], TransformToApply.QuatWXYZ[2], TransformToApply.QuatWXYZ[3], TransformToApply.QuatWXYZ[0]));
}

bool ABoardActor::TryBuildRealMesh()
{
	struct FPartSpec
	{
		UProceduralMeshComponent* Component;
		const TCHAR* StlBaseName;
		float ExtraYawDeg;
	};
	const FPartSpec Parts[] = {
		{ FrontEnclosureMesh, TEXT("front_enclosure"), 0.f },
		{ RearEnclosureMesh, TEXT("rear_enclosure"), 0.f },
		{ FrontBumperMesh, TEXT("front_bumper"), 0.f },
		{ RearBumperMesh, TEXT("rear_bumper"), 0.f },
		// Ships authored sitting at the REAR footpad location (confirmed by the loader's own
		// bounding-box output against mesh/tests/test_stl_loader.cpp, not just the XML comment)
		// -- needs the same extra 180 degree yaw the MJCF applies to move it to the front.
		{ FrontFootpadMesh, TEXT("front_footpad"), 180.f },
		{ RearFootpadMesh, TEXT("rear_footpad"), 0.f },
		{ ElectronicsPlatformMesh, TEXT("electronics_platform"), 0.f },
	};

	bool bAllOk = true;
	FBox BodyLocalBounds(EForceInit::ForceInit); // union of all 7 STL parts, actor-local space
	for (const FPartSpec& Part : Parts)
	{
		if (!BuildPartFromStl(Part.Component, Part.StlBaseName, Part.ExtraYawDeg, BodyLocalBounds))
		{
			bAllOk = false;
		}
	}

	// Arithmetic check, not eyeballing (overboard#162): the body (7 STL parts, not the wheel) is
	// 938 x 232 x 83mm per direct STL measurement, i.e. a (46.9, 11.6, 4.2)cm half-extent in
	// actor-local space. This is computed independently of the actor's current world transform
	// (spawn position, any received pose) specifically so it stays meaningful before any wire
	// state has arrived. A real fix here changed a silent scale bug (real mesh inheriting
	// BoxMesh's (0.7,0.25,0.08) placeholder-shaping scale) that was NOT visible in the wheel-only
	// bounds check below, because the wheel is a sibling of BoxMesh too, not a child of it --
	// only components actually nested under BoxMesh inherited the bug. Log this every time so
	// that stays caught if it ever regresses.
	if (BodyLocalBounds.IsValid)
	{
		const FVector BodyExtent = BodyLocalBounds.GetExtent(); // half-size, cm
		const FVector BodySize = BodyLocalBounds.GetSize();
		constexpr float kExpectedHalfX = 46.9f, kExpectedHalfY = 11.6f, kExpectedHalfZ = 4.2f;
		constexpr float kToleranceCm = 2.0f; // generous sanity-check tolerance, not a unit test
		const bool bXOk = FMath::IsNearlyEqual(BodyExtent.X, kExpectedHalfX, kToleranceCm);
		const bool bYOk = FMath::IsNearlyEqual(BodyExtent.Y, kExpectedHalfY, kToleranceCm);
		const bool bZOk = FMath::IsNearlyEqual(BodyExtent.Z, kExpectedHalfZ, kToleranceCm);
		UE_LOG(LogOverboardMesh, Log,
			TEXT("ABoardActor: body (7 STL parts) local bounds half-extent = (%.2f, %.2f, %.2f) cm, full size = (%.2f, %.2f, %.2f) cm ")
			TEXT("-- expected half-extent ~(46.9, 11.6, 4.2) cm / full ~(93.8, 23.2, 8.3) cm. %s"),
			BodyExtent.X, BodyExtent.Y, BodyExtent.Z, BodySize.X, BodySize.Y, BodySize.Z,
			(bXOk && bYOk && bZOk) ? TEXT("MATCH.") : TEXT("MISMATCH -- scale or placement bug, do not trust this render."));
	}
	else
	{
		UE_LOG(LogOverboardMesh, Error, TEXT("ABoardActor: body bounds empty -- no STL part contributed a single vertex."));
	}

	// Wheel: not an STL, a primitive cylinder. Radius 145.4mm / width 150mm (mesh/README.md,
	// straight from overboard_onewheel.xml's wheel_geom: size="0.1454 0.075" = radius, half-width).
	UStaticMesh* CylinderMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderMesh && WheelMesh)
	{
		WheelMesh->SetStaticMesh(CylinderMesh);

		// ASSUMPTION, unverified against this specific engine build (no display to check it):
		// /Engine/BasicShapes/Cylinder.Cylinder is the standard UE default bounds, radius 50uu,
		// height 100uu. If the board comes out ~2x or ~0.5x wrong on the wheel specifically
		// (everything else right), this assumption is the first thing to check -- see the log
		// line below, which prints the actual computed bounds for exactly that comparison.
		constexpr float kAssumedEngineCylinderRadiusUu = 50.f;
		constexpr float kAssumedEngineCylinderHeightUu = 100.f;
		constexpr float kTireRadiusCm = 14.54f; // 145.4mm
		constexpr float kTireWidthCm = 15.f;    // 150mm (2 x 75mm half-width)
		const float RadiusScale = kTireRadiusCm / kAssumedEngineCylinderRadiusUu;
		const float HeightScale = kTireWidthCm / kAssumedEngineCylinderHeightUu;
		WheelMesh->SetRelativeScale3D(FVector(RadiusScale, RadiusScale, HeightScale));

		// MuJoCo's cylinder default axis is Z; euler="90 0 0" in the MJCF tips it onto Y
		// (lateral) -- same operation here, a 90 degree Roll (UE FRotator rotates about X on
		// Roll), frame-labelling-agnostic since the tire is rotationally symmetric about its own
		// axis (see mesh/README.md).
		WheelMesh->SetRelativeRotation(FRotator(0.f, 0.f, 90.f)); // FRotator(Pitch, Yaw, Roll)

		WheelMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		WheelMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
		WheelMesh->SetSimulatePhysics(false);
		WheelMesh->SetEnableGravity(false);
		WheelMesh->SetMobility(EComponentMobility::Movable);

		const FBoxSphereBounds Bounds = WheelMesh->CalcBounds(WheelMesh->GetComponentTransform());
		UE_LOG(LogOverboardMesh, Log,
			TEXT("ABoardActor: wheel mesh world bounds extent = (%.2f, %.2f, %.2f) cm -- expect radius ~14.54cm, ")
			TEXT("width ~15cm if the assumed engine cylinder native bounds (radius 50uu, height 100uu) are correct. ")
			TEXT("VERIFY VISUALLY, not assumed correct."),
			Bounds.BoxExtent.X, Bounds.BoxExtent.Y, Bounds.BoxExtent.Z);
	}
	else
	{
		UE_LOG(LogOverboardMesh, Error, TEXT("ABoardActor: failed to load wheel cylinder mesh (/Engine/BasicShapes/Cylinder.Cylinder)"));
		bAllOk = false;
	}

	return bAllOk;
}

bool ABoardActor::BuildPartFromStl(UProceduralMeshComponent* Component, const FString& StlBaseName, float ExtraYawDeg, FBox& InOutLocalBounds)
{
	if (!Component)
	{
		return false;
	}

	const FString StlPath = FPaths::ProjectDir() / TEXT("Meshes/openwheel") / (StlBaseName + TEXT(".stl"));

	OverboardMesh::FStlMesh StlMesh;
	std::string Err;
	if (!OverboardMesh::LoadBinaryStl(TCHAR_TO_UTF8(*StlPath), StlMesh, Err))
	{
		UE_LOG(LogOverboardMesh, Error, TEXT("ABoardActor: failed to load %s: %s"), *StlBaseName, *FString(Err.c_str()));
		return false;
	}

	constexpr float kMmToCm = 0.1f; // mesh/README.md: mm -> cm is x0.1 (MuJoCo's 0.001 x UE's 100)
	const float YawRad = FMath::DegreesToRadians(ExtraYawDeg);
	const float CosYaw = FMath::Cos(YawRad);
	const float SinYaw = FMath::Sin(YawRad);

	// mm -> cm, then the same Y-mirror wire/CoordinateTransform.cpp applies to world positions --
	// see mesh/README.md: the actor's local mesh space is "world space at identity rotation", so
	// the same right-to-left-handed mirror applies to local vertices too. Front_footpad's extra
	// yaw (matching the MJCF's euler="0 0 180" on that one geom) is applied after the mirror, in
	// UE-local space, as a plain 2D rotation about Z.
	auto ToUeLocal = [kMmToCm, CosYaw, SinYaw](const OverboardMesh::FVec3& V) -> FVector
	{
		FVector P(V.X * kMmToCm, -V.Y * kMmToCm, V.Z * kMmToCm);
		const float X = P.X;
		const float Y = P.Y;
		P.X = X * CosYaw - Y * SinYaw;
		P.Y = X * SinYaw + Y * CosYaw;
		return P;
	};

	const int32 TriCount = static_cast<int32>(StlMesh.Triangles.size());
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> VertexColors;
	TArray<FProcMeshTangent> Tangents;
	Vertices.Reserve(TriCount * 3);
	Triangles.Reserve(TriCount * 3);
	Normals.Reserve(TriCount * 3);
	UVs.Reserve(TriCount * 3);
	VertexColors.Reserve(TriCount * 3);

	int32 Index = 0;
	for (const OverboardMesh::FStlTriangle& Tri : StlMesh.Triangles)
	{
		const FVector V0 = ToUeLocal(Tri.V0);
		const FVector V1 = ToUeLocal(Tri.V1);
		const FVector V2 = ToUeLocal(Tri.V2);

		// Negating Y reverses winding -- add in (V0, V2, V1) order, not (V0, V1, V2), to keep
		// faces front-facing. The normal below is computed from this same (already-swapped)
		// order so it stays consistent with the winding, rather than trusting the STL's own
		// facet normal (see StlLoader.h: deliberately ignored on load).
		Vertices.Add(V0);
		Vertices.Add(V2);
		Vertices.Add(V1);

		InOutLocalBounds += V0;
		InOutLocalBounds += V1;
		InOutLocalBounds += V2;

		const FVector FaceNormal = FVector::CrossProduct(V2 - V0, V1 - V0).GetSafeNormal();
		Normals.Add(FaceNormal);
		Normals.Add(FaceNormal);
		Normals.Add(FaceNormal);

		Triangles.Add(Index);
		Triangles.Add(Index + 1);
		Triangles.Add(Index + 2);
		Index += 3;

		UVs.Add(FVector2D(0.f, 0.f));
		UVs.Add(FVector2D(0.f, 0.f));
		UVs.Add(FVector2D(0.f, 0.f));

		// Not currently consumed by any material (see mesh/README.md -- per-part brand-palette
		// colour is a deliberately deferred follow-up, not attempted blind). Included anyway so
		// a future material can read it without another geometry pass.
		VertexColors.Add(FColor::White);
		VertexColors.Add(FColor::White);
		VertexColors.Add(FColor::White);
	}

	Component->CreateMeshSection(0, Vertices, Triangles, Normals, UVs, VertexColors, Tangents, /*bCreateCollision=*/false);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetCollisionResponseToAllChannels(ECR_Ignore);
	Component->SetSimulatePhysics(false);
	Component->SetEnableGravity(false);
	Component->SetMobility(EComponentMobility::Movable);

	return true;
}
