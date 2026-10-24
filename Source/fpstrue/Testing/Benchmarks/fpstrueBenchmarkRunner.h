// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Runtime/fpstrueRuntime.h"

class UWorld;
class UGameInstance;

// GameState supplies only the World. Explicit PvpBaseline is shared by Editor-game
// and Shipping clients; legacy PvpProfile/CSV/diagnostic gameplay stays non-Shipping.
FPSTRUE_API void StartPvpPerformanceProfile(UWorld* World);

// Shipping intentionally ignores general command-line map/URL overrides. This
// explicit diagnostic bootstrap accepts only a local baseline peer; it neither
// exposes arbitrary travel nor changes the normal JoinPvp/reconnect protocol.
enum class EFPPvpBaselineBootstrap : uint8 { NotRequested, Started, Rejected };
// 返回是否启动或拒绝了本机基线连接；未请求时不改正式 GameInstance 的连接流程。
FPSTRUE_API EFPPvpBaselineBootstrap StartPvpBaselineClient(UGameInstance* Instance);

/**
 * 旧网络 PvpProfile 仍使用的只读人数/种子契约，不再挂接单机 GameMode。
 * 当前网络160基线的参数、采集和完整验证保留在同文件的 PvpBaseline 实现中。
 */
struct FPSTRUE_API FFPBenchmarkConfig : public FFPRuntimeOptions
{
public:
	// 复用运行时快照，仅首次解析 Profile 仍消费的参数。
	static const FFPBenchmarkConfig& Get();
	bool HasEnemyCountOverride() const { return bHasEnemyCountOverride; }
	int32 EnemyCount = 0;
	int32 Seed = 1337;

private:
	FFPBenchmarkConfig();
	bool bHasEnemyCountOverride = false;
};
