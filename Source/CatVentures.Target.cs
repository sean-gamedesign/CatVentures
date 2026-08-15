// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class CatVenturesTarget : TargetRules
{
	public CatVenturesTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		// Kept in step with CatVenturesEditor.Target.cs — see the note there.
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
		ExtraModuleNames.Add("CatVentures");
	}
}
