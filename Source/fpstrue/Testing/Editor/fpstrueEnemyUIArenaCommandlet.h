// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Commandlets/Commandlet.h"
#include "fpstrueEnemyUIArenaCommandlet.generated.h"

// 只创建独立 UI/群体 AI 采集场地；不改正式地图、相机、玩家或敌人蓝图。
// -Terrain 创建独立地形验证图；-AuditTerrain 冷加载只读复核，均不修改原 UI 场地。
// 已有地形图仅允许 -ReplaceTerrainWithBackupRoot=<精确备份目录> 显式替换；不删除备份或旧基准。
UCLASS()
class UfpstrueEnemyUIArenaCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	UfpstrueEnemyUIArenaCommandlet();
	virtual int32 Main(const FString& Params) override;
};
