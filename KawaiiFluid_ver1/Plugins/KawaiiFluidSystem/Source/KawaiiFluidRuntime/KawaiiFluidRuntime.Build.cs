// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class KawaiiFluidRuntime : ModuleRules
{
	public KawaiiFluidRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		string ModulePath = ModuleDirectory;

		PublicIncludePaths.AddRange(
			new string[] {
				Path.Combine(ModulePath, "Public"),
				Path.Combine(ModulePath, "Public/Core"),
				Path.Combine(ModulePath, "Public/Physics"),
				Path.Combine(ModulePath, "Public/Collision"),
				Path.Combine(ModulePath, "Public/Components"),
				Path.Combine(ModulePath, "Public/Rendering"),
				Path.Combine(ModulePath, "Public/Data"),
				Path.Combine(ModulePath, "Public/Tests")
			}
		);

		PrivateIncludePaths.AddRange(
			new string[] {
				Path.Combine(ModulePath, "Private"),
				Path.Combine(ModulePath, "Private/Core"),
				Path.Combine(ModulePath, "Private/Physics"),
				Path.Combine(ModulePath, "Private/Collision"),
				Path.Combine(ModulePath, "Private/Components"),
				Path.Combine(ModulePath, "Private/Rendering"),
				Path.Combine(ModulePath, "Private/Data"),
				Path.Combine(ModulePath, "Private/Tests"),
				Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private"),
				Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Internal")
			}
		);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"RenderCore",
				"Renderer",
				"RHI",
				"Niagara"  // Niagara Component 사용을 위해 추가
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
				"Projects", // IPluginManager 위해 추가
				"RenderCore", // AddShaderSourceDirectoryMapping 위해 추가
				"Renderer"
			}
		);

		// Editor-only dependencies (FEditorDelegates for PIE sync)
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}
	}
}
