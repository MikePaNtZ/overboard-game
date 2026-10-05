// TrailBuildLibrary.h
//
// Editor-only helpers for tools/trail/build_trail_level.py. Python in UE 5.7 cannot create a
// landscape from height data, so this one call does it, the same way the editor's "Import"
// landscape tool does: ALandscapeProxy::Import with raw 16-bit heights and 8-bit layer weights
// that tools/trail/gen_course.py wrote from the MuJoCo course.
//
// Cosmetic only. A landscape is something the board is DRAWN over; the board pose still comes
// only from MuJoCo (ADR-0009).
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "TrailBuildLibrary.generated.h"

class ALandscape;
class UMaterialInterface;

UCLASS()
class OVERBOARDGAME_API UTrailBuildLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// Creates a landscape in the current editor world from raw files.
	//   HeightRawPath: Size*Size uint16 little-endian, row-major, row = landscape local +Y.
	//   LayerRawPaths: Size*Size uint8 weights, one file per entry of LayerNames (may be empty).
	//   Size must be ComponentCount * SectionsPerComponent * QuadsPerSection + 1.
	//   Layer info assets are created in LayerInfoDir (for example /Game/Trail/Landscape).
	// Returns nullptr and logs the reason on failure. Editor builds only.
	UFUNCTION(BlueprintCallable, Category = "Overboard|Trail")
	static ALandscape* ImportLandscapeFromRaw(const FString& HeightRawPath, int32 Size, int32 SectionsPerComponent,
		int32 QuadsPerSection, FVector Location, FVector Scale, UMaterialInterface* Material,
		const TArray<FString>& LayerNames, const TArray<FString>& LayerRawPaths, const FString& LayerInfoDir,
		const FString& Label);

	// Creates (or replaces) a static mesh asset from an OBM1 file that tools/trail/obm.py wrote.
	// The file holds UE-frame positions in cm, normals, UV0, optional sRGB vertex colours and
	// optional UV1, triangles, and one material slot per section. Exact coordinates: no importer
	// axis or unit conversion is in the path. ShapePreservation: 0 none, 1 preserve area, 2 voxelize.
	UFUNCTION(BlueprintCallable, Category = "Overboard|Trail")
	static UStaticMesh* CreateStaticMeshFromObm(const FString& ObmPath, const FString& AssetPath, bool bNanite,
		int32 ShapePreservation, bool bTwoSidedFoliage);

	// Compiles Material for the running shader platform and returns its compile errors (empty
	// when it compiles). Python has no other way to see why a generated material fell back to
	// the default material.
	UFUNCTION(BlueprintCallable, Category = "Overboard|Trail")
	static TArray<FString> GetMaterialCompileErrors(UMaterialInterface* Material);
};
