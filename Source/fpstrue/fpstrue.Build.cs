// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class fpstrue : ModuleRules
{
	// 声明玩法模块的编译依赖；编辑器专属功能仅进入 Editor 目标，不引入游戏目标。
	public fpstrue(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// 本模块按职责分目录；内部头文件统一使用模块根相对路径，避免依赖旧式隐式搜索路径。
		PrivateIncludePaths.Add(ModuleDirectory);

		// 核心玩法依赖输入、UI、AI、导航与动画共享；这里是编译装配，不在运行时创建模块对象。
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine",
			"InputCore", "EnhancedInput", "UMG",
			"AIModule", "NavigationSystem", "GameplayTasks", "GameplayAbilities", "GameplayTags",
			"AnimationSharing"
		});
		// 重连票据使用平台安全随机源；失败时拒绝创建票据，不退回可预测标识。
		PrivateDependencyModuleNames.AddRange(new string[] { "SignificanceManager", "PlatformCrypto", "PlatformCryptoOpenSSL", "AnimationCore", "PhysicsCore" });
		if (Target.bBuildEditor)
		{
			// 可编辑行为树资产生成和编辑器自动化测试需要这些模块；Game 目标不依赖它们。
			PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "UMGEditor", "AIGraph", "BehaviorTreeEditor", "BlueprintGraph", "GraphEditor", "KismetCompiler", "AnimGraph", "AnimGraphRuntime", "AssetRegistry", "RHI", "RenderCore" });
		}
	}
}
