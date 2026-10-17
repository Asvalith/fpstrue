// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class fpstrueServerTarget : TargetRules
{
	// 无本地玩家的独立服务器目标；需要支持 Server 的 UE 构建，不依赖编辑器启动。
	public fpstrueServerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Server;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_5;
		ExtraModuleNames.Add("fpstrue");
	}
}
