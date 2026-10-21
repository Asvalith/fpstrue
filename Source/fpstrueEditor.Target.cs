// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

// 编辑器目标；玩法编辑和自动化工具的依赖在模块规则中按此目标启用。
public class fpstrueEditorTarget : TargetRules
{
	// 定义编辑器构建目标，允许玩法编辑和自动化测试使用编辑器专属依赖。
	public fpstrueEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_5;
		ExtraModuleNames.Add("fpstrue");
	}
}
