// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueBenchmarkConfig.h"
#include "fpstrueEnemySignificance.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

/*
 * 自动测试的只读命令行快照。
 * 运行期间所有模块读取同一份静态配置，避免不同敌人在不同时间重复解析命令行，
 * TOptional 则区分“没有传参”和“显式传入 0/false”两种情况。
 */

namespace
{
// 从命令行读取一个可选数值；未提供时保持为空，避免覆盖编辑器配置。
template <typename TValue> TOptional<TValue> ParseOptionalValue(const TCHAR* CommandLine, const TCHAR* Key)
{
	TValue Value{};
	if (FParse::Value(CommandLine, Key, Value))
	{
		return Value;
	}

	return TOptional<TValue>();
}

// 只在命令行确实提供值时覆盖目标策略字段。
template <typename TValue> void ApplyOptionalValue(const TOptional<TValue>& Override, TValue& OutValue)
{
	if (Override.IsSet())
	{
		OutValue = Override.GetValue();
	}
}
} // namespace

const FFPBenchmarkConfig& FFPBenchmarkConfig::Get()
{
	// 首次使用时构造，后续 Enemy、AIController 和 GameMode 共享同一份快照。
	static const FFPBenchmarkConfig Config;
	return Config;
}

FFPBenchmarkConfig::FFPBenchmarkConfig()
{
	const TCHAR* CommandLine = FCommandLine::Get();

	bAutoBenchmark = FParse::Param(CommandLine, TEXT("AutoBenchmark"));
	bDisableAttackSweep = FParse::Param(CommandLine, TEXT("BenchmarkDisableAttackSweep"));
	bDisableEnemyPawnCollision = FParse::Param(CommandLine, TEXT("BenchmarkDisableEnemyPawnCollision"));
	bDisablePathFollowingTick = FParse::Param(CommandLine, TEXT("BenchmarkDisablePathFollowingTick"));
	bDisableCharacterMovementTick = FParse::Param(CommandLine, TEXT("BenchmarkDisableCharacterMovementTick"));
	bDisableSkeletalMeshTick = FParse::Param(CommandLine, TEXT("BenchmarkDisableSkeletalMeshTick"));
	bDisableEnemySignificance = FParse::Param(CommandLine, TEXT("BenchmarkDisableEnemySignificance")) ||
								FParse::Param(CommandLine, TEXT("BenchmarkDisableEnemyUpdateBudget"));
	bDisableMovementTiering = FParse::Param(CommandLine, TEXT("BenchmarkDisableMovementTiering"));
	bDisableShadowTiering = FParse::Param(CommandLine, TEXT("BenchmarkDisableShadowTiering"));
	bDisableEnemyRayTracing = FParse::Param(CommandLine, TEXT("BenchmarkEnemyRayTracingOff"));
	bDisableEnemyShadows = FParse::Param(CommandLine, TEXT("BenchmarkEnemyShadowsOff"));
	bDisableAnimationOptimizations = FParse::Param(CommandLine, TEXT("BenchmarkDisableAnimationOptimizations"));
	bDisableAIThrottling = FParse::Param(CommandLine, TEXT("BenchmarkDisableAIThrottling"));
	bDisableEnemyRenderTiering = FParse::Param(CommandLine, TEXT("BenchmarkDisableEnemyRenderTiering"));
	bDisableEnemySkeletalLOD = FParse::Param(CommandLine, TEXT("BenchmarkDisableEnemySkeletalLOD"));
	bDisableEnemyAnimationTiering = FParse::Param(CommandLine, TEXT("BenchmarkDisableEnemyAnimationTiering"));
	bDisableEnemyRayTracingTiering = FParse::Param(CommandLine, TEXT("BenchmarkDisableEnemyRayTracingTiering"));
	bDisableEnemyAnimationSharing = FParse::Param(CommandLine, TEXT("BenchmarkDisableEnemyAnimationSharing"));
	bDisableActiveAttackerBudget = FParse::Param(CommandLine, TEXT("BenchmarkDisableActiveAttackerBudget"));
	bDisableMoveToRequestBudget = FParse::Param(CommandLine, TEXT("BenchmarkDisableMoveToRequestBudget"));
	bCollectTextureStats = FParse::Param(CommandLine, TEXT("BenchmarkTextureStats"));
	bTakeScreenshot = FParse::Param(CommandLine, TEXT("BenchmarkScreenshot"));
	bAutoQuit = FParse::Param(CommandLine, TEXT("BenchmarkAutoQuit"));

	bHasEnemyCountOverride = FParse::Value(CommandLine, TEXT("BenchmarkEnemies="), EnemyCount);
	EnemyCount = FMath::Max(EnemyCount, 0);
	FParse::Value(CommandLine, TEXT("BenchmarkSeed="), Seed);
	FParse::Value(CommandLine, TEXT("BenchmarkWarmup="), WarmupSeconds);
	FParse::Value(CommandLine, TEXT("BenchmarkDuration="), DurationSeconds);
	WarmupSeconds = FMath::Max(WarmupSeconds, 0.0f);
	DurationSeconds = FMath::Max(DurationSeconds, 1.0f);

	if (FParse::Value(CommandLine, TEXT("BenchmarkTraceFile="), TraceFile))
	{
		TraceFile.TrimQuotesInline();
	}

	FrustumWeight = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigFrustumWeight="));
	ScreenCoverageWeight = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigScreenWeight="));
	RecentFrustumWeight = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigRecentWeight="));
	DistanceWeight = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigDistanceWeight="));
	ExpandedFrustumMargin = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigFrustumMargin="));
	RecentFrustumGraceSeconds = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigRecentGrace="));
	ScreenRadiusForFullScore = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigScreenRadius="));
	NearDistance = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigNearDistance="));
	FarDistance = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigFarDistance="));
	CombatPriorityGraceSeconds = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigCombatGrace="));
	FullEnterThreshold = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigFullEnter="));
	FullExitThreshold = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigFullExit="));
	ReducedEnterThreshold = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigReducedEnter="));
	ReducedExitThreshold = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigReducedExit="));
	DemotionDelaySeconds = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigDemotionDelay="));
	MinimumTierHoldSeconds = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigMinimumHold="));
	MaxFullRenderEnemies = ParseOptionalValue<int32>(CommandLine, TEXT("EnemySigFullBudget="));
	MaxShadowCastingEnemies = ParseOptionalValue<int32>(CommandLine, TEXT("EnemySigShadowBudget="));
	ShadowMaxDistance = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigShadowDistance="));
	MaxRayTracingEnemies = ParseOptionalValue<int32>(CommandLine, TEXT("EnemySigRayTracingBudget="));
	RayTracingMaxDistance = ParseOptionalValue<float>(CommandLine, TEXT("EnemySigRayTracingDistance="));
	FullMinLOD = ParseOptionalValue<int32>(CommandLine, TEXT("EnemySigFullMinLOD="));
	ReducedMinLOD = ParseOptionalValue<int32>(CommandLine, TEXT("EnemySigReducedMinLOD="));
	BackgroundMinLOD = ParseOptionalValue<int32>(CommandLine, TEXT("EnemySigBackgroundMinLOD="));
}

void FFPBenchmarkConfig::ApplyEnemySignificanceOverrides(FFPEnemyRenderSignificancePolicy& InOutPolicy) const
{
	// 未出现在命令行中的字段保持蓝图或 C++ 默认值，避免测试配置污染正式配置。
	ApplyOptionalValue(FrustumWeight, InOutPolicy.FrustumWeight);
	ApplyOptionalValue(ScreenCoverageWeight, InOutPolicy.ScreenCoverageWeight);
	ApplyOptionalValue(RecentFrustumWeight, InOutPolicy.RecentFrustumWeight);
	ApplyOptionalValue(DistanceWeight, InOutPolicy.DistanceWeight);
	ApplyOptionalValue(ExpandedFrustumMargin, InOutPolicy.ExpandedFrustumMargin);
	ApplyOptionalValue(RecentFrustumGraceSeconds, InOutPolicy.RecentFrustumGraceSeconds);
	ApplyOptionalValue(ScreenRadiusForFullScore, InOutPolicy.ScreenRadiusForFullScore);
	ApplyOptionalValue(NearDistance, InOutPolicy.NearDistance);
	ApplyOptionalValue(FarDistance, InOutPolicy.FarDistance);
	ApplyOptionalValue(CombatPriorityGraceSeconds, InOutPolicy.CombatPriorityGraceSeconds);
	ApplyOptionalValue(FullEnterThreshold, InOutPolicy.FullEnterThreshold);
	ApplyOptionalValue(FullExitThreshold, InOutPolicy.FullExitThreshold);
	ApplyOptionalValue(ReducedEnterThreshold, InOutPolicy.ReducedEnterThreshold);
	ApplyOptionalValue(ReducedExitThreshold, InOutPolicy.ReducedExitThreshold);
	ApplyOptionalValue(DemotionDelaySeconds, InOutPolicy.DemotionDelaySeconds);
	ApplyOptionalValue(MinimumTierHoldSeconds, InOutPolicy.MinimumTierHoldSeconds);
	ApplyOptionalValue(MaxFullRenderEnemies, InOutPolicy.MaxFullRenderEnemies);
	ApplyOptionalValue(MaxShadowCastingEnemies, InOutPolicy.MaxShadowCastingEnemies);
	ApplyOptionalValue(ShadowMaxDistance, InOutPolicy.ShadowMaxDistance);
	ApplyOptionalValue(MaxRayTracingEnemies, InOutPolicy.MaxRayTracingEnemies);
	ApplyOptionalValue(RayTracingMaxDistance, InOutPolicy.RayTracingMaxDistance);
	ApplyOptionalValue(FullMinLOD, InOutPolicy.FullMinLOD);
	ApplyOptionalValue(ReducedMinLOD, InOutPolicy.ReducedMinLOD);
	ApplyOptionalValue(BackgroundMinLOD, InOutPolicy.BackgroundMinLOD);

	if (bDisableEnemyRenderTiering)
	{
		InOutPolicy.bEnableRenderTiering = false;
	}
	if (bDisableEnemySkeletalLOD)
	{
		InOutPolicy.bEnableSkeletalLOD = false;
	}
	if (bDisableEnemyAnimationTiering)
	{
		InOutPolicy.bEnableAnimationTickTiering = false;
	}
	if (bDisableShadowTiering)
	{
		InOutPolicy.bEnableShadowBudget = false;
	}
	if (bDisableEnemyRayTracingTiering)
	{
		InOutPolicy.bEnableRayTracingBudget = false;
	}
}
