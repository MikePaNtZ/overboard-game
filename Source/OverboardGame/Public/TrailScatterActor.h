// TrailScatterActor.h
//
// Holds the OB_Trail dressing that tools/trail/build_trail_level.py places: one instanced
// component per mesh, static (HISM) or skinned (Nanite foliage trees). The transforms come from
// tools/trail/gen_course.py, which owns every placement rule (for example the 1.5 m clearance
// from the path edge). This actor only stores and draws them.
//
// Cosmetic only. Nothing here touches the board pose (ADR-0009).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TrailScatterActor.generated.h"

class UStaticMesh;
class USkeletalMesh;
class UMaterialInterface;

UCLASS()
class OVERBOARDGAME_API ATrailScatterActor : public AActor
{
	GENERATED_BODY()

public:
	ATrailScatterActor();

	// Adds one hierarchical instanced static mesh component with these world-space transforms.
	// CullEnd (cm, 0 = never) hides instances beyond that distance; small dressing uses it.
	UFUNCTION(BlueprintCallable, Category = "Overboard|Trail")
	int32 AddStaticInstances(UStaticMesh* Mesh, const TArray<FTransform>& Transforms, float CullEndCm, bool bCastShadow,
		const FString& ComponentName);

	// Adds one instanced skinned mesh component (Nanite skeletal foliage) with these transforms.
	UFUNCTION(BlueprintCallable, Category = "Overboard|Trail")
	int32 AddSkinnedInstances(USkeletalMesh* Mesh, const TArray<FTransform>& Transforms, bool bCastShadow,
		const FString& ComponentName);

	// Overrides material slot Index on every component that draws Mesh.
	UFUNCTION(BlueprintCallable, Category = "Overboard|Trail")
	void SetMaterialOnMesh(UObject* Mesh, int32 Index, UMaterialInterface* Material);
};
