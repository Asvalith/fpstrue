// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/fpstrueEnemySignificanceCoordinator.h"
#include "Testing/Benchmarks/fpstrueBenchmarkConfig.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/fpstrueEnemyAnimationSharingCoordinator.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Game/fpstrueGameMode.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "SignificanceManager.h"

CSV_DEFINE_CATEGORY(fpstrueSignificance, true);

/*
 * 多敌人的重要性集中采样与预算分配器。
 * 每轮先用同一个玩家/相机快照收集全部候选，再用有界 Top-K 分配 Full Render、阴影和 RT 名额，
 * 最后才把结果写回组件，避免敌人按注册顺序边计算边抢预算造成不稳定。
 *
 * 同一 Timer 只统一“采样时刻”，不同消费者仍保持独立语义：
 *   Gameplay：玩家距离 + 战斗保护 -> AI 决策倍率、CharacterMovement Tick 间隔。
 *   Render：视锥、屏占比、最近可见和相机距离 -> Render Tier、LOD、动画、阴影、RT、Animation Sharing。
 * 相机可见性绝不参与 Gameplay 评分，因此玩家身后的敌人仍能正常追击。
 */

// ==================== 生命周期与策略初始化 ====================

// Coordinator 不参与逐帧 Tick，只在统一低频时钟上采样并下发各消费者档位。
UfpstrueEnemySignificanceCoordinator::UfpstrueEnemySignificanceCoordinator()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UfpstrueEnemySignificanceCoordinator::Start(AfpstrueGameMode* InGameMode)
{
	// GameMode 只提供策略配置和敌人注册表；本组件拥有校验、集中更新、Top-K 选择及预算应用流程。
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
		const FFPEnemyRenderSignificancePolicy& Policy = OwnerGameMode->EnemyRenderSignificancePolicy;
		UE_LOG(LogTemp, Display,
			   TEXT("Enemy render significance: weights[F=%.2f S=%.2f R=%.2f D=%.2f] thresholds[full=%.2f/%.2f reduced=%.2f/%.2f] "
					"budgets[full=%d shadow=%d rt=%d] features[tier=%d lod=%d anim=%d shadow=%d rt=%d]"),
			   Policy.FrustumWeight, Policy.ScreenCoverageWeight, Policy.RecentFrustumWeight, Policy.DistanceWeight,
			   Policy.FullEnterThreshold, Policy.FullExitThreshold, Policy.ReducedEnterThreshold, Policy.ReducedExitThreshold,
			   Policy.MaxFullRenderEnemies, Policy.MaxShadowCastingEnemies, Policy.MaxRayTracingEnemies,
			   Policy.bEnableRenderTiering ? 1 : 0, Policy.bEnableSkeletalLOD ? 1 : 0, Policy.bEnableAnimationTickTiering ? 1 : 0,
			   Policy.bEnableShadowBudget ? 1 : 0, Policy.bEnableRayTracingBudget ? 1 : 0);
	}

	// 固定频率集中更新，避免每个敌人在 Tick 中各自评分、排序和争抢预算
	// 首轮采样错开开局创建峰值；启动正确性由 GameMode 的初始化顺序保证，而不是依赖此延迟。
	GetWorld()->GetTimerManager().SetTimer(UpdateTimerHandle, this, &UfpstrueEnemySignificanceCoordinator::Update,
										   FMath::Max(OwnerGameMode->EnemySignificanceUpdateInterval, 0.1f), true, 0.1f);
}

void UfpstrueEnemySignificanceCoordinator::Stop()
{
	// 对局结束时停止下一轮集中采样；敌人自己的死亡/组件清理由各自生命周期负责。
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(UpdateTimerHandle);
	}
}

void UfpstrueEnemySignificanceCoordinator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 组件退出前复用 Stop，保证关卡切换时 Timer 不再访问旧 GameMode。
	Stop();
	Super::EndPlay(EndPlayReason);
}

// ==================== 集中更新管线 ====================
/*
	 * 单轮固定阶段：
	 * 0.守卫检查(GameMode、PlayerCharacter 是否有效)
	 * 1.Gameplay：插件可用时用玩家 Transform 更新评分；插件缺失不阻断独立 Render 预算；
	 * 2. Render快照：从 PlayerCameraManager 创建唯一 Render ViewContext；
	 * 3. 收集全部存活敌人样本，用纯数值 Top-K 分配 Full/Shadow/RT 名额；
	 * 4. 统一写回组件，并记录“消费者数量 + 局部耗时”所需的 CSV 指标。
	 * 阶段之间不边采样边抢预算，保证结果只由本轮快照和稳定优先级决定。
	 */

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

	OwnerGameMode->PruneInvalidEnemyRegistrations();
	// 插件缺失只跳过 Gameplay 评分；自定义 Render 预算仍需独立执行。
	if (USignificanceManager* Manager = USignificanceManager::Get(GetWorld()))
	{
		const FTransform Viewpoint = OwnerGameMode->PlayerCharacter->GetActorTransform();
		Manager->Update(MakeArrayView(&Viewpoint, 1));
	}

	APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
	if (PlayerController == nullptr)
	{
		return;
	}

	// Render 层：相机视锥、屏占比和相机距离只服务于渲染分级。
	FFPEnemyRenderViewContext ViewContext;
	///采样阶段：统一视点、FOV、宽高比和采样时刻
	PlayerController->GetPlayerViewPoint(ViewContext.ViewLocation, ViewContext.ViewRotation);
	ViewContext.HorizontalFOVDegrees =
		PlayerController->PlayerCameraManager != nullptr ? PlayerController->PlayerCameraManager->GetFOVAngle() : 90.0f;
	int32 ViewportWidth = 0;
	int32 ViewportHeight = 0;
	PlayerController->GetViewportSize(ViewportWidth, ViewportHeight);
	ViewContext.AspectRatio =
		ViewportWidth > 0 && ViewportHeight > 0 ? static_cast<float>(ViewportWidth) / static_cast<float>(ViewportHeight) : 16.0f / 9.0f;
	ViewContext.TimeSeconds = GetWorld()->GetTimeSeconds();
	const FFPEnemyRenderSignificancePolicy& Policy = OwnerGameMode->EnemyRenderSignificancePolicy;

	// 采样阶段不改组件状态，确保所有敌人使用同一帧的观察条件；这也是“统一时钟”的含义。
	TArray<FEnemyRenderCandidate>& Candidates = CandidateBuffer;
	Candidates.Reset();
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
		Candidate.Sample = Enemy->EvaluateRenderSignificance(ViewContext, Policy);
		Candidate.PriorityKey.Score = Candidate.Sample.Score;
		Candidate.PriorityKey.TieBreakId = Enemy->GetUniqueID();
		Candidate.PriorityKey.bInPrimaryFrustum = Candidate.Sample.bInPrimaryFrustum;
		Candidate.PriorityKey.bInExpandedFrustum = Candidate.Sample.bInExpandedFrustum;
		// 玩法保护是独立的正确性约束，不参与 RenderScore 和渲染预算排序。
		Candidate.bGameplayAnimationProtection =
			Enemy->RequiresGameplayAnimationProtection(ViewContext.TimeSeconds, Policy.CombatPriorityGraceSeconds);
		Candidate.NaturalTier = Enemy->ResolveNaturalRenderSignificanceTier(Candidate.Sample, Policy);
		// 先把所有自然 Full 候选降为 Reduced，再只恢复优先级最高的 K 个
		//先讲解再恢复优先级最高的 K 个
		Candidate.AssignedTier = Policy.bEnableRenderTiering && Candidate.NaturalTier == EFPEnemyRenderSignificanceTier::Full
									 ? EFPEnemyRenderSignificanceTier::Reduced
									 : Candidate.NaturalTier;
		// 关闭某项预算时默认放行，开启时仅由对应 Top-K 授予资格，不再另扫一遍候选。
		Candidate.bShouldCastShadow = !Policy.bEnableShadowBudget;
		Candidate.bShouldBeVisibleInRayTracing = !Policy.bEnableRayTracingBudget;
	}

	// 三类预算复用同一个小型堆。默认名额不超过内联容量，避免为 Top-K 额外申请堆内存；
	// 配置超过内联容量时 TArray 仍可正常扩展。比较阶段只读取预计算的纯数值 PriorityKey。
	TArray<FFPEnemyRenderTopKEntry, TInlineAllocator<32>> TopKHeap;
	const auto SelectTopK = [&Candidates, &TopKHeap](int32 RequestedCount, const auto& IsEligible)
	{
		TopKHeap.Reset();
		const int32 SafeCount = FMath::Clamp(RequestedCount, 0, Candidates.Num());
		int32 EligibleCount = 0;
		for (int32 CandidateIndex = 0; CandidateIndex < Candidates.Num(); ++CandidateIndex)
		{
			const FEnemyRenderCandidate& Candidate = Candidates[CandidateIndex];
			if (!IsEligible(Candidate))
			{
				continue;
			}

			++EligibleCount;
			FPEnemyRenderTopKInsert(TopKHeap, SafeCount, CandidateIndex, Candidate.PriorityKey);
		}
		return EligibleCount;
	};

	// Render Tier 预算：先尊重自然档位，再限制 Full 档总量。
	int32 FullBudgetDowngradeCount = 0;
	// 禁用分档时 ResolveNaturalRenderSignificanceTier 已返回 Full，无须再次覆盖全体。
	if (Policy.bEnableRenderTiering)
	{
		const int32 EligibleFullCount = SelectTopK(Policy.MaxFullRenderEnemies, [](const FEnemyRenderCandidate& Candidate)
												   { return Candidate.NaturalTier == EFPEnemyRenderSignificanceTier::Full; });
		for (const FFPEnemyRenderTopKEntry& Entry : TopKHeap)
		{
			Candidates[Entry.CandidateIndex].AssignedTier = EFPEnemyRenderSignificanceTier::Full;
		}
		FullBudgetDowngradeCount = EligibleFullCount - TopKHeap.Num();
	}

	// 光追预算：只让 Full 且在距离内的高排序敌人参与动态 BLAS。
	int32 RayTracingBudgetRejectedCount = 0;
	if (Policy.bEnableRayTracingBudget)
	{
		const int32 EligibleRayTracingCount = SelectTopK(Policy.MaxRayTracingEnemies,
														 [&Policy](const FEnemyRenderCandidate& Candidate)
														 {
															 return Candidate.AssignedTier == EFPEnemyRenderSignificanceTier::Full &&
																	Candidate.Sample.Distance <= Policy.RayTracingMaxDistance;
														 });
		for (const FFPEnemyRenderTopKEntry& Entry : TopKHeap)
		{
			Candidates[Entry.CandidateIndex].bShouldBeVisibleInRayTracing = true;
		}
		RayTracingBudgetRejectedCount = EligibleRayTracingCount - TopKHeap.Num();
	}

	// 阴影预算：扩展视锥、距离和 Render Tier 共同决定候选资格。
	int32 ShadowBudgetRejectedCount = 0;
	if (Policy.bEnableShadowBudget)
	{
		const int32 EligibleShadowCount = SelectTopK(Policy.MaxShadowCastingEnemies,
													 [&Policy](const FEnemyRenderCandidate& Candidate)
													 {
														 return Candidate.Sample.bInExpandedFrustum &&
																Candidate.Sample.Distance <= Policy.ShadowMaxDistance &&
																Candidate.AssignedTier != EFPEnemyRenderSignificanceTier::Background;
													 });
		for (const FFPEnemyRenderTopKEntry& Entry : TopKHeap)
		{
			Candidates[Entry.CandidateIndex].bShouldCastShadow = true;
		}
		ShadowBudgetRejectedCount = EligibleShadowCount - TopKHeap.Num();
	}

	ApplyAndRecordCandidates(*OwnerGameMode, FullBudgetDowngradeCount, ShadowBudgetRejectedCount, RayTracingBudgetRejectedCount);
	// 不跨轮保存非拥有 Actor 指针，仅保留临时数组的分配容量。
	Candidates.Reset();
}

void UfpstrueEnemySignificanceCoordinator::ApplyAndRecordCandidates(const AfpstrueGameMode& OwnerGameMode, int32 FullBudgetDowngradeCount,
																	int32 ShadowBudgetRejectedCount, int32 RayTracingBudgetRejectedCount)
{
	// 统一应用结果并在同一位置记录消融所需的 CSV 指标。
	// 注意：统一采样不等于所有消费者必须无条件重写；组件内部仍应通过状态比较避免重复修改渲染状态。
	const FFPEnemyRenderSignificancePolicy& Policy = OwnerGameMode.EnemyRenderSignificancePolicy;
	const TArray<FEnemyRenderCandidate>& Candidates = CandidateBuffer;
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
	int32 ManagedMeshCount = 0;
	int32 ShadowMeshCount = 0;
	int32 RayTracingMeshCount = 0;
	int32 ShadowOwnerCount = 0;
	int32 RayTracingOwnerCount = 0;
	int32 ExpandedFrustumCount = 0;
	int32 GameplayAnimationProtectionCount = 0;
	float ScoreSum = 0.0f;
	float FrustumFactorSum = 0.0f;
	float ScreenCoverageFactorSum = 0.0f;
	float RecentFrustumFactorSum = 0.0f;
	float DistanceFactorSum = 0.0f;
	for (const FEnemyRenderCandidate& Candidate : Candidates)
	{
		Candidate.Enemy->ApplyRenderSignificanceTier(Candidate.AssignedTier, Candidate.bShouldCastShadow,
													 Candidate.bShouldBeVisibleInRayTracing, Candidate.bGameplayAnimationProtection,
													 Policy);

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
			// 保留旧 CSV 的主 Mesh 口径，避免附件加入后历史数据列含义悄悄变化。
			AppliedShadowCastingCount += CharacterMesh->CastShadow ? 1 : 0;
			AppliedRayTracingVisibleCount += CharacterMesh->bVisibleInRayTracing ? 1 : 0;
		}
		int32 Meshes, ShadowMeshes, RayTracingMeshes;
		Candidate.Enemy->GetRenderBudgetMeshCounts(Meshes, ShadowMeshes, RayTracingMeshes);
		ManagedMeshCount += Meshes;
		ShadowMeshCount += ShadowMeshes;
		RayTracingMeshCount += RayTracingMeshes;
		ShadowOwnerCount += ShadowMeshes > 0 ? 1 : 0;
		RayTracingOwnerCount += RayTracingMeshes > 0 ? 1 : 0;

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
	CSV_CUSTOM_STAT(fpstrueSignificance, AliveEnemies, Candidates.Num(), ECsvCustomStatOp::Set);
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
	// 名额以敌人为单位，组件数可能大于名额；均为实际标志读回，不是 GPU 执行统计。
	CSV_CUSTOM_STAT(fpstrueSignificance, ManagedMeshes, ManagedMeshCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, ShadowMeshes, ShadowMeshCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, RayTracingMeshes, RayTracingMeshCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, ShadowOwners, ShadowOwnerCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, RayTracingOwners, RayTracingOwnerCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, ExpandedFrustum, ExpandedFrustumCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, GameplayAnimationProtection, GameplayAnimationProtectionCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, FullBudgetDowngrades, FullBudgetDowngradeCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, ShadowBudgetRejected, ShadowBudgetRejectedCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, RayTracingBudgetRejected, RayTracingBudgetRejectedCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, AnimationSharingFollowers,
					OwnerGameMode.EnemyAnimationSharingCoordinator != nullptr
						? OwnerGameMode.EnemyAnimationSharingCoordinator->GetRegisteredEnemyCount()
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
	// 配置可能来自 CDO、蓝图或 Benchmark 命令行；进入热路径前集中修正，Update 中不再重复做防御性分支。
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr)
	{
		return;
	}

	FFPEnemyRenderSignificancePolicy& Policy = OwnerGameMode->EnemyRenderSignificancePolicy;
	Policy.FrustumWeight = FMath::Max(Policy.FrustumWeight, 0.0f);
	Policy.ScreenCoverageWeight = FMath::Max(Policy.ScreenCoverageWeight, 0.0f);
	Policy.RecentFrustumWeight = FMath::Max(Policy.RecentFrustumWeight, 0.0f);
	Policy.DistanceWeight = FMath::Max(Policy.DistanceWeight, 0.0f);
	const float WeightSum = Policy.FrustumWeight + Policy.ScreenCoverageWeight + Policy.RecentFrustumWeight + Policy.DistanceWeight;
	if (WeightSum <= KINDA_SMALL_NUMBER)
	{
		Policy.FrustumWeight = 1.0f;
	}

	Policy.ExpandedFrustumMargin = FMath::Clamp(Policy.ExpandedFrustumMargin, 0.0f, 1.0f);
	Policy.RecentFrustumGraceSeconds = FMath::Max(Policy.RecentFrustumGraceSeconds, 0.0f);
	Policy.ScreenRadiusForFullScore = FMath::Max(Policy.ScreenRadiusForFullScore, 0.001f);
	Policy.NearDistance = FMath::Max(Policy.NearDistance, 0.0f);
	Policy.FarDistance = FMath::Max(Policy.FarDistance, Policy.NearDistance + 1.0f);
	Policy.CombatPriorityGraceSeconds = FMath::Max(Policy.CombatPriorityGraceSeconds, 0.0f);

	Policy.FullEnterThreshold = FMath::Clamp(Policy.FullEnterThreshold, 0.0f, 1.0f);
	Policy.FullExitThreshold = FMath::Clamp(Policy.FullExitThreshold, 0.0f, Policy.FullEnterThreshold);
	Policy.ReducedEnterThreshold = FMath::Clamp(Policy.ReducedEnterThreshold, 0.0f, Policy.FullExitThreshold);
	Policy.ReducedExitThreshold = FMath::Clamp(Policy.ReducedExitThreshold, 0.0f, Policy.ReducedEnterThreshold);
	Policy.DemotionDelaySeconds = FMath::Max(Policy.DemotionDelaySeconds, 0.0f);
	Policy.MinimumTierHoldSeconds = FMath::Max(Policy.MinimumTierHoldSeconds, 0.0f);

	Policy.MaxFullRenderEnemies = FMath::Max(Policy.MaxFullRenderEnemies, 0);
	Policy.MaxShadowCastingEnemies = FMath::Max(Policy.MaxShadowCastingEnemies, 0);
	Policy.ShadowMaxDistance = FMath::Max(Policy.ShadowMaxDistance, 0.0f);
	Policy.MaxRayTracingEnemies = FMath::Max(Policy.MaxRayTracingEnemies, 0);
	Policy.RayTracingMaxDistance = FMath::Max(Policy.RayTracingMaxDistance, 0.0f);
	Policy.FullMinLOD = FMath::Max(Policy.FullMinLOD, 0);
	Policy.ReducedMinLOD = FMath::Max(Policy.ReducedMinLOD, Policy.FullMinLOD);
	Policy.BackgroundMinLOD = FMath::Max(Policy.BackgroundMinLOD, Policy.ReducedMinLOD);
}
