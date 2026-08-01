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
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		// The wire layer (packet decode/encode + the MuJoCo -> Unreal transform) lives at the
		// repo root in wire/, deliberately outside any UE module, so it stays a small,
		// engine-free C++17 harness that compiles and tests standalone (see wire/README.md).
		// This module does not fork a copy of it: PrivateWireBridge.cpp pulls the real .cpp
		// files in via #include so there is exactly one implementation. Only the include path
		// is needed here so `#include "OverboardWire.h"` resolves.
		string WireDir = Path.Combine(ModuleDirectory, "..", "..", "wire");
		PublicIncludePaths.Add(WireDir);
	}
}
