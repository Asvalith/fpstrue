// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "fpstrueWaveConfiguration.generated.h"

class AfpstrueEnemyCharacter;

// 单波敌人类型与数量配置，由 GameMode 的生成队列读取。
USTRUCT(BlueprintType)
struct FfpstrueWaveConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wave")
	TSubclassOf<AfpstrueEnemyCharacter> EnemyClass;

	// 元信息限制编辑器输入；运行时仍校验外部配置。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wave", meta = (ClampMin = "1"))
	int32 EnemyCount = 5;
};

/**
 * 策划可复用的波次配置。指定资产后，它完整接管波次与对局时长，不混用 GameMode 旧默认值。
 * 每波未指定敌人类时只回退到本资产的 DefaultEnemyClass；空 Waves 或缺失敌人类会使开局失败。
 */
UCLASS(BlueprintType)
class FPSTRUE_API UfpstrueWaveConfiguration : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Waves")
	TSubclassOf<AfpstrueEnemyCharacter> DefaultEnemyClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Waves")
	TArray<FfpstrueWaveConfig> Waves;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Waves", meta = (ClampMin = "0.0"))
	float WaveInterval = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Match", meta = (ClampMin = "1"))
	int32 GameDuration = 90;
};
