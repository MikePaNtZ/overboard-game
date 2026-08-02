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

	// Where MuJoCo's world origin lands in UE world space, in centimetres.
	//
	// The wire carries an ABSOLUTE position and UpdatePoseFromHistory applies it with
	// SetActorLocation, so without this the board is pinned to UE (0,0,0) no matter where it is
	// spawned -- the first packet overwrites the spawn transform. That is fine over the C++
	// placeholder ground (a 100x100m plane centred on the origin) but useless in an imported
	// environment, where the origin is wherever the environment author happened to put it and is
	// typically not flat, not paved, and sometimes not even above ground.
	//
	// This is a RENDERING offset and nothing else. It is added after MuJoCoToUnreal and never
	// fed back anywhere: the wire, the controller and MuJoCo all keep working in MuJoCo's own
	// frame, exactly as before. Moving the board around a level cannot change a single physics
	// value, which is the property ADR-0009 is protecting.
	void SetWorldOriginOffsetCm(const FVector& InOffsetCm) { WorldOriginOffsetCm = InOffsetCm; }
	FVector GetWorldOriginOffsetCm() const { return WorldOriginOffsetCm; }

protected:
	// The actor's true root: identity scale, always. BoxMesh and MeshAssemblyRoot are SIBLINGS
	// attached here, not parent/child of each other -- see the scale bug this fixed (overboard#162):
	// BoxMesh carries its own non-uniform SetRelativeScale3D to turn a 1m cube into a board-ish
	// placeholder shape, and a child component inherits its parent's scale by default. The real
	// mesh was attached under BoxMesh, so it silently inherited (0.7, 0.25, 0.08) on top of its
	// own correct mm->cm conversion -- exact match to the measured on-screen sliver. The
	// placeholder's cosmetic scale must never be able to reach anything else in the hierarchy.
	UPROPERTY(VisibleAnywhere, Category = "Board")
	TObjectPtr<USceneComponent> SceneRoot;

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

	// Zero by default, so OB_Main and every existing capture are bit-identical to before this
	// existed. Set by AOverboardGameMode from the level's PlayerStart, if it has one.
	UPROPERTY(EditAnywhere, Category = "Board|Level")
	FVector WorldOriginOffsetCm = FVector::ZeroVector;

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
	// and logs the parse error on failure. Every processed vertex also extends InOutLocalBounds,
	// in the actor's local space (Meshes/README.md's mm->cm + Y-mirror conversion already
	// applied), so TryBuildRealMesh can log and arithmetically check the whole assembly's size
	// instead of asking a human to eyeball it on screen.
	bool BuildPartFromStl(UProceduralMeshComponent* Component, const FString& StlBaseName, float ExtraYawDeg, FBox& InOutLocalBounds);

	bool bLatestSampleFallen = false;
};
