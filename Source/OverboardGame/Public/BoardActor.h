// BoardActor.h
//
// The rendered board. THE RULE: this actor computes no physics. Collision and gravity are
// disabled in the constructor, on every mesh component including the real board geometry below;
// its transform every tick comes entirely from state received over the wire, one render-delay
// behind the wall clock, interpolated -- never extrapolated.
//
// W3 (overboard#162): the real Openwheel geometry (see ../../mesh/README.md and
// ../../Meshes/openwheel/), built at runtime from the STL files via UProceduralMeshComponent --
// no .uasset import step, see mesh/README.md for why. BoxMesh, the W1 placeholder, stays: it is
// the fallback if the real mesh fails to load for any reason. An invisible board during capture
// is worse than an ugly one, so failure here must never mean "nothing renders".
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "BoardStateClient.h"
#include "BoardActor.generated.h"

class UStaticMeshComponent;
class UProceduralMeshComponent;

UCLASS()
class OVERBOARDGAME_API ABoardActor : public AActor
{
	GENERATED_BODY()

public:
	ABoardActor();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	// True if the newest received sample had OverboardWire::EStateFlags::Fallen set. False
	// (never fallen) before anything has been received. For AOverboardPlayerController's
	// reset-on-fall (overboard#162 W3) -- reads the newest raw sample, not the interpolated
	// render pose, so it reacts on the tick data actually arrives, not one render-delay later.
	bool IsFallen() const { return bLatestSampleFallen; }

protected:
	UPROPERTY(VisibleAnywhere, Category = "Board")
	TObjectPtr<UStaticMeshComponent> BoxMesh;

	// How far behind the wall clock we render, in seconds. Must always be able to find two real
	// samples to interpolate between; too small and we run out of history and hold the last
	// known pose (still not extrapolation, just a stall). Tune once the host's real send rate
	// (500 Hz control loop) is confirmed -- this default is a conservative placeholder.
	UPROPERTY(EditAnywhere, Category = "Board|Networking")
	float RenderDelaySeconds = 0.05f;

	// --- W3 real mesh -----------------------------------------------------------------------

	// Parent for every real-geometry part, attached at zero relative offset -- see mesh/README.md:
	// all seven STL parts and the wheel share one origin at the wheel axle, matching this actor's
	// own origin, so nothing here is individually hand-positioned.
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<USceneComponent> MeshAssemblyRoot;

	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UProceduralMeshComponent> FrontEnclosureMesh;
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UProceduralMeshComponent> RearEnclosureMesh;
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UProceduralMeshComponent> FrontBumperMesh;
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UProceduralMeshComponent> RearBumperMesh;
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UProceduralMeshComponent> FrontFootpadMesh;
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UProceduralMeshComponent> RearFootpadMesh;
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UProceduralMeshComponent> ElectronicsPlatformMesh;

	// Not an STL -- a MuJoCo primitive cylinder (radius 145.4mm, width 150mm; see mesh/README.md).
	UPROPERTY(VisibleAnywhere, Category = "Board|RealMesh")
	TObjectPtr<UStaticMeshComponent> WheelMesh;

	// True once every part above has loaded and built successfully. While false, BoxMesh is the
	// visible fallback and the real-mesh components stay hidden -- see TryBuildRealMesh.
	bool bRealMeshLoaded = false;

private:
	TUniquePtr<FBoardStateClient> StateClient;

	// Applies the interpolated, transformed pose to the actor. No-op if we don't yet have at
	// least two received samples.
	void UpdatePoseFromHistory();

	// Attempts to load and build all seven STL parts plus the wheel cylinder. Returns true only
	// if every single one succeeded; on any failure, tears down what it built and leaves BoxMesh
	// as the visible fallback -- partial real geometry (e.g. a board with no rear bumper) would
	// look like a rendering bug, not a graceful degradation, so this is all-or-nothing.
	bool TryBuildRealMesh();

	// Loads Meshes/openwheel/<StlBaseName>.stl and builds it into Component as one procedural
	// mesh section. ExtraYawDeg applies an additional local Z rotation on top of the shared
	// zero-offset placement (only front_footpad needs this -- see mesh/README.md). Returns false
	// and logs the parse error on failure.
	bool BuildPartFromStl(UProceduralMeshComponent* Component, const FString& StlBaseName, float ExtraYawDeg = 0.f);

	bool bLatestSampleFallen = false;
};
