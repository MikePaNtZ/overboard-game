using UnrealBuildTool;
using System.Collections.Generic;

public class OverboardGameTarget : TargetRules
{
	public OverboardGameTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("OverboardGame");
	}
}
