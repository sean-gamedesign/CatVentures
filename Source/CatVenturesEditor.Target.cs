// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class CatVenturesEditorTarget : TargetRules
{
	public CatVenturesEditorTarget( TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		// V7 (UE 5.8): adopts FPSemantics=Precise for editor targets and promotes
		// ReturnType / Dangling / UnreachableCode to Error. Not optional on an installed
		// engine — the Editor target shares build products with UnrealEditor, which 5.8
		// builds with those levels, and UBT refuses a target that differs.
		DefaultBuildSettings = BuildSettingsVersion.V7;
		// Deliberately left at 5_7: include-order changes break compiles in their own
		// right, and moving it is a separate change from the engine bump.
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
		ExtraModuleNames.Add("CatVentures");
	}
}
