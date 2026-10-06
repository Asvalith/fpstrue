// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// 运行时埋点声明：AI、生成和攻击模块写入，stat fpstruePerformance 与 Insights/测试器读取。
DECLARE_STATS_GROUP(TEXT("fpstrue Performance"), STATGROUP_fpstruePerformance, STATCAT_Advanced);

// AI 决策自身耗时与调用量，用来区分“决策函数昂贵”和“决策触发下游工作昂贵”。
DECLARE_CYCLE_STAT_EXTERN(TEXT("AI Decision Time"), STAT_fpstrueAIDecisionTime, STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("AI Decision Count"), STAT_fpstrueAIDecisionCount, STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("AI Move Request Count"), STAT_fpstrueAIMoveRequestCount, STATGROUP_fpstruePerformance,
									  FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("AI Move Budget Rejected Count"), STAT_fpstrueAIMoveBudgetRejectedCount,
									  STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("AI Attack Budget Rejected Count"), STAT_fpstrueAIAttackBudgetRejectedCount,
									  STATGROUP_fpstruePerformance, FPSTRUE_API);

// 波次生成耗时与数量，主要观察分帧生成是否消除了集中 Spawn 尖峰。
DECLARE_CYCLE_STAT_EXTERN(TEXT("Wave Spawn Time"), STAT_fpstrueWaveSpawnTime, STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Enemy Spawn Count"), STAT_fpstrueEnemySpawnCount, STATGROUP_fpstruePerformance, FPSTRUE_API);

// 近战窗口的 Sweep 耗时、执行次数和返回命中数，用于证明碰撞查询是否值得继续优化。
DECLARE_CYCLE_STAT_EXTERN(TEXT("Attack Sweep Time"), STAT_fpstrueAttackSweepTime, STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Attack Sweep Count"), STAT_fpstrueAttackSweepCount, STATGROUP_fpstruePerformance, FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Sweep Returned Hit Count"), STAT_fpstrueSweepReturnedHitCount, STATGROUP_fpstruePerformance,
									  FPSTRUE_API);

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Attack Window Update Count"), STAT_fpstrueAttackWindowUpdateCount, STATGROUP_fpstruePerformance,
									  FPSTRUE_API);

struct FFPEnemyRenderSignificancePolicy;

/**
 * 运行时性能策略的只读启动覆盖：玩法类只依赖此层，不依赖采集器。
 * 默认值不改变资产策略；旧 Benchmark* 命令行保留兼容，正式配置仍来自 Config/ 和玩法资产。
 * 实验预设在 Tools/ExperimentProfiles，Runner 只负责阶段、采集与验收。
 */
struct FPSTRUE_API FFPRuntimeOptions
{
	static const FFPRuntimeOptions& Get();
	void ApplyEnemySignificanceOverrides(FFPEnemyRenderSignificancePolicy& InOutPolicy) const;

	bool bDisableAttackSweep = false;
	bool bDisableEnemyPawnCollision = false;
	bool bDisablePathFollowingTick = false;
	bool bDisableCharacterMovementTick = false;
	bool bDisableSkeletalMeshTick = false;
	bool bDisableEnemySignificance = false;
	bool bDisableMovementTiering = false;
	bool bDisableShadowTiering = false;
	bool bDisableEnemyRayTracing = false;
	bool bDisableEnemyShadows = false;
	bool bDisableAnimationOptimizations = false;
	bool bDisableAIThrottling = false;
	bool bDisableEnemyRenderTiering = false;
	bool bDisableEnemySkeletalLOD = false;
	bool bDisableEnemyAnimationTiering = false;
	bool bDisableEnemyRayTracingTiering = false;
	bool bDisableEnemyAnimationSharing = false;
	bool bDisableActiveAttackerBudget = false;
	bool bDisableMoveToRequestBudget = false;

private:
	FFPRuntimeOptions();
	TOptional<float> FrustumWeight;
	TOptional<float> ScreenCoverageWeight;
	TOptional<float> RecentFrustumWeight;
	TOptional<float> DistanceWeight;
	TOptional<float> ExpandedFrustumMargin;
	TOptional<float> RecentFrustumGraceSeconds;
	TOptional<float> ScreenRadiusForFullScore;
	TOptional<float> NearDistance;
	TOptional<float> FarDistance;
	TOptional<float> CombatPriorityGraceSeconds;
	TOptional<float> FullEnterThreshold;
	TOptional<float> FullExitThreshold;
	TOptional<float> ReducedEnterThreshold;
	TOptional<float> ReducedExitThreshold;
	TOptional<float> DemotionDelaySeconds;
	TOptional<float> MinimumTierHoldSeconds;
	TOptional<int32> MaxFullRenderEnemies;
	TOptional<int32> MaxShadowCastingEnemies;
	TOptional<float> ShadowMaxDistance;
	TOptional<int32> MaxRayTracingEnemies;
	TOptional<float> RayTracingMaxDistance;
	TOptional<int32> FullMinLOD;
	TOptional<int32> ReducedMinLOD;
	TOptional<int32> BackgroundMinLOD;
};
