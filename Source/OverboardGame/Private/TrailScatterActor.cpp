// TrailScatterActor.cpp -- see the header.
#include "TrailScatterActor.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedSkinnedMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"

ATrailScatterActor::ATrailScatterActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent->SetMobility(EComponentMobility::Static);
}

int32 ATrailScatterActor::AddStaticInstances(UStaticMesh* Mesh, const TArray<FTransform>& Transforms, float CullEndCm,
	bool bCastShadow, const FString& ComponentName)
{
	if (!Mesh || Transforms.Num() == 0)
	{
		return 0;
	}
	UHierarchicalInstancedStaticMeshComponent* C = NewObject<UHierarchicalInstancedStaticMeshComponent>(
		this, FName(*ComponentName), RF_Transactional);
	C->SetMobility(EComponentMobility::Static);
	C->SetStaticMesh(Mesh);
	C->SetCastShadow(bCastShadow);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (CullEndCm > 0.f)
	{
		C->SetCullDistances(int32(CullEndCm * 0.8f), int32(CullEndCm));
	}
	C->SetupAttachment(RootComponent);
	C->CreationMethod = EComponentCreationMethod::Instance;
	AddInstanceComponent(C);
	C->RegisterComponent();
	C->AddInstances(Transforms, false, true);
	return C->GetInstanceCount();
}

int32 ATrailScatterActor::AddSkinnedInstances(USkeletalMesh* Mesh, const TArray<FTransform>& Transforms, bool bCastShadow,
	const FString& ComponentName)
{
	if (!Mesh || Transforms.Num() == 0)
	{
		return 0;
	}
	UInstancedSkinnedMeshComponent* C = NewObject<UInstancedSkinnedMeshComponent>(this, FName(*ComponentName), RF_Transactional);
	C->SetMobility(EComponentMobility::Static);
	C->SetSkinnedAssetAndUpdate(Mesh);
	C->SetCastShadow(bCastShadow);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetupAttachment(RootComponent);
	C->CreationMethod = EComponentCreationMethod::Instance;
	AddInstanceComponent(C);
	C->RegisterComponent();
	TArray<int32> Anim;
	Anim.Init(0, Transforms.Num());
	C->AddInstances(Transforms, Anim, false, true);
	return C->GetInstanceCount();
}

void ATrailScatterActor::SetMaterialOnMesh(UObject* Mesh, int32 Index, UMaterialInterface* Material)
{
	TArray<UMeshComponent*> Comps;
	GetComponents(Comps);
	for (UMeshComponent* C : Comps)
	{
		const UInstancedStaticMeshComponent* S = Cast<UInstancedStaticMeshComponent>(C);
		const UInstancedSkinnedMeshComponent* K = Cast<UInstancedSkinnedMeshComponent>(C);
		if ((S && S->GetStaticMesh() == Mesh) || (K && K->GetSkinnedAsset() == Mesh))
		{
			C->SetMaterial(Index, Material);
		}
	}
}
