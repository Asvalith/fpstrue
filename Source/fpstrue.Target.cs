// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class fpstrueTarget : TargetRules
{
	// 定义独立游戏构建目标，采用 UE5.5 的构建与头文件顺序规则，并装载 fpstrue 模块。
	public fpstrueTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_5;
		ExtraModuleNames.Add("fpstrue");
	}
}
