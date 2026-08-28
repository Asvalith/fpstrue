// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FFPEnemyRenderSignificancePolicy;

/** Benchmark 命令行快照：集中解析所有消融开关和显著性覆盖参数。 */
struct FPSTRUE_API FFPBenchmarkConfig
{
public:
	// 命令行是进程级只读输入，集中解析一次，避免 Gameplay 类各自维护同一组开关。
	static const FFPBenchmarkConfig& Get();

	// 判断命令行是否指定了固定敌人数。
	bool HasEnemyCountOverride() const { return bHasEnemyCountOverride; }
	// 把命令行中的可选参数覆盖到本次渲染显著性策略。
	void ApplyEnemySignificanceOverrides(FFPEnemyRenderSignificancePolicy& InOutPolicy) const;

	bool bAutoBenchmark = false;
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
	bool bCollectTextureStats = false;
	bool bTakeScreenshot = false;
	bool bAutoQuit = false;

	int32 EnemyCount = 0;
	int32 Seed = 1337;
	float WarmupSeconds = 10.0f;
	float DurationSeconds = 30.0f;
	FString TraceFile;

private:
	// 首次访问单例时解析一次命令行。
	FFPBenchmarkConfig();

	bool bHasEnemyCountOverride = false;
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
