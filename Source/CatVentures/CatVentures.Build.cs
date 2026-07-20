// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class CatVentures : ModuleRules
{
	public CatVentures(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "OnlineSubsystem", "OnlineSubsystemUtils", "UMG", "Slate", "SlateCore", "GeometryCollectionEngine" });

		PrivateDependencyModuleNames.AddRange(new string[] { "Chaos", "DeveloperSettings" });

		// PawPrint window (editor-only Slate tab) — see PawPrintWindow.cpp
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[] { "WorkspaceMenuStructure", "Settings" });
		}

		DynamicallyLoadedModuleNames.Add("OnlineSubsystemSteam");

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
