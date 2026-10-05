#include "RenderLookOverride.h"

#include "EngineUtils.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/PostProcessVolume.h"

DEFINE_LOG_CATEGORY_STATIC(LogOverboardRenderLook, Log, All);

ARenderLookOverride::ARenderLookOverride()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
	DisableClassNameContains = {
		TEXT("DirectionalLight"), TEXT("SkyLight"), TEXT("Sky_Sphere"), TEXT("AtmosphericFog"),
		TEXT("SkyAtmosphere"), TEXT("ExponentialHeightFog"), TEXT("PostProcessVolume"),
	};
}

void ARenderLookOverride::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	DisableForeignLookActors();
}

void ARenderLookOverride::DisableForeignLookActors()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	int32 Count = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor->GetLevel() == GetLevel())
		{
			continue;
		}
		const FString ClassName = Actor->GetClass()->GetName();
		const bool bMatch = DisableClassNameContains.ContainsByPredicate(
			[&ClassName](const FString& Needle) { return ClassName.Contains(Needle); });
		if (!bMatch)
		{
			continue;
		}
		++Count;
		if (APostProcessVolume* Volume = Cast<APostProcessVolume>(Actor))
		{
			Volume->bEnabled = false;
			continue;
		}
		if (Actor->IsHidden())
		{
			continue;
		}
		Actor->SetActorHiddenInGame(true);
		TInlineComponentArray<USceneComponent*> Components(Actor);
		for (USceneComponent* Component : Components)
		{
			Component->SetVisibility(false, false);
		}
	}
	if (Count != LoggedDisabledCount)
	{
		LoggedDisabledCount = Count;
		UE_LOG(LogOverboardRenderLook, Log, TEXT("RenderLookOverride: %d foreign light/sky/fog/post-process actors switched off."), Count);
	}
}
