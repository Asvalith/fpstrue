// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

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
