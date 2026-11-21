// Copyright Epic Games, Inc. All Rights Reserved.
using UnrealBuildTool;
using System.Collections.Generic;

public class SyphonePluginTarget : TargetRules
{
	public SyphonePluginTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("SyphonePlugin");
		AdditionalCompilerArguments = "-Wno-unused-command-line-argument";
	}
}
