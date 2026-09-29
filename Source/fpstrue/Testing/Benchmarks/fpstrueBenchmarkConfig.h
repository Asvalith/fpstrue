// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/fpstrueRuntimeOptions.h"

/**
 * Benchmark 采集配置：人数、时长、输出及采集开关。
 * 外部实验预设维护在 Tools/ExperimentProfiles；脚本生成命令行，本类只解析一次。
 * Runner 管理采集流程；玩法只消费 Runtime/fpstrueRuntimeOptions，不依赖本文件。
 * 正式玩法默认配置仍由 Config/ 和玩法资产维护，不在这里修改。
 */
struct FPSTRUE_API FFPBenchmarkConfig : public FFPRuntimeOptions
{
public:
	// 命令行是进程级只读输入，集中解析一次，避免 Gameplay 类各自维护同一组开关。
	static const FFPBenchmarkConfig& Get();

	// 判断命令行是否指定了固定敌人数。
	bool HasEnemyCountOverride() const { return bHasEnemyCountOverride; }
	// 判断自动测试是否显式要求提高玩家最大生命值。
	bool HasPlayerHealthOverride() const { return bHasPlayerHealthOverride; }

	bool bAutoBenchmark = false;
	bool bCollectTextureStats = false;
	bool bTakeScreenshot = false;
	bool bAutoQuit = false;

	int32 EnemyCount = 0;
	int32 Seed = 1337;
	float PlayerHealth = 0.0f;
	float WarmupSeconds = 10.0f;
	float DurationSeconds = 30.0f;
	FString TraceFile;

private:
	// 首次访问单例时解析一次命令行。
	FFPBenchmarkConfig();

	bool bHasEnemyCountOverride = false;
	bool bHasPlayerHealthOverride = false;
};
