// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UEAgentTool : ModuleRules
{
	public UEAgentTool(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"ApplicationCore",
				"AssetRegistry",
				"AssetTools",
				"BlueprintGraph",
				"ContentBrowser",
				"CoreUObject",
				"EditorFramework",
				"EditorInteractiveToolsFramework",
				"EditorStyle",
				"Engine",
				"HTTP",
				"InputCore",
				"InteractiveToolsFramework",
				"Json",
				"JsonUtilities",
				"LevelEditor",
				"Networking",
				"Projects",
				"Slate",
				"SlateCore",
				"Sockets",
				"ToolMenus",
				"UnrealEd"
			}
		);
	}
}
