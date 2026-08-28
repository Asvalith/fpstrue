// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemySignificanceCoordinator.h"
#include "fpstrueBenchmarkConfig.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyAnimationSharingCoordinator.h"
#include "fpstrueEnemyCharacter.h"
#include "fpstrueGameMode.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "SignificanceManager.h"

CSV_DEFINE_CATEGORY(fpstrueSignificance, true);

// ==================== 生命周期与策略初始化 ====================

UfpstrueEnemySignificanceCoordinator::UfpstrueEnemySignificanceCoordinator()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UfpstrueEnemySignificanceCoordinator::Start(AfpstrueGameMode* InGameMode)
{
	GameMode = InGameMode;
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr)
	{
		return;
	}

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	if (!OwnerGameMode->bEnableEnemySignificance || BenchmarkConfig.bDisableEnemySignificance)
	{
		return;
	}

	if (!bPolicyInitialized)
	{
		BenchmarkConfig.ApplyEnemySignificanceOverrides(OwnerGameMode->EnemyRenderSignificancePolicy);
		SanitizePolicy();
		bPolicyInitialized = true;
		UE_LOG(
			LogTemp, Display,
			TEXT("Enemy render significance: weights[F=%.2f S=%.2f R=%.2f D=%.2f] thresholds[full=%.2f/%.2f reduced=%.2f/%.2f] "
				 "budgets[full=%d shadow=%d rt=%d] features[tier=%d lod=%d anim=%d shadow=%d rt=%d]"),
			OwnerGameMode->EnemyRenderSignificancePolicy.FrustumWeight, OwnerGameMode->EnemyRenderSignificancePolicy.ScreenCoverageWeight,
			OwnerGameMode->EnemyRenderSignificancePolicy.RecentFrustumWeight, OwnerGameMode->EnemyRenderSignificancePolicy.DistanceWeight,
			OwnerGameMode->EnemyRenderSignificancePolicy.FullEnterThreshold, OwnerGameMode->EnemyRenderSignificancePolicy.FullExitThreshold,
			OwnerGameMode->EnemyRenderSignificancePolicy.ReducedEnterThreshold,
			OwnerGameMode->EnemyRenderSignificancePolicy.ReducedExitThreshold,
			OwnerGameMode->EnemyRenderSignificancePolicy.MaxFullRenderEnemies,
			OwnerGameMode->EnemyRenderSignificancePolicy.MaxShadowCastingEnemies,
			OwnerGameMode->EnemyRenderSignificancePolicy.MaxRayTracingEnemies,
			OwnerGameMode->EnemyRenderSignificancePolicy.bEnableRenderTiering ? 1 : 0,
			OwnerGameMode->EnemyRenderSignificancePolicy.bEnableSkeletalLOD ? 1 : 0,
			OwnerGameMode->EnemyRenderSignificancePolicy.bEnableAnimationTickTiering ? 1 : 0,
			OwnerGameMode->EnemyRenderSignificancePolicy.bEnableShadowBudget ? 1 : 0,
			OwnerGameMode->EnemyRenderSignificancePolicy.bEnableRayTracingBudget ? 1 : 0);
	}

	// 固定频率集中更新，避免每个敌人在 Tick 中各自评分、排序和争抢预算。
	GetWorld()->GetTimerManager().SetTimer(UpdateTimerHandle, this, &UfpstrueEnemySignificanceCoordinator::Update,
										   FMath::Max(OwnerGameMode->EnemySignificanceUpdateInterval, 0.1f), true, 0.1f);
}

void UfpstrueEnemySignificanceCoordinator::Stop()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(UpdateTimerHandle);
	}
}

void UfpstrueEnemySignificanceCoordinator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Stop();
	Super::EndPlay(EndPlayReason);
}

// ==================== 集中更新管线 ====================

void UfpstrueEnemySignificanceCoordinator::Update()
{
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr)
	{
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueGameMode_UpdateEnemySignificance);
	CSV_SCOPED_TIMING_STAT(fpstrueSignificance, UpdateTime);
	if (!IsValid(OwnerGameMode->PlayerCharacter))
	{
		return;
	}

	USignificanceManager* Manager = USignificanceManager::Get(GetWorld());
	if (Manager == nullptr)
	{
		return;
	}

	TArray<FTransform> Viewpoints;
	Viewpoints.Reserve(1);
	Viewpoints.Add(OwnerGameMode->PlayerCharacter->GetActorTransform());
	// Gameplay 层：Significance Manager 只按玩法目标距离更新 AI 与移动频率。
	Manager->Update(Viewpoints);

	APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
	if (PlayerController == nullptr)
	{
		return;
	}

	// Render 层：相机视锥、屏占比和相机距离只服务于渲染分级。
	FFPEnemyRenderViewContext ViewContext;
	PlayerController->GetPlayerViewPoint(ViewContext.ViewLocation, ViewContext.ViewRotation);
	ViewContext.HorizontalFOVDegrees =
		PlayerController->PlayerCameraManager != nullptr ? PlayerController->PlayerCameraManager->GetFOVAngle() : 90.0f;
	int32 ViewportWidth = 0;
	int32 ViewportHeight = 0;
	PlayerController->GetViewportSize(ViewportWidth, ViewportHeight);
	ViewContext.AspectRatio =
		ViewportWidth > 0 && ViewportHeight > 0 ? static_cast<float>(ViewportWidth) / static_cast<float>(ViewportHeight) : 16.0f / 9.0f;
	ViewContext.TimeSeconds = GetWorld()->GetTimeSeconds();

	struct FEnemyRenderCandidate
	{
		AfpstrueEnemyCharacter* Enemy = nullptr;
		FFPEnemyRenderSignificanceSample Sample;
		EFPEnemyRenderSignificanceTier NaturalTier = EFPEnemyRenderSignificanceTier::Background;
		EFPEnemyRenderSignificanceTier AssignedTier = EFPEnemyRenderSignificanceTier::Background;
		bool bGameplayAnimationProtection = false;
		bool bShouldCastShadow = false;
		bool bShouldBeVisibleInRayTracing = false;
	};

	// 采样阶段不改组件状态，确保所有敌人使用同一帧的观察条件。
	TArray<FEnemyRenderCandidate> Candidates;
	Candidates.Reserve(OwnerGameMode->RegisteredEnemies.Num());
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : OwnerGameMode->RegisteredEnemies)
	{
		AfpstrueEnemyCharacter* Enemy = EnemyPtr.Get();
		if (!IsValid(Enemy) || Enemy->IsDead())
		{
			continue;
		}

		FEnemyRenderCandidate& Candidate = Candidates.AddDefaulted_GetRef();
		Candidate.Enemy = Enemy;
		Candidate.Sample = Enemy->EvaluateRenderSignificance(ViewContext, OwnerGameMode->EnemyRenderSignificancePolicy);
		// 玩法保护是独立的正确性约束，不参与 RenderScore 和渲染预算排序。
		Candidate.bGameplayAnimationProtection = Enemy->RequiresGameplayAnimationProtection(
			ViewContext.TimeSeconds, OwnerGameMode->EnemyRenderSignificancePolicy.CombatPriorityGraceSeconds);
		Candidate.NaturalTier = Enemy->ResolveNaturalRenderSignificanceTier(Candidate.Sample, OwnerGameMode->EnemyRenderSignificancePolicy);
		Candidate.AssignedTier = Candidate.NaturalTier;
	}

	// 纯渲染排序：玩法保护不改变分数，也不争抢阴影或 RT 名额。
	Candidates.Sort(
		[](const FEnemyRenderCandidate& Left, const FEnemyRenderCandidate& Right)
		{
			// 主视锥使用字典序硬优先，避免多个低权重项相加后反超屏幕内敌人。
			if (Left.Sample.bInPrimaryFrustum != Right.Sample.bInPrimaryFrustum)
			{
				return Left.Sample.bInPrimaryFrustum;
			}
			if (Left.Sample.bInExpandedFrustum != Right.Sample.bInExpandedFrustum)
			{
				return Left.Sample.bInExpandedFrustum;
			}
			if (!FMath::IsNearlyEqual(Left.Sample.Score, Right.Sample.Score))
			{
				return Left.Sample.Score > Right.Sample.Score;
			}
			return Left.Enemy->GetUniqueID() < Right.Enemy->GetUniqueID();
		});

	// Render Tier 预算：先尊重自然档位，再限制 Full 档总量。
	int32 AssignedFullCount = 0;
	int32 FullBudgetDowngradeCount = 0;
	for (FEnemyRenderCandidate& Candidate : Candidates)
	{
		if (!OwnerGameMode->EnemyRenderSignificancePolicy.bEnableRenderTiering)
		{
			Candidate.AssignedTier = EFPEnemyRenderSignificanceTier::Full;
			++AssignedFullCount;
			continue;
		}

		if (Candidate.NaturalTier == EFPEnemyRenderSignificanceTier::Full)
		{
			if (AssignedFullCount < OwnerGameMode->EnemyRenderSignificancePolicy.MaxFullRenderEnemies)
			{
				Candidate.AssignedTier = EFPEnemyRenderSignificanceTier::Full;
				++AssignedFullCount;
			}
			else
			{
				Candidate.AssignedTier = EFPEnemyRenderSignificanceTier::Reduced;
				++FullBudgetDowngradeCount;
			}
		}
	}

	// 光追预算：只让 Full 且在距离内的高排序敌人参与动态 BLAS。
	int32 RayTracingVisibleCount = 0;
	int32 RayTracingBudgetRejectedCount = 0;
	for (FEnemyRenderCandidate& Candidate : Candidates)
	{
		if (!OwnerGameMode->EnemyRenderSignificancePolicy.bEnableRayTracingBudget)
		{
			Candidate.bShouldBeVisibleInRayTracing = true;
			continue;
		}

		const bool bRayTracingEligible = Candidate.AssignedTier == EFPEnemyRenderSignificanceTier::Full &&
										 Candidate.Sample.Distance <= OwnerGameMode->EnemyRenderSignificancePolicy.RayTracingMaxDistance;
		Candidate.bShouldBeVisibleInRayTracing =
			bRayTracingEligible && RayTracingVisibleCount < OwnerGameMode->EnemyRenderSignificancePolicy.MaxRayTracingEnemies;
		if (Candidate.bShouldBeVisibleInRayTracing)
		{
			++RayTracingVisibleCount;
		}
		else if (bRayTracingEligible)
		{
			++RayTracingBudgetRejectedCount;
		}
	}

	// 阴影预算：扩展视锥、距离和 Render Tier 共同决定候选资格。
	int32 ShadowCastingCount = 0;
	int32 ShadowBudgetRejectedCount = 0;
	for (FEnemyRenderCandidate& Candidate : Candidates)
	{
		if (!OwnerGameMode->EnemyRenderSignificancePolicy.bEnableShadowBudget)
		{
			Candidate.bShouldCastShadow = true;
			continue;
		}

		const bool bShadowEligible = Candidate.Sample.bInExpandedFrustum &&
									 Candidate.Sample.Distance <= OwnerGameMode->EnemyRenderSignificancePolicy.ShadowMaxDistance &&
									 Candidate.AssignedTier != EFPEnemyRenderSignificanceTier::Background;
		Candidate.bShouldCastShadow =
			bShadowEligible && ShadowCastingCount < OwnerGameMode->EnemyRenderSignificancePolicy.MaxShadowCastingEnemies;
		if (Candidate.bShouldCastShadow)
		{
			++ShadowCastingCount;
		}
		else if (bShadowEligible)
		{
			++ShadowBudgetRejectedCount;
		}
	}

	// 统一应用结果并在同一位置记录消融所需的 CSV 指标。
	int32 GameplayFullCount = 0;
	int32 GameplayReducedCount = 0;
	int32 GameplayBackgroundCount = 0;
	int32 RenderFullCount = 0;
	int32 RenderReducedCount = 0;
	int32 RenderBackgroundCount = 0;
	int32 LOD0Count = 0;
	int32 LOD1Count = 0;
	int32 LOD2PlusCount = 0;
	int32 AppliedShadowCastingCount = 0;
	int32 AppliedRayTracingVisibleCount = 0;
	int32 ExpandedFrustumCount = 0;
	int32 GameplayAnimationProtectionCount = 0;
	float ScoreSum = 0.0f;
	float FrustumFactorSum = 0.0f;
	float ScreenCoverageFactorSum = 0.0f;
	float RecentFrustumFactorSum = 0.0f;
	float DistanceFactorSum = 0.0f;
	for (FEnemyRenderCandidate& Candidate : Candidates)
	{
		Candidate.Enemy->ApplyRenderSignificanceTier(Candidate.AssignedTier, Candidate.bShouldCastShadow,
													 Candidate.bShouldBeVisibleInRayTracing, Candidate.bGameplayAnimationProtection,
													 OwnerGameMode->EnemyRenderSignificancePolicy);

		switch (Candidate.Enemy->GetGameplaySignificanceTier())
		{
		case EFPEnemySignificanceTier::Full:
			++GameplayFullCount;
			break;
		case EFPEnemySignificanceTier::Reduced:
			++GameplayReducedCount;
			break;
		case EFPEnemySignificanceTier::Background:
		default:
			++GameplayBackgroundCount;
			break;
		}

		switch (Candidate.Enemy->GetRenderSignificanceTier())
		{
		case EFPEnemyRenderSignificanceTier::Full:
			++RenderFullCount;
			break;
		case EFPEnemyRenderSignificanceTier::Reduced:
			++RenderReducedCount;
			break;
		case EFPEnemyRenderSignificanceTier::Background:
		default:
			++RenderBackgroundCount;
			break;
		}

		const int32 AppliedMinLOD = Candidate.Enemy->GetAppliedMinimumLOD();
		if (AppliedMinLOD <= 0)
		{
			++LOD0Count;
		}
		else if (AppliedMinLOD == 1)
		{
			++LOD1Count;
		}
		else
		{
			++LOD2PlusCount;
		}
		if (const USkeletalMeshComponent* CharacterMesh = Candidate.Enemy->GetMesh())
		{
			AppliedShadowCastingCount += CharacterMesh->CastShadow ? 1 : 0;
			AppliedRayTracingVisibleCount += CharacterMesh->bVisibleInRayTracing ? 1 : 0;
		}

		ExpandedFrustumCount += Candidate.Sample.bInExpandedFrustum ? 1 : 0;
		GameplayAnimationProtectionCount += Candidate.bGameplayAnimationProtection ? 1 : 0;
		ScoreSum += Candidate.Sample.Score;
		FrustumFactorSum += Candidate.Sample.FrustumFactor;
		ScreenCoverageFactorSum += Candidate.Sample.ScreenCoverageFactor;
		RecentFrustumFactorSum += Candidate.Sample.RecentFrustumFactor;
		DistanceFactorSum += Candidate.Sample.DistanceFactor;
	}

	const float CandidateCount = static_cast<float>(Candidates.Num());
	const float InverseCandidateCount = CandidateCount > 0.0f ? 1.0f / CandidateCount : 0.0f;
	CSV_CUSTOM_STAT(fpstrueSignificance, GameplayFull, GameplayFullCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, GameplayReduced, GameplayReducedCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, GameplayBackground, GameplayBackgroundCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, RenderFull, RenderFullCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, RenderReduced, RenderReducedCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, RenderBackground, RenderBackgroundCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, LOD0, LOD0Count, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, LOD1, LOD1Count, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, LOD2Plus, LOD2PlusCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, ShadowCasters, AppliedShadowCastingCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, RayTracingVisible, AppliedRayTracingVisibleCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, ExpandedFrustum, ExpandedFrustumCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, GameplayAnimationProtection, GameplayAnimationProtectionCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, FullBudgetDowngrades, FullBudgetDowngradeCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, ShadowBudgetRejected, ShadowBudgetRejectedCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, RayTracingBudgetRejected, RayTracingBudgetRejectedCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, AnimationSharingFollowers,
					OwnerGameMode->EnemyAnimationSharingCoordinator != nullptr
						? OwnerGameMode->EnemyAnimationSharingCoordinator->GetRegisteredEnemyCount()
						: 0,
					ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, MeanScore, ScoreSum * InverseCandidateCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, MeanFrustumFactor, FrustumFactorSum * InverseCandidateCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, MeanScreenFactor, ScreenCoverageFactorSum * InverseCandidateCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, MeanRecentFactor, RecentFrustumFactorSum * InverseCandidateCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, MeanDistanceFactor, DistanceFactorSum * InverseCandidateCount, ECsvCustomStatOp::Set);
}

// ==================== 策略校验 ====================

void UfpstrueEnemySignificanceCoordinator::SanitizePolicy()
{
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr)
	{
		return;
	}

	OwnerGameMode->EnemyRenderSignificancePolicy.FrustumWeight =
		FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.FrustumWeight, 0.0f);
	OwnerGameMode->EnemyRenderSignificancePolicy.ScreenCoverageWeight =
		FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.ScreenCoverageWeight, 0.0f);
	OwnerGameMode->EnemyRenderSignificancePolicy.RecentFrustumWeight =
		FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.RecentFrustumWeight, 0.0f);
	OwnerGameMode->EnemyRenderSignificancePolicy.DistanceWeight =
		FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.DistanceWeight, 0.0f);
	const float WeightSum =
		OwnerGameMode->EnemyRenderSignificancePolicy.FrustumWeight + OwnerGameMode->EnemyRenderSignificancePolicy.ScreenCoverageWeight +
		OwnerGameMode->EnemyRenderSignificancePolicy.RecentFrustumWeight + OwnerGameMode->EnemyRenderSignificancePolicy.DistanceWeight;
	if (WeightSum <= KINDA_SMALL_NUMBER)
	{
		OwnerGameMode->EnemyRenderSignificancePolicy.FrustumWeight = 1.0f;
	}

	OwnerGameMode->EnemyRenderSignificancePolicy.ExpandedFrustumMargin =
		FMath::Clamp(OwnerGameMode->EnemyRenderSignificancePolicy.ExpandedFrustumMargin, 0.0f, 1.0f);
	OwnerGameMode->EnemyRenderSignificancePolicy.RecentFrustumGraceSeconds =
		FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.RecentFrustumGraceSeconds, 0.0f);
	OwnerGameMode->EnemyRenderSignificancePolicy.ScreenRadiusForFullScore =
		FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.ScreenRadiusForFullScore, 0.001f);
	OwnerGameMode->EnemyRenderSignificancePolicy.NearDistance = FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.NearDistance, 0.0f);
	OwnerGameMode->EnemyRenderSignificancePolicy.FarDistance = FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.FarDistance,
																		  OwnerGameMode->EnemyRenderSignificancePolicy.NearDistance + 1.0f);
	OwnerGameMode->EnemyRenderSignificancePolicy.CombatPriorityGraceSeconds =
		FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.CombatPriorityGraceSeconds, 0.0f);

	OwnerGameMode->EnemyRenderSignificancePolicy.FullEnterThreshold =
		FMath::Clamp(OwnerGameMode->EnemyRenderSignificancePolicy.FullEnterThreshold, 0.0f, 1.0f);
	OwnerGameMode->EnemyRenderSignificancePolicy.FullExitThreshold =
		FMath::Clamp(OwnerGameMode->EnemyRenderSignificancePolicy.FullExitThreshold, 0.0f,
					 OwnerGameMode->EnemyRenderSignificancePolicy.FullEnterThreshold);
	OwnerGameMode->EnemyRenderSignificancePolicy.ReducedEnterThreshold =
		FMath::Clamp(OwnerGameMode->EnemyRenderSignificancePolicy.ReducedEnterThreshold, 0.0f,
					 OwnerGameMode->EnemyRenderSignificancePolicy.FullExitThreshold);
	OwnerGameMode->EnemyRenderSignificancePolicy.ReducedExitThreshold =
		FMath::Clamp(OwnerGameMode->EnemyRenderSignificancePolicy.ReducedExitThreshold, 0.0f,
					 OwnerGameMode->EnemyRenderSignificancePolicy.ReducedEnterThreshold);
	OwnerGameMode->EnemyRenderSignificancePolicy.DemotionDelaySeconds =
		FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.DemotionDelaySeconds, 0.0f);
	OwnerGameMode->EnemyRenderSignificancePolicy.MinimumTierHoldSeconds =
		FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.MinimumTierHoldSeconds, 0.0f);

	OwnerGameMode->EnemyRenderSignificancePolicy.MaxFullRenderEnemies =
		FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.MaxFullRenderEnemies, 0);
	OwnerGameMode->EnemyRenderSignificancePolicy.MaxShadowCastingEnemies =
		FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.MaxShadowCastingEnemies, 0);
	OwnerGameMode->EnemyRenderSignificancePolicy.ShadowMaxDistance =
		FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.ShadowMaxDistance, 0.0f);
	OwnerGameMode->EnemyRenderSignificancePolicy.MaxRayTracingEnemies =
		FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.MaxRayTracingEnemies, 0);
	OwnerGameMode->EnemyRenderSignificancePolicy.RayTracingMaxDistance =
		FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.RayTracingMaxDistance, 0.0f);
	OwnerGameMode->EnemyRenderSignificancePolicy.FullMinLOD = FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.FullMinLOD, 0);
	OwnerGameMode->EnemyRenderSignificancePolicy.ReducedMinLOD =
		FMath::Max(OwnerGameMode->EnemyRenderSignificancePolicy.ReducedMinLOD, OwnerGameMode->EnemyRenderSignificancePolicy.FullMinLOD);
	OwnerGameMode->EnemyRenderSignificancePolicy.BackgroundMinLOD = FMath::Max(
		OwnerGameMode->EnemyRenderSignificancePolicy.BackgroundMinLOD, OwnerGameMode->EnemyRenderSignificancePolicy.ReducedMinLOD);
}
