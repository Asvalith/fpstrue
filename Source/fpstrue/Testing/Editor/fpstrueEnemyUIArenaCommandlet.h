// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Commandlets/Commandlet.h"
#include "fpstrueEnemyUIArenaCommandlet.generated.h"

// 只创建独立 UI/群体 AI 采集场地；不改正式地图、相机、玩家或敌人蓝图。
UCLASS()
class UfpstrueEnemyUIArenaCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	UfpstrueEnemyUIArenaCommandlet();
	virtual int32 Main(const FString& Params) override;
};
