// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class fpstrue : ModuleRules
{
	public fpstrue(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// 本模块按职责分目录；内部头文件统一使用模块根相对路径，避免依赖旧式隐式搜索路径。
		PrivateIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "AIModule", "NavigationSystem", "GameplayTasks", "UMG", "AnimationSharing" });
		PrivateDependencyModuleNames.Add("SignificanceManager");
	}
}
