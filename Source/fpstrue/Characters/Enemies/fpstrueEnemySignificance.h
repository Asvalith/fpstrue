// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "fpstrueEnemySignificance.generated.h"

UENUM(BlueprintType)
enum class EFPEnemyRenderSignificanceTier : uint8
{
	Full,
	Reduced,
	Background
};

/**
 * 敌人全局渲染显著性策略，由 GameMode 配置、Coordinator 校验和消费。
 *
 * 四个权重会在运行时归一化，不要求手工相加为 1。Gameplay Significance 仍按玩法目标距离和交互状态独立计算，
 * 本结构只控制渲染评分、档位、LOD、动画、阴影和硬件光追预算。
 */
USTRUCT(BlueprintType)
struct FPSTRUE_API FFPEnemyRenderSignificancePolicy
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Features")
	bool bEnableRenderTiering = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Features")
	bool bEnableSkeletalLOD = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Features")
	bool bEnableAnimationTickTiering = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Features")
	bool bEnableShadowBudget = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Features")
	bool bEnableRayTracingBudget = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Weights", meta = (ClampMin = "0.0"))
	float FrustumWeight = 0.45f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Weights", meta = (ClampMin = "0.0"))
	float ScreenCoverageWeight = 0.30f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Weights", meta = (ClampMin = "0.0"))
	float RecentFrustumWeight = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Weights", meta = (ClampMin = "0.0"))
	float DistanceWeight = 0.10f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Inputs", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ExpandedFrustumMargin = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Inputs", meta = (ClampMin = "0.0"))
	float RecentFrustumGraceSeconds = 0.50f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Inputs", meta = (ClampMin = "0.001"))
	float ScreenRadiusForFullScore = 0.10f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Inputs", meta = (ClampMin = "0.0"))
	float NearDistance = 1500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Score|Inputs", meta = (ClampMin = "1.0"))
	float FarDistance = 10000.0f;

	// 只控制战斗动画/LOD 的正确性保护窗口，不进入 RenderScore。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Animation Protection", meta = (ClampMin = "0.0"))
	float CombatPriorityGraceSeconds = 0.75f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tier|Hysteresis", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FullEnterThreshold = 0.70f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tier|Hysteresis", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FullExitThreshold = 0.60f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tier|Hysteresis", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ReducedEnterThreshold = 0.30f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tier|Hysteresis", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ReducedExitThreshold = 0.20f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tier|Hysteresis", meta = (ClampMin = "0.0"))
	float DemotionDelaySeconds = 0.50f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tier|Hysteresis", meta = (ClampMin = "0.0"))
	float MinimumTierHoldSeconds = 0.50f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Budget", meta = (ClampMin = "0"))
	int32 MaxFullRenderEnemies = 12;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Budget", meta = (ClampMin = "0"))
	int32 MaxShadowCastingEnemies = 5;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Budget", meta = (ClampMin = "0.0"))
	float ShadowMaxDistance = 5000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Budget", meta = (ClampMin = "0"))
	int32 MaxRayTracingEnemies = 12;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Budget", meta = (ClampMin = "0.0"))
	float RayTracingMaxDistance = 10000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LOD", meta = (ClampMin = "0"))
	int32 FullMinLOD = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LOD", meta = (ClampMin = "0"))
	int32 ReducedMinLOD = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LOD", meta = (ClampMin = "0"))
	int32 BackgroundMinLOD = 2;
};

// Coordinator 每轮从玩家相机生成一次，所有敌人共享同一份观察上下文。
struct FFPEnemyRenderViewContext
{
	FVector ViewLocation = FVector::ZeroVector;
	FRotator ViewRotation = FRotator::ZeroRotator;
	float HorizontalFOVDegrees = 90.0f;
	float AspectRatio = 16.0f / 9.0f;
	float TimeSeconds = 0.0f;
};

// 纯渲染采样结果：不得加入攻击、受击、威胁等玩法状态。
struct FFPEnemyRenderSignificanceSample
{
	float Score = 0.0f;
	float FrustumFactor = 0.0f;
	float ScreenCoverageFactor = 0.0f;
	float RecentFrustumFactor = 0.0f;
	float DistanceFactor = 0.0f;
	float Distance = MAX_flt;
	bool bInPrimaryFrustum = false;
	bool bInExpandedFrustum = false;
};

/**
 * 单轮渲染预算使用的不可变排序键。
 *
 * 排序优先级依次为主视锥、扩展视锥、评分、TieBreakId。TieBreakId 只在其余字段完全相同时生效，
 * 用于保证同一批候选的结果确定。若以后需要把相近评分视为同档，应在生成键之前显式量化评分，
 * 不能在比较器中使用 IsNearlyEqual；“近似相等”不具备传递性，会破坏排序算法要求的严格弱序。
 */
struct FFPEnemyRenderPriorityKey
{
	float Score = 0.0f;
	uint32 TieBreakId = 0;
	bool bInPrimaryFrustum = false;
	bool bInExpandedFrustum = false;
};

/** 按渲染重要性从高到低排列候选；同一视锥层级内，无效评分落到有效评分之后。 */
struct FFPEnemyRenderPriorityLess
{
	bool operator()(const FFPEnemyRenderPriorityKey& Left, const FFPEnemyRenderPriorityKey& Right) const
	{
		if (Left.bInPrimaryFrustum != Right.bInPrimaryFrustum)
		{
			return Left.bInPrimaryFrustum;
		}
		if (Left.bInExpandedFrustum != Right.bInExpandedFrustum)
		{
			return Left.bInExpandedFrustum;
		}

		const float LeftScore = FMath::IsFinite(Left.Score) ? FMath::Clamp(Left.Score, 0.0f, 1.0f) : -1.0f;
		const float RightScore = FMath::IsFinite(Right.Score) ? FMath::Clamp(Right.Score, 0.0f, 1.0f) : -1.0f;
		if (LeftScore != RightScore)
		{
			return LeftScore > RightScore;
		}
		return Left.TieBreakId < Right.TieBreakId;
	}
};

/**
 * 有界 Top-K 堆中的轻量条目。CandidateIndex 指回本轮连续候选数组，排序键按值保存，
 * 避免堆比较期间再访问 Actor、组件或分散内存。
 */
struct FFPEnemyRenderTopKEntry
{
	int32 CandidateIndex = INDEX_NONE;
	FFPEnemyRenderPriorityKey PriorityKey;
};

/** 让堆顶始终保存当前 Top-K 中优先级最低的条目，便于新候选在 O(log K) 内替换它。 */
struct FFPEnemyRenderTopKWorstFirst
{
	bool operator()(const FFPEnemyRenderTopKEntry& Left, const FFPEnemyRenderTopKEntry& Right) const
	{
		return FFPEnemyRenderPriorityLess{}(Right.PriorityKey, Left.PriorityKey);
	}
};

/**
 * 把候选并入一个容量为 MaxCount 的 Top-K 堆。
 *
 * 扫描 N 个候选时总复杂度为 O(N log K)，额外空间为 O(K)。输出堆不保证有序，
 * 但其中保存的集合与按 FFPEnemyRenderPriorityLess 完整排序后取前 K 个完全一致。
 */
template <typename AllocatorType>
FORCEINLINE void FPEnemyRenderTopKInsert(TArray<FFPEnemyRenderTopKEntry, AllocatorType>& TopKHeap, int32 MaxCount,
										int32 CandidateIndex, const FFPEnemyRenderPriorityKey& PriorityKey)
{
	if (MaxCount <= 0 || CandidateIndex == INDEX_NONE)
	{
		return;
	}

	const FFPEnemyRenderTopKEntry Entry{CandidateIndex, PriorityKey};
	const FFPEnemyRenderTopKWorstFirst WorstFirst;
	if (TopKHeap.Num() < MaxCount)
	{
		TopKHeap.HeapPush(Entry, WorstFirst);
		return;
	}

	check(!TopKHeap.IsEmpty());
	if (FFPEnemyRenderPriorityLess{}(Entry.PriorityKey, TopKHeap[0].PriorityKey))
	{
		TopKHeap.HeapPopDiscard(WorstFirst, EAllowShrinking::No);
		TopKHeap.HeapPush(Entry, WorstFirst);
	}
}
