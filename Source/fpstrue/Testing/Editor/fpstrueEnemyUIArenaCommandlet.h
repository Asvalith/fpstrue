// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Commandlets/Commandlet.h"
#include "fpstrueEnemyUIArenaCommandlet.generated.h"

/**
 * 显式 Editor Commandlet：创建独立 160 AI 采集地图 → 烘焙导航 → 验证 160 个点均可达 → 保存新图。
 * 固定输出包已经存在时只报告并返回，不覆盖旧证据；正式地图、玩家/敌人蓝图和源资源不在写入范围。
 * 不随普通游戏启动运行，也不在此直接生成运行时敌人。
 */
UCLASS()
class UfpstrueEnemyUIArenaCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	// 标记为离线 Editor 工具，关闭客户端/服务器运行身份并输出控制台诊断。
	UfpstrueEnemyUIArenaCommandlet();
	// 返回 0 表示地图已存在或新图验证保存成功；任何构建/导航/保存失败返回 1。
	virtual int32 Main(const FString& Params) override;
};
