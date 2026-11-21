// Copyright Epic Games, Inc. All Rights Reserved.
using UnrealBuildTool;
using System.Collections.Generic;

public class SyphonePluginEditorTarget : TargetRules
{
	public SyphonePluginEditorTarget( TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("SyphonePlugin");
	}
}
