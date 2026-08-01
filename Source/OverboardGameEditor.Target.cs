using UnrealBuildTool;
using System.Collections.Generic;

public class OverboardGameEditorTarget : TargetRules
{
	public OverboardGameEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("OverboardGame");
	}
}
