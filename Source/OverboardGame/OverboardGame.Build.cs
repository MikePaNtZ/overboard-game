using System.IO;
using UnrealBuildTool;

public class OverboardGame : ModuleRules
{
	public OverboardGame(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"Sockets",
			"Networking",
			"ProceduralMeshComponent",
		});

		// HairStrandsCore: the render rider's hair groom (GroomComponent).
		PrivateDependencyModuleNames.AddRange(new string[] { "LevelSequence", "MovieScene", "AnimationCore", "HairStrandsCore" });

		// Landscape, MeshDescription (+ UnrealEd in the editor): UTrailBuildLibrary, the landscape and mesh import that
		// tools/trail/build_trail_level.py calls. Editor-only code; the game build links no editor.
		PrivateDependencyModuleNames.AddRange(new string[] { "Landscape", "MeshDescription", "StaticMeshDescription", "RHI", "RenderCore" });
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
		// ATrailScatterActor draws the PVE trees through UInstancedSkinnedMeshComponent, which 5.7
		// marks UE_EXPERIMENTAL. That attribute does not compile on a UCLASS outside the engine, so
		// the module accepts experimental API (only that component uses it).
		bValidateExperimentalApi = false;

		// The wire layer (packet decode/encode + the MuJoCo -> Unreal transform) lives at the
		// repo root in wire/, deliberately outside any UE module, so it stays a small,
		// engine-free C++17 harness that compiles and tests standalone (see wire/README.md).
		// This module does not fork a copy of it: WireBridge.cpp pulls the real .cpp files in
		// via #include so there is exactly one implementation. Only the include path is needed
		// here so `#include "OverboardWire.h"` resolves.
		string WireDir = Path.Combine(ModuleDirectory, "..", "..", "wire");
		PublicIncludePaths.Add(WireDir);

		// Same pattern for the STL loader (see mesh/README.md): engine-free, tested standalone,
		// pulled in via MeshBridge.cpp so there is exactly one implementation.
		string MeshDir = Path.Combine(ModuleDirectory, "..", "..", "mesh");
		PublicIncludePaths.Add(MeshDir);
	}
}
