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
