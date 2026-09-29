// Copyright Epic Games, Inc. All Rights Reserved.

#include "Testing/Benchmarks/fpstrueBenchmarkConfig.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

/*
 * 自动测试的只读命令行快照。
 * 只供采集流程使用；玩法开关复用 FFPRuntimeOptions 的进程快照，不再次解析。
 * 人数/生命值用独立存在标志区分未传参与显式赋值，其余参数保留默认值。
 */

const FFPBenchmarkConfig& FFPBenchmarkConfig::Get()
{
	// 采集器共享同一份配置；运行时覆盖由基类快照提供，不重新解析。
	static const FFPBenchmarkConfig Config;
	return Config;
}

FFPBenchmarkConfig::FFPBenchmarkConfig()
	: FFPRuntimeOptions(FFPRuntimeOptions::Get())
{
	// 所有开关只在首次读取时解析一次；正式基线不传 BenchmarkDisable*，消融脚本才显式启用单项关闭。
	const TCHAR* CommandLine = FCommandLine::Get();

	// 布尔参数用 FParse::Param 判断“是否出现”，不需要再为 true/false 解析字符串值。
	bAutoBenchmark = FParse::Param(CommandLine, TEXT("AutoBenchmark"));
	bCollectTextureStats = FParse::Param(CommandLine, TEXT("BenchmarkTextureStats"));
	bTakeScreenshot = FParse::Param(CommandLine, TEXT("BenchmarkScreenshot"));
	bAutoQuit = FParse::Param(CommandLine, TEXT("BenchmarkAutoQuit"));

	// 带值参数用 FParse::Value 解析，并在进入 Gameplay 前钳制到可执行范围。
	bHasEnemyCountOverride = FParse::Value(CommandLine, TEXT("BenchmarkEnemies="), EnemyCount);
	EnemyCount = FMath::Max(EnemyCount, 0);
	bHasPlayerHealthOverride = FParse::Value(CommandLine, TEXT("BenchmarkPlayerHealth="), PlayerHealth);
	if (bHasPlayerHealthOverride)
	{
		PlayerHealth = FMath::Max(PlayerHealth, 1.0f);
	}
	FParse::Value(CommandLine, TEXT("BenchmarkSeed="), Seed);
	FParse::Value(CommandLine, TEXT("BenchmarkWarmup="), WarmupSeconds);
	FParse::Value(CommandLine, TEXT("BenchmarkDuration="), DurationSeconds);
	WarmupSeconds = FMath::Max(WarmupSeconds, 0.0f);
	DurationSeconds = FMath::Max(DurationSeconds, 1.0f);

	if (FParse::Value(CommandLine, TEXT("BenchmarkTraceFile="), TraceFile))
	{
		TraceFile.TrimQuotesInline();
	}

}
