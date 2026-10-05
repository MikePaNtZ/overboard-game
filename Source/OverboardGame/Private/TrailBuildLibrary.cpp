// TrailBuildLibrary.cpp -- see the header.
#include "TrailBuildLibrary.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "LandscapeUtils.h"
#include "Misc/FileHelper.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "RHIGlobals.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogOverboardTrail, Log, All);

ALandscape* UTrailBuildLibrary::ImportLandscapeFromRaw(const FString& HeightRawPath, int32 Size, int32 SectionsPerComponent,
	int32 QuadsPerSection, FVector Location, FVector Scale, UMaterialInterface* Material,
	const TArray<FString>& LayerNames, const TArray<FString>& LayerRawPaths, const FString& LayerInfoDir,
	const FString& Label)
{
#if WITH_EDITOR
	const int32 QuadsPerComponent = SectionsPerComponent * QuadsPerSection;
	if (QuadsPerComponent <= 0 || (Size - 1) % QuadsPerComponent != 0)
	{
		UE_LOG(LogOverboardTrail, Error, TEXT("ImportLandscapeFromRaw: size %d is not N * %d + 1."), Size, QuadsPerComponent);
		return nullptr;
	}
	if (LayerNames.Num() != LayerRawPaths.Num())
	{
		UE_LOG(LogOverboardTrail, Error, TEXT("ImportLandscapeFromRaw: %d layer names but %d layer files."), LayerNames.Num(), LayerRawPaths.Num());
		return nullptr;
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		UE_LOG(LogOverboardTrail, Error, TEXT("ImportLandscapeFromRaw: no editor world."));
		return nullptr;
	}

	const int64 NumVerts = int64(Size) * Size;
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *HeightRawPath) || Bytes.Num() != NumVerts * 2)
	{
		UE_LOG(LogOverboardTrail, Error, TEXT("ImportLandscapeFromRaw: '%s' missing or not %lld bytes."), *HeightRawPath, NumVerts * 2);
		return nullptr;
	}
	TArray<uint16> Heights;
	Heights.SetNumUninitialized(NumVerts);
	FMemory::Memcpy(Heights.GetData(), Bytes.GetData(), Bytes.Num());  // little-endian on every target

	TArray<FLandscapeImportLayerInfo> ImportLayers;
	for (int32 i = 0; i < LayerNames.Num(); ++i)
	{
		FLandscapeImportLayerInfo Info{FName(*LayerNames[i])};
		Info.LayerInfo = UE::Landscape::CreateTargetLayerInfo(FName(*LayerNames[i]), LayerInfoDir);
		if (!Info.LayerInfo)
		{
			UE_LOG(LogOverboardTrail, Error, TEXT("ImportLandscapeFromRaw: cannot create layer info '%s' in %s."), *LayerNames[i], *LayerInfoDir);
			return nullptr;
		}
		if (!FFileHelper::LoadFileToArray(Info.LayerData, *LayerRawPaths[i]) || Info.LayerData.Num() != NumVerts)
		{
			UE_LOG(LogOverboardTrail, Error, TEXT("ImportLandscapeFromRaw: layer '%s' missing or not %lld bytes."), *LayerRawPaths[i], NumVerts);
			return nullptr;
		}
		Info.SourceFilePath = LayerRawPaths[i];
		ImportLayers.Add(MoveTemp(Info));
	}

	TMap<FGuid, TArray<uint16>> HeightDataPerLayers;
	HeightDataPerLayers.Add(FGuid(), MoveTemp(Heights));
	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataPerLayers;
	MaterialLayerDataPerLayers.Add(FGuid(), ImportLayers);

	ALandscape* Landscape = World->SpawnActor<ALandscape>(Location, FRotator::ZeroRotator);
	if (!Landscape)
	{
		UE_LOG(LogOverboardTrail, Error, TEXT("ImportLandscapeFromRaw: SpawnActor<ALandscape> failed."));
		return nullptr;
	}
	Landscape->LandscapeMaterial = Material;
	Landscape->SetActorRelativeScale3D(Scale);
	Landscape->Import(FGuid::NewGuid(), 0, 0, Size - 1, Size - 1, SectionsPerComponent, QuadsPerSection,
		HeightDataPerLayers, *HeightRawPath, MaterialLayerDataPerLayers, ELandscapeImportAlphamapType::Additive,
		TArrayView<const FLandscapeLayer>());

	ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
	if (!LandscapeInfo)
	{
		UE_LOG(LogOverboardTrail, Error, TEXT("ImportLandscapeFromRaw: the landscape has no info after Import."));
		return nullptr;
	}
	if (!Label.IsEmpty())
	{
		Landscape->SetActorLabel(Label);
	}
	LandscapeInfo->UpdateLayerInfoMap(Landscape);
	for (const FLandscapeImportLayerInfo& Layer : ImportLayers)
	{
		Landscape->AddTargetLayer(Layer.LayerName, FLandscapeTargetLayerSettings(Layer.LayerInfo, Layer.SourceFilePath));
		const int32 Index = LandscapeInfo->GetLayerInfoIndex(Layer.LayerName);
		if (Index != INDEX_NONE)
		{
			LandscapeInfo->Layers[Index].LayerInfoObj = Layer.LayerInfo;
		}
	}
	UE_LOG(LogOverboardTrail, Log, TEXT("ImportLandscapeFromRaw: %s, %dx%d verts, %d layers, scale %s, at %s."),
		*Landscape->GetActorLabel(), Size, Size, ImportLayers.Num(), *Scale.ToString(), *Location.ToString());
	return Landscape;
#else
	return nullptr;
#endif
}

UStaticMesh* UTrailBuildLibrary::CreateStaticMeshFromObm(const FString& ObmPath, const FString& AssetPath, bool bNanite,
	int32 ShapePreservation, bool bTwoSidedFoliage)
{
#if WITH_EDITOR
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *ObmPath) || Bytes.Num() < 20 || FMemory::Memcmp(Bytes.GetData(), "OBM1", 4) != 0)
	{
		UE_LOG(LogOverboardTrail, Error, TEXT("CreateStaticMeshFromObm: '%s' missing or not OBM1."), *ObmPath);
		return nullptr;
	}
	int64 Off = 4;
	auto ReadU32 = [&Bytes, &Off]() { uint32 V; FMemory::Memcpy(&V, Bytes.GetData() + Off, 4); Off += 4; return V; };
	const uint32 NumVerts = ReadU32(), NumTris = ReadU32(), NumSections = ReadU32(), Flags = ReadU32();
	const bool bColors = (Flags & 1) != 0, bUV1 = (Flags & 2) != 0;
	const int64 Need = Off + int64(NumVerts) * (12 + 12 + 8 + (bColors ? 4 : 0) + (bUV1 ? 8 : 0)) + int64(NumTris) * 16;
	if (Bytes.Num() < Need)
	{
		UE_LOG(LogOverboardTrail, Error, TEXT("CreateStaticMeshFromObm: '%s' is truncated."), *ObmPath);
		return nullptr;
	}
	const float* Pos = reinterpret_cast<const float*>(Bytes.GetData() + Off); Off += int64(NumVerts) * 12;
	const float* Nrm = reinterpret_cast<const float*>(Bytes.GetData() + Off); Off += int64(NumVerts) * 12;
	const float* UV0 = reinterpret_cast<const float*>(Bytes.GetData() + Off); Off += int64(NumVerts) * 8;
	const uint8* Col = nullptr;
	if (bColors) { Col = Bytes.GetData() + Off; Off += int64(NumVerts) * 4; }
	const float* UV1 = nullptr;
	if (bUV1) { UV1 = reinterpret_cast<const float*>(Bytes.GetData() + Off); Off += int64(NumVerts) * 8; }
	const uint32* Idx = reinterpret_cast<const uint32*>(Bytes.GetData() + Off); Off += int64(NumTris) * 12;
	const uint32* Sec = reinterpret_cast<const uint32*>(Bytes.GetData() + Off); Off += int64(NumTris) * 4;
	TArray<FName> SlotNames;
	for (uint32 s = 0; s < NumSections; ++s)
	{
		const uint32 Len = ReadU32();
		const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Off), Len);
		SlotNames.Add(FName(FString::ConstructFromPtrSize(Conv.Get(), Conv.Length())));
		Off += Len;
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	Package->FullyLoad();
	UStaticMesh* Mesh = FindObject<UStaticMesh>(Package, *AssetName);
	const bool bNew = Mesh == nullptr;
	if (bNew)
	{
		Mesh = NewObject<UStaticMesh>(Package, *AssetName, RF_Public | RF_Standalone);
	}
	Mesh->PreEditChange(nullptr);
	Mesh->SetNumSourceModels(1);
	FMeshDescription* MD = Mesh->CreateMeshDescription(0);
	FStaticMeshAttributes Attr(*MD);
	Attr.Register();
	const int32 NumUVs = bUV1 ? 2 : 1;
	Attr.GetVertexInstanceUVs().SetNumChannels(NumUVs);
	TVertexAttributesRef<FVector3f> VPos = Attr.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> VNrm = Attr.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> VUV = Attr.GetVertexInstanceUVs();
	TVertexInstanceAttributesRef<FVector4f> VCol = Attr.GetVertexInstanceColors();
	TPolygonGroupAttributesRef<FName> PGNames = Attr.GetPolygonGroupMaterialSlotNames();

	MD->ReserveNewVertices(NumVerts);
	MD->ReserveNewVertexInstances(NumVerts);
	MD->ReserveNewTriangles(NumTris);
	TArray<FPolygonGroupID> Groups;
	for (uint32 s = 0; s < NumSections; ++s)
	{
		const FPolygonGroupID G = MD->CreatePolygonGroup();
		PGNames[G] = SlotNames[s];
		Groups.Add(G);
	}
	TArray<FVertexInstanceID> Inst;
	Inst.SetNumUninitialized(NumVerts);
	for (uint32 v = 0; v < NumVerts; ++v)
	{
		const FVertexID V = MD->CreateVertex();
		VPos[V] = FVector3f(Pos[3 * v], Pos[3 * v + 1], Pos[3 * v + 2]);
		const FVertexInstanceID I = MD->CreateVertexInstance(V);
		VNrm[I] = FVector3f(Nrm[3 * v], Nrm[3 * v + 1], Nrm[3 * v + 2]);
		VUV.Set(I, 0, FVector2f(UV0[2 * v], UV0[2 * v + 1]));
		if (UV1) { VUV.Set(I, 1, FVector2f(UV1[2 * v], UV1[2 * v + 1])); }
		if (Col)
		{
			const FLinearColor C = FLinearColor::FromSRGBColor(FColor(Col[4 * v], Col[4 * v + 1], Col[4 * v + 2], Col[4 * v + 3]));
			VCol[I] = FVector4f(C.R, C.G, C.B, C.A);
		}
		else
		{
			VCol[I] = FVector4f(1.f, 1.f, 1.f, 1.f);
		}
		Inst[v] = I;
	}
	for (uint32 t = 0; t < NumTris; ++t)
	{
		const uint32 S = Sec[t] < NumSections ? Sec[t] : 0;
		TArray<FVertexInstanceID, TInlineAllocator<3>> Tri = {Inst[Idx[3 * t]], Inst[Idx[3 * t + 1]], Inst[Idx[3 * t + 2]]};
		MD->CreateTriangle(Groups[S], Tri);
	}
	UStaticMesh::FCommitMeshDescriptionParams Commit;
	Commit.bMarkPackageDirty = true;
	Commit.bUseHashAsGuid = true;
	Mesh->CommitMeshDescription(0, Commit);

	FStaticMeshSourceModel& SM = Mesh->GetSourceModel(0);
	SM.BuildSettings.bRecomputeNormals = false;
	SM.BuildSettings.bRecomputeTangents = true;
	SM.BuildSettings.bUseMikkTSpace = true;
	SM.BuildSettings.bGenerateLightmapUVs = false;
	SM.BuildSettings.bRemoveDegenerates = false;
	SM.BuildSettings.bUseFullPrecisionUVs = true;
	SM.BuildSettings.bUseHighPrecisionTangentBasis = true;

	TArray<FStaticMaterial> Materials;
	for (const FName& N : SlotNames)
	{
		Materials.Add(FStaticMaterial(nullptr, N, N));
	}
	if (!bNew)
	{
		// Keep the materials that are already assigned to slots of the same name.
		for (FStaticMaterial& M : Materials)
		{
			for (const FStaticMaterial& Old : Mesh->GetStaticMaterials())
			{
				if (Old.MaterialSlotName == M.MaterialSlotName) { M.MaterialInterface = Old.MaterialInterface; }
			}
		}
	}
	Mesh->SetStaticMaterials(Materials);
	FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
	Nanite.bEnabled = bNanite;
	Nanite.ShapePreservation = ShapePreservation == 2 ? ENaniteShapePreservation::Voxelize
		: ShapePreservation == 1 ? ENaniteShapePreservation::PreserveArea : ENaniteShapePreservation::None;
	// Full-detail fallback: the fallback mesh is also the collision mesh, and verify_trail.py
	// traces against it to measure the tyre gap on the real asphalt and deck surfaces.
	Nanite.FallbackPercentTriangles = 1.0f;
	Nanite.FallbackRelativeError = 0.0f;
	Mesh->SetNaniteSettings(Nanite);
	Mesh->bSupportRayTracing = false;
	Mesh->CreateBodySetup();
	Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
	Mesh->Build(false);
	Mesh->PostEditChange();
	if (UBodySetup* Body = Mesh->GetBodySetup())
	{
		Body->InvalidatePhysicsData();
		Body->CreatePhysicsMeshes();
	}
	if (bNew)
	{
		FAssetRegistryModule::AssetCreated(Mesh);
	}
	Package->MarkPackageDirty();
	UE_LOG(LogOverboardTrail, Log, TEXT("CreateStaticMeshFromObm: %s, %u verts, %u tris, %u slots, nanite %d."),
		*AssetPath, NumVerts, NumTris, NumSections, bNanite ? 1 : 0);
	return Mesh;
#else
	return nullptr;
#endif
}

TArray<FString> UTrailBuildLibrary::GetMaterialCompileErrors(UMaterialInterface* Material)
{
	TArray<FString> Errors;
#if WITH_EDITOR
	UMaterial* Base = Material ? Material->GetMaterial() : nullptr;
	if (!Base)
	{
		Errors.Add(TEXT("no material"));
		return Errors;
	}
	Base->ForceRecompileForRendering();
	FMaterialResource* Resource = Base->GetMaterialResource(GMaxRHIShaderPlatform);
	if (!Resource)
	{
		Errors.Add(TEXT("no material resource for this shader platform"));
		return Errors;
	}
	Resource->FinishCompilation();
	Errors = Resource->GetCompileErrors();
#endif
	return Errors;
}
