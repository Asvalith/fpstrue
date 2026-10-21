// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/Performance/fpstrueEnemyPerformance.h"
#include "Runtime/fpstrueRuntime.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Game/fpstrueGameMode.h"
#include "Game/fpstrueMultiplayer.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "SignificanceManager.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AnimationSharingManager.h"
#include "AnimationSharingSetup.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeExit.h"
#include "Math/RotationMatrix.h"

CSV_DEFINE_CATEGORY(fpstrueSignificance, true);

// 把随视点/策略变化的公共计算移出每敌人评分；保留除法和加法顺序，避免改动临界评分与预算选择。
FFPEnemyRenderScoringContext::FFPEnemyRenderScoringContext(const FFPEnemyRenderViewContext& ViewContext,
	const FFPEnemyRenderSignificancePolicy& Policy)
	: ViewLocation(ViewContext.ViewLocation), TimeSeconds(ViewContext.TimeSeconds)
{
	const FRotationMatrix ViewRotationMatrix(ViewContext.ViewRotation);
	ForwardAxis = ViewRotationMatrix.GetUnitAxis(EAxis::X);
	RightAxis = ViewRotationMatrix.GetUnitAxis(EAxis::Y);
	UpAxis = ViewRotationMatrix.GetUnitAxis(EAxis::Z);
	const float HalfHorizontalFOVRadians = FMath::DegreesToRadians(FMath::Clamp(ViewContext.HorizontalFOVDegrees, 5.0f, 170.0f) * 0.5f);
	HorizontalTangent = FMath::Max(FMath::Tan(HalfHorizontalFOVRadians), 0.01f);
	VerticalTangent = HorizontalTangent / FMath::Max(ViewContext.AspectRatio, 0.1f);
	ExpandedFrustumMargin = FMath::Max(Policy.ExpandedFrustumMargin, 0.0f);
	RecentGraceSeconds = FMath::Max(Policy.RecentFrustumGraceSeconds, 0.0f);
	ScreenRadiusForFullScore = FMath::Max(Policy.ScreenRadiusForFullScore, 0.001f);
	NearDistance = FMath::Max(Policy.NearDistance, 0.0f);
	DistanceRange = FMath::Max(Policy.FarDistance, NearDistance + 1.0f) - NearDistance;
	FrustumWeight = FMath::Max(Policy.FrustumWeight, 0.0f);
	ScreenCoverageWeight = FMath::Max(Policy.ScreenCoverageWeight, 0.0f);
	RecentFrustumWeight = FMath::Max(Policy.RecentFrustumWeight, 0.0f);
	DistanceWeight = FMath::Max(Policy.DistanceWeight, 0.0f);
	WeightSum = FMath::Max(FrustumWeight + ScreenCoverageWeight + RecentFrustumWeight + DistanceWeight, KINDA_SMALL_NUMBER);
}

FFPEnemyRenderSignificanceSample FFPEnemyRenderScoringContext::EvaluateBounds(const FVector& BoundsOrigin,
	float BoundsRadius, float& LastPrimaryFrustumTime) const
{
	FFPEnemyRenderSignificanceSample Sample;
	// 将包围球转换到相机坐标，估算屏幕中心位置及半径，保留边缘部分可见的候选。
	const FVector ToEnemy = BoundsOrigin - ViewLocation;
	Sample.Distance = ToEnemy.Size();
	const float ForwardDistance = FVector::DotProduct(ToEnemy, ForwardAxis);
	const float HorizontalDistance = FVector::DotProduct(ToEnemy, RightAxis);
	const float VerticalDistance = FVector::DotProduct(ToEnemy, UpAxis);
	const float SafeForwardDistance = FMath::Max(ForwardDistance, 1.0f);
	const float ProjectedHorizontalRadius = BoundsRadius / (SafeForwardDistance * HorizontalTangent);
	const float ProjectedVerticalRadius = BoundsRadius / (SafeForwardDistance * VerticalTangent);
	const float NormalizedHorizontalPosition = HorizontalDistance / (SafeForwardDistance * HorizontalTangent);
	const float NormalizedVerticalPosition = VerticalDistance / (SafeForwardDistance * VerticalTangent);

	// 主视锥给予更高优先级；扩展视锥作为镜头边缘缓冲，不检查墙体是否遮住敌人。
	// 不将远距或背面对象提前清零，以免改变近期可见性评分。
	const bool bBoundsInFront = ForwardDistance + BoundsRadius > 0.0f;
	// 用归一化屏幕范围加包围球半径检测相交；Margin 控制额外的镜头边缘宽限。
	const auto IntersectsFrustum = [bBoundsInFront, NormalizedHorizontalPosition, NormalizedVerticalPosition,
		ProjectedHorizontalRadius, ProjectedVerticalRadius](float Margin)
	{
		return bBoundsInFront && FMath::Abs(NormalizedHorizontalPosition) <= 1.0f + Margin + ProjectedHorizontalRadius &&
			FMath::Abs(NormalizedVerticalPosition) <= 1.0f + Margin + ProjectedVerticalRadius;
	};
	Sample.bInPrimaryFrustum = IntersectsFrustum(0.0f);
	Sample.bInExpandedFrustum = IntersectsFrustum(ExpandedFrustumMargin);
	Sample.FrustumFactor = Sample.bInPrimaryFrustum ? 1.0f : (Sample.bInExpandedFrustum ? 0.5f : 0.0f);
	// 刚离开主视锥的敌人短暂保留部分分数，降低转镜时的档位抖动。
	if (Sample.bInPrimaryFrustum)
	{
		LastPrimaryFrustumTime = TimeSeconds;
		Sample.RecentFrustumFactor = 1.0f;
	}
	else if (RecentGraceSeconds > 0.0f)
	{
		const float TimeSincePrimaryFrustum = TimeSeconds - LastPrimaryFrustumTime;
		Sample.RecentFrustumFactor = 1.0f - FMath::Clamp(TimeSincePrimaryFrustum / RecentGraceSeconds, 0.0f, 1.0f);
	}
	// 屏占比只奖励扩展视锥中的对象；距离独立平滑衰减，各输入最终限制到零至一。
	const float ProjectedScreenRadius = ForwardDistance > 0.0f ? FMath::Max(ProjectedHorizontalRadius, ProjectedVerticalRadius) : 0.0f;
	Sample.ScreenCoverageFactor = Sample.bInExpandedFrustum
		? FMath::Clamp(ProjectedScreenRadius / ScreenRadiusForFullScore, 0.0f, 1.0f) : 0.0f;
	Sample.DistanceFactor = 1.0f - FMath::Clamp((Sample.Distance - NearDistance) / DistanceRange, 0.0f, 1.0f);
	// 权重归一化后合成纯渲染分数，攻击或受击不会提高这个分数和预算排序。
	Sample.Score = FMath::Clamp((Sample.FrustumFactor * FrustumWeight + Sample.ScreenCoverageFactor * ScreenCoverageWeight +
		Sample.RecentFrustumFactor * RecentFrustumWeight + Sample.DistanceFactor * DistanceWeight) / WeightSum, 0.0f, 1.0f);
	return Sample;
}

/*
 * 多敌人的重要性集中采样与预算分配器。
 * 每轮先用同一批玩家位置/本端相机快照收集全部候选，再用有界 Top-K 分配 Full Render、阴影和 RT 名额，
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

// 从 GameMode 取得配置和敌人集合，首次启动先应用诊断覆盖并校验，再建立集中采样 Timer。
void UfpstrueEnemySignificanceCoordinator::Start(AfpstrueGameMode* InGameMode)
{
	// GameMode 只提供策略配置和敌人注册表；本组件拥有校验、集中更新、Top-K 选择及预算应用流程。
	bMultiplayerMode = false;
	GameMode = InGameMode;
	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	if (OwnerGameMode == nullptr)
	{
		return;
	}

	const FFPRuntimeOptions& BenchmarkConfig = FFPRuntimeOptions::Get();
	if (!OwnerGameMode->bEnableEnemySignificance || BenchmarkConfig.bDisableEnemySignificance)
	{
		return;
	}

	if (!bPolicyInitialized)
	{
		FFPEnemyRenderSignificancePolicy& Policy = OwnerGameMode->EnemyRenderSignificancePolicy;
		BenchmarkConfig.ApplyEnemySignificanceOverrides(Policy);
		SanitizePolicy(Policy);
		bPolicyInitialized = true;
		UE_LOG(LogTemp, Display,
			   TEXT("Enemy render significance: weights[F=%.2f S=%.2f R=%.2f D=%.2f] thresholds[full=%.2f/%.2f reduced=%.2f/%.2f] "
					"budgets[full=%d shadow=%d rt=%d] features[tier=%d lod=%d anim=%d shadow=%d rt=%d]"),
			   Policy.FrustumWeight, Policy.ScreenCoverageWeight, Policy.RecentFrustumWeight, Policy.DistanceWeight,
			   Policy.FullEnterThreshold, Policy.FullExitThreshold, Policy.ReducedEnterThreshold, Policy.ReducedExitThreshold,
			   Policy.MaxFullRenderEnemies, Policy.MaxShadowCastingEnemies, Policy.MaxRayTracingEnemies,
			   Policy.bEnableRenderTiering ? 1 : 0, Policy.bEnableSkeletalLOD ? 1 : 0, Policy.bEnableAnimationTickTiering ? 1 : 0,
			   Policy.bEnableShadowBudget ? 1 : 0, Policy.bEnableRayTracingBudget ? 1 : 0);
	}
	ActiveRenderPolicy = OwnerGameMode->EnemyRenderSignificancePolicy;
	AnimationSharingCoordinator = OwnerGameMode->EnemyAnimationSharingCoordinator;
	bRunning = true;

	// 固定频率集中更新，避免每个敌人在 Tick 中各自评分、排序和争抢预算
	// 首轮采样错开开局创建峰值；启动正确性由 GameMode 的初始化顺序保证，而不是依赖此延迟。
	GetWorld()->GetTimerManager().SetTimer(UpdateTimerHandle, this, &UfpstrueEnemySignificanceCoordinator::Update,
										   FMath::Max(OwnerGameMode->EnemySignificanceUpdateInterval, 0.1f), true, 0.1f);
}

// 同一套集中管线在各端独立运行；GameState 只提供生命周期，不要求客户端拥有 GameMode 或 AIController。
void UfpstrueEnemySignificanceCoordinator::StartMultiplayer(UfpstrueEnemyAnimationSharingCoordinator* InSharingCoordinator)
{
	if (bRunning || GetWorld() == nullptr || !bEnableMultiplayerSignificance || FFPRuntimeOptions::Get().bDisableEnemySignificance)
	{
		return;
	}
	GameMode.Reset();
	bMultiplayerMode = true;
	AnimationSharingCoordinator = InSharingCoordinator;
	ActiveRenderPolicy = MultiplayerRenderPolicy;
	FFPRuntimeOptions::Get().ApplyEnemySignificanceOverrides(ActiveRenderPolicy);
	SanitizePolicy(ActiveRenderPolicy);
	bSharingStartAttempted = false;
	bReportedPopulatedUpdate = false;
	bReportedRenderUpdate = false;
	bReportedSharingFollowers = false;
	bRunning = true;
	GetWorld()->GetTimerManager().SetTimer(UpdateTimerHandle, this, &UfpstrueEnemySignificanceCoordinator::Update,
		FMath::Max(MultiplayerUpdateInterval, 0.1f), true, 0.1f);
	UE_LOG(LogTemp, Display, TEXT("PVP_PERFORMANCE START NetMode=%d Gameplay=%d Render=%d Interval=%.3f"),
		static_cast<int32>(GetNetMode()), GetOwner()->HasAuthority() ? 1 : 0, GetNetMode() != NM_DedicatedServer ? 1 : 0,
		FMath::Max(MultiplayerUpdateInterval, 0.1f));
}

// 停止后续采样，不替代敌人自身的死亡、组件退出和共享注销流程。
void UfpstrueEnemySignificanceCoordinator::Stop()
{
	bRunning = false;
	FullBudgetHolders.Reset();
	ShadowBudgetHolders.Reset();
	RayTracingBudgetHolders.Reset();
	// 对局结束时停止下一轮集中采样；敌人自己的死亡/组件清理由各自生命周期负责。
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(UpdateTimerHandle);
	}
}

// 关卡或组件退出时清除本组件拥有的采样 Timer，然后交给引擎完成退出。
void UfpstrueEnemySignificanceCoordinator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 组件退出前复用 Stop，保证关卡切换时 Timer 不再访问旧 GameMode。
	Stop();
	Super::EndPlay(EndPlayReason);
}

// ==================== 集中更新管线 ====================
/*
	 * 单轮固定阶段：
	 * 0. 检查运行上下文（单机使用 GameMode 注册表，多人发现本 World 副本）；
	 * 1. 收齐存活敌人并采样一次近战保护，借给本轮 Gameplay、Render 和 Sharing 使用；
	 * 2. Gameplay：权威端用有效玩家 Transform 更新评分；专服到此结束，客户端不执行此阶段；
	 * 3. 从 PlayerCameraManager 创建唯一 Render ViewContext，填充样本并用纯数值 Top-K 分配名额；
	 * 4. 统一写回组件，并记录“消费者数量 + 局部耗时”所需的 CSV 指标。
	 * 阶段之间不边采样边抢预算；本轮快照与本端各预算自己的名额历史共同决定结果。
	 */

// 【设计取舍】集中一轮发现、位置/近战采样与相机派生值复用，省重复读取，不承诺消除 O(N) 遍历。
// 单机复用登记表，多人保留 World 类型迭代以覆盖晚复制/脚本生成；不是每个 AI 再搜索全部玩家。
// 两端只共享状态事实：服务器 Gameplay 档位不看镜头，客户端 Render 评分与预算不回传授权。
void UfpstrueEnemySignificanceCoordinator::Update()
{

	AfpstrueGameMode* OwnerGameMode = GameMode.Get();
	// 候选非空意味着上一轮仍在同步应用；拒绝重入，避免重置被 Enemy 借用的候选地址。
	if (!bRunning || (!bMultiplayerMode && OwnerGameMode == nullptr) || !CandidateBuffer.IsEmpty())
	{
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueGameMode_UpdateEnemySignificance);
	CSV_SCOPED_TIMING_STAT(fpstrueSignificance, UpdateTime);
	if (!bMultiplayerMode && !IsValid(OwnerGameMode->PlayerCharacter))
	{
		return;
	}

	TArray<FEnemyRenderCandidate>& Candidates = CandidateBuffer;
	Candidates.Reset();
	// 类型迭代只发现本 World 已存在的敌人；晚复制、相关性重建和重连无需额外维护一份客户端注册表。
	// 多人 GameMode 的 PveEnemies 是人口管理登记，并未覆盖任意脚本/流送后的生成，不能直接代替全部敌人发现。
	if (bMultiplayerMode)
	{
		for (TActorIterator<AfpstrueEnemyCharacter> It(GetWorld()); It; ++It)
		{
			if (It->IsDead()) continue;
			Candidates.AddDefaulted_GetRef().Enemy = *It;
		}
	}
	else
	{
		OwnerGameMode->PruneInvalidEnemyRegistrations();
		Candidates.Reserve(OwnerGameMode->RegisteredEnemies.Num());
		for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : OwnerGameMode->RegisteredEnemies)
		{
			AfpstrueEnemyCharacter* Enemy = EnemyPtr.Get();
			if (IsValid(Enemy) && !Enemy->IsDead()) Candidates.AddDefaulted_GetRef().Enemy = Enemy;
		}
	}
	// 先收齐候选再借出地址，之后不扩容；同轮 Gameplay、Render 和 Sharing 只采样一次近战遮挡。
	for (FEnemyRenderCandidate& Candidate : Candidates)
	{
		Candidate.bCloseCombatProtection = Candidate.Enemy->IsAttacking() || Candidate.Enemy->IsTargetInAttackRange();
		Candidate.Enemy->SignificanceCombatProtectionSample = &Candidate.bCloseCombatProtection;
		// ★【重要边界】[线程交接] 插件只读取本轮位置值；Update 同步等待并行评分结束后，游戏线程才继续修改快照。
		Candidate.Enemy->SignificancePositionSnapshot = Candidate.Enemy->GetActorLocation();
		Candidate.Enemy->bHasSignificancePositionSnapshot = true;
	}
	ON_SCOPE_EXIT
	{
		for (const FEnemyRenderCandidate& Candidate : Candidates)
		{
			Candidate.Enemy->SignificanceCombatProtectionSample = nullptr;
		}
		// 不跨轮保存非拥有 Actor 指针，仅保留临时数组的分配容量。
		Candidates.Reset();
	};
	LastTrackedEnemyCount = Candidates.Num();
	LastGameplayViewpointCount = 0;
	++UpdateSerial;
	// 插件缺失只跳过 Gameplay 评分；自定义 Render 预算仍需独立执行。客户端绝不写入服务器玩法档位。
	if (GetOwner()->HasAuthority())
	{
		// Wall scope 包括插件可能执行的并行评分与同步等待，不冒充纯 on-core 时间。
		TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemySignificance_GameplayUpdate);
		TArray<FTransform, TInlineAllocator<8>> GameplayViewpoints;
		if (bMultiplayerMode)
		{
			if (const AfpstruePvpGameMode* MultiplayerMode = GetWorld()->GetAuthGameMode<AfpstruePvpGameMode>())
				for (TActorIterator<AfpstrueCharacter> It(GetWorld()); It; ++It)
					if (MultiplayerMode->IsCombatParticipant(*It)) GameplayViewpoints.Add(It->GetActorTransform());
		}
		else GameplayViewpoints.Add(OwnerGameMode->PlayerCharacter->GetActorTransform());
		LastGameplayViewpointCount = GameplayViewpoints.Num();
		if (USignificanceManager* Manager = USignificanceManager::Get(GetWorld()))
		{
			// 默认降序策略取各视点的最大评分：靠近任意有效玩家都保持相应响应，不取平均削弱远端玩家。
			Manager->Update(MakeArrayView(GameplayViewpoints));
		}
	}
	if (bMultiplayerMode && !Candidates.IsEmpty() && !bReportedPopulatedUpdate)
	{
		bReportedPopulatedUpdate = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_PERFORMANCE POPULATED NetMode=%d Enemies=%d GameplayViews=%d Render=%d"),
			static_cast<int32>(GetNetMode()), LastTrackedEnemyCount, LastGameplayViewpointCount,
			GetNetMode() != NM_DedicatedServer ? 1 : 0);
	}
	if (GetNetMode() == NM_DedicatedServer) return;

	// 多人客户端没有服务器 GameMode 配置和 AIController；共享池只在首次发现兼容敌人后创建一次。
	if (bMultiplayerMode && !GetOwner()->HasAuthority() && !Candidates.IsEmpty() && !bSharingStartAttempted)
	{
		bSharingStartAttempted = true;
		if (UfpstrueEnemyAnimationSharingCoordinator* Sharing = AnimationSharingCoordinator.Get())
			Sharing->Start(Candidates[0].Enemy->GetClass());
	}
	if (bMultiplayerMode)
		for (FEnemyRenderCandidate& Candidate : Candidates)
			Candidate.Enemy->SetAnimationSharingCoordinator(GetOwner()->HasAuthority() ? nullptr : AnimationSharingCoordinator.Get());

	APlayerController* PlayerController = UGameplayStatics::GetPlayerController(this, 0);
	if (PlayerController == nullptr || !PlayerController->IsLocalController())
	{
		return;
	}

	int32 FullBudgetDowngradeCount = 0, RayTracingBudgetRejectedCount = 0, ShadowBudgetRejectedCount = 0;
	{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemySignificance_RenderScoreAndBudget);
	// Render 层：相机视锥、屏占比和相机距离只服务于渲染分级。
	FFPEnemyRenderViewContext ViewContext;
	// 采样阶段：统一视点、FOV、宽高比和采样时刻。
	PlayerController->GetPlayerViewPoint(ViewContext.ViewLocation, ViewContext.ViewRotation);
	ViewContext.HorizontalFOVDegrees =
		PlayerController->PlayerCameraManager != nullptr ? PlayerController->PlayerCameraManager->GetFOVAngle() : 90.0f;
	int32 ViewportWidth = 0;
	int32 ViewportHeight = 0;
	PlayerController->GetViewportSize(ViewportWidth, ViewportHeight);
	ViewContext.AspectRatio =
		ViewportWidth > 0 && ViewportHeight > 0 ? static_cast<float>(ViewportWidth) / static_cast<float>(ViewportHeight) : 16.0f / 9.0f;
	ViewContext.TimeSeconds = GetWorld()->GetTimeSeconds();
	const FFPEnemyRenderSignificancePolicy& Policy = ActiveRenderPolicy;
	const FFPEnemyRenderScoringContext ScoringContext(ViewContext, Policy);

	// 采样阶段不改组件状态，确保所有敌人使用同一帧的观察条件；这也是“统一时钟”的含义。
	// ★【重要边界】[预算保护] Gameplay 评分控制决策和移动，Render 快照控制表现预算；统一采样时刻不代表共享评分或高层行为。
	for (FEnemyRenderCandidate& Candidate : Candidates)
	{
		AfpstrueEnemyCharacter* Enemy = Candidate.Enemy;
		Candidate.Sample = Enemy->EvaluateRenderSignificance(ScoringContext);
		Enemy->GetRenderBudgetEligibility(Candidate.bShadowEligible, Candidate.bRayTracingEligible);
		Candidate.PriorityKey.Score = Candidate.Sample.Score;
		Candidate.PriorityKey.TieBreakId = Enemy->GetUniqueID();
		Candidate.PriorityKey.bInPrimaryFrustum = Candidate.Sample.bInPrimaryFrustum;
		Candidate.PriorityKey.bInExpandedFrustum = Candidate.Sample.bInExpandedFrustum;
		// 玩法保护是独立的正确性约束，不参与 RenderScore 和渲染预算排序。
		Candidate.bGameplayAnimationProtection =
			Enemy->RequiresGameplayAnimationProtection(ViewContext.TimeSeconds, Policy.CombatPriorityGraceSeconds);
		Candidate.NaturalTier = Enemy->ResolveNaturalRenderSignificanceTier(Candidate.Sample, Policy);
	}
	AllocatePresentationBudgets(FullBudgetDowngradeCount, ShadowBudgetRejectedCount, RayTracingBudgetRejectedCount);
	}

	// ★【重要边界】[线程交接] 预算选择完成后在当前游戏线程统一写回组件；候选裸指针只用于本轮同步处理，不作为 Worker 或跨轮数据。
	ApplyAndRecordCandidates(FullBudgetDowngradeCount, ShadowBudgetRejectedCount, RayTracingBudgetRejectedCount);
	++RenderUpdateSerial;
	if (bMultiplayerMode && !Candidates.IsEmpty() && !bReportedRenderUpdate)
	{
		bReportedRenderUpdate = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_PERFORMANCE RENDER_APPLIED NetMode=%d Enemies=%d GameplayViews=%d"),
			static_cast<int32>(GetNetMode()), Candidates.Num(), LastGameplayViewpointCount);
	}
	if (bMultiplayerMode && !bReportedSharingFollowers && AnimationSharingCoordinator.IsValid() &&
		AnimationSharingCoordinator->GetRegisteredEnemyCount() > 0)
	{
		bReportedSharingFollowers = true;
		UE_LOG(LogTemp, Display, TEXT("PVP_PERFORMANCE SHARING_ACTIVE NetMode=%d Followers=%d"),
			static_cast<int32>(GetNetMode()), AnimationSharingCoordinator->GetRegisteredEnemyCount());
	}
}

// 【设计取舍】小 K 时用有界堆替代全排序；名额保持加分减少邻近评分抢占，接受并非瞬时最高原始分。
// 自然档位另有迟滞/降档延时，名额仍受硬上限约束；Full、阴影、RT 各分预算，避免一项升级全套放行。
// 战斗可恢复必要姿态但不额外获阴影/RT，这些预算也不能反向授予服务器攻击资格。
void UfpstrueEnemySignificanceCoordinator::AllocatePresentationBudgets(int32& FullBudgetDowngradeCount,
	int32& ShadowBudgetRejectedCount, int32& RayTracingBudgetRejectedCount)
{
	TArray<FEnemyRenderCandidate>& Candidates = CandidateBuffer;
	const FFPEnemyRenderSignificancePolicy& Policy = ActiveRenderPolicy;
	FullBudgetDowngradeCount = ShadowBudgetRejectedCount = RayTracingBudgetRejectedCount = 0;
	for (FEnemyRenderCandidate& Candidate : Candidates)
	{
		// 先把所有自然 Full 候选降为 Reduced，再只恢复优先级最高的 K 个。
		Candidate.AssignedTier = Policy.bEnableRenderTiering && Candidate.NaturalTier == EFPEnemyRenderSignificanceTier::Full
			? EFPEnemyRenderSignificanceTier::Reduced : Candidate.NaturalTier;
		// 关闭某项预算时默认放行；名额历史仍清除，重启时不得继承停用前的优势。
		Candidate.bShouldCastShadow = !Policy.bEnableShadowBudget;
		Candidate.bShouldBeVisibleInRayTracing = !Policy.bEnableRayTracingBudget;
	}

	// 三类预算复用同一个小型堆。默认名额不超过内联容量，避免为 Top-K 额外申请堆内存；
	// 配置超过内联容量时 TArray 仍可正常扩展。比较阶段只读取预计算的纯数值 PriorityKey。
	// ★【重要边界】[预算保护] 先收齐候选再用纯数值 Top-K 分配名额，不在 Actor 采样过程中按注册先后抢预算。
	TArray<FFPEnemyRenderTopKEntry, TInlineAllocator<32>> TopKHeap;
	// 每种预算单独过滤资格并复用同一堆；返回候选总数，便于统计因名额不足被拒绝的数量。
	const auto SelectTopK = [&Candidates, &TopKHeap, &Policy](int32 RequestedCount,
		TSet<TWeakObjectPtr<AfpstrueEnemyCharacter>>& PreviousHolders, const auto& IsEligible)
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
			const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Candidate.Enemy);
			const FFPEnemyRenderPriorityKey BudgetKey = FPEnemyRenderBudgetPriorityKey(Candidate.PriorityKey,
				PreviousHolders.Contains(EnemyKey), Policy.BudgetRetentionMargin);
			FPEnemyRenderTopKInsert(TopKHeap, SafeCount, CandidateIndex, BudgetKey);
		}
		// 当轮不再有资格、已死亡/销毁或不再被发现的对象自然失去名额，不额外保留计时锁。
		PreviousHolders.Reset();
		for (const FFPEnemyRenderTopKEntry& Entry : TopKHeap)
			PreviousHolders.Add(TWeakObjectPtr<AfpstrueEnemyCharacter>(Candidates[Entry.CandidateIndex].Enemy));
		return EligibleCount;
	};

	// Render Tier 预算：先尊重自然档位，再限制 Full 档总量。
	// 禁用分档时 ResolveNaturalRenderSignificanceTier 已返回 Full，无须再次覆盖全体。
	if (Policy.bEnableRenderTiering)
	{
		const int32 EligibleFullCount = SelectTopK(Policy.MaxFullRenderEnemies, FullBudgetHolders, [](const FEnemyRenderCandidate& Candidate)
												   { return Candidate.NaturalTier == EFPEnemyRenderSignificanceTier::Full; });
		for (const FFPEnemyRenderTopKEntry& Entry : TopKHeap)
		{
			Candidates[Entry.CandidateIndex].AssignedTier = EFPEnemyRenderSignificanceTier::Full;
		}
		FullBudgetDowngradeCount = EligibleFullCount - TopKHeap.Num();
	}
	else FullBudgetHolders.Reset();

	// 光追预算：只让 Full 且在距离内的高排序敌人参与动态 BLAS。
	if (Policy.bEnableRayTracingBudget)
	{
		const int32 EligibleRayTracingCount = SelectTopK(Policy.MaxRayTracingEnemies, RayTracingBudgetHolders,
														 [&Policy](const FEnemyRenderCandidate& Candidate)
														 {
															 return Candidate.bRayTracingEligible && Candidate.AssignedTier == EFPEnemyRenderSignificanceTier::Full &&
																	Candidate.Sample.Distance <= Policy.RayTracingMaxDistance;
														 });
		for (const FFPEnemyRenderTopKEntry& Entry : TopKHeap)
		{
			Candidates[Entry.CandidateIndex].bShouldBeVisibleInRayTracing = true;
		}
		RayTracingBudgetRejectedCount = EligibleRayTracingCount - TopKHeap.Num();
	}
	else RayTracingBudgetHolders.Reset();

	// 阴影预算：扩展视锥、距离和 Render Tier 共同决定候选资格。
	if (Policy.bEnableShadowBudget)
	{
		const int32 EligibleShadowCount = SelectTopK(Policy.MaxShadowCastingEnemies, ShadowBudgetHolders,
													 [&Policy](const FEnemyRenderCandidate& Candidate)
													 {
														 return Candidate.bShadowEligible && Candidate.Sample.bInExpandedFrustum &&
																Candidate.Sample.Distance <= Policy.ShadowMaxDistance &&
																Candidate.AssignedTier != EFPEnemyRenderSignificanceTier::Background;
													 });
		for (const FFPEnemyRenderTopKEntry& Entry : TopKHeap)
		{
			Candidates[Entry.CandidateIndex].bShouldCastShadow = true;
		}
		ShadowBudgetRejectedCount = EligibleShadowCount - TopKHeap.Num();
	}
	else ShadowBudgetHolders.Reset();
}

// 应用本轮已确定的三类预算，并读回组件标志和档位写入 CSV；统计结果不参与本轮决策。
void UfpstrueEnemySignificanceCoordinator::ApplyAndRecordCandidates(int32 FullBudgetDowngradeCount,
																	int32 ShadowBudgetRejectedCount, int32 RayTracingBudgetRejectedCount)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemySignificance_ApplyPresentationBudget);
	// 统一应用结果并在同一位置记录消融所需的 CSV 指标。
	// 注意：统一采样不等于所有消费者必须无条件重写；组件内部仍应通过状态比较避免重复修改渲染状态。
	const FFPEnemyRenderSignificancePolicy& Policy = ActiveRenderPolicy;
	const TArray<FEnemyRenderCandidate>& Candidates = CandidateBuffer;
	// 档位按 Full/Reduced/Background，LOD 按 0/1/2+ 分桶；保持全部历史 CSV 列及其计量口径。
	static_assert(static_cast<int32>(EFPEnemySignificanceTier::Background) == 2);
	static_assert(static_cast<int32>(EFPEnemyRenderSignificanceTier::Background) == 2);
	int32 GameplayCounts[3] = {};
	int32 RenderCounts[3] = {};
	int32 MinLODCounts[3] = {};
	int32 PredictedLODCounts[3] = {};
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
	// 逐个提交策略后立即读回：区分敌人数、受管 Mesh 数和 CPU 预测 LOD，避免混用口径。
	for (const FEnemyRenderCandidate& Candidate : Candidates)
	{
		Candidate.Enemy->ApplyRenderSignificanceTier(Candidate.AssignedTier, Candidate.bShouldCastShadow,
													 Candidate.bShouldBeVisibleInRayTracing, Candidate.bGameplayAnimationProtection,
													 Policy);

		++GameplayCounts[FMath::Min(static_cast<int32>(Candidate.Enemy->GetGameplaySignificanceTier()), 2)];
		++RenderCounts[FMath::Min(static_cast<int32>(Candidate.Enemy->GetRenderSignificanceTier()), 2)];
		++MinLODCounts[FMath::Clamp(Candidate.Enemy->GetAppliedMinimumLOD(), 0, 2)];
		if (const USkeletalMeshComponent* CharacterMesh = Candidate.Enemy->GetMesh())
		{
			// CPU 预测选择与 MinLOD 策略约束分开；预测值也不冒充 GPU 每个 Pass 的实际 LOD。
			const int32 PredictedLOD = CharacterMesh->GetPredictedLODLevel();
			if (PredictedLOD >= 0)
			{
				++PredictedLODCounts[FMath::Min(PredictedLOD, 2)];
			}
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

	// 以本轮全部存活候选为分母记录均值；没有候选时仍写零，避免沿用上一轮数据。
	const float CandidateCount = static_cast<float>(Candidates.Num());
	const float InverseCandidateCount = CandidateCount > 0.0f ? 1.0f / CandidateCount : 0.0f;
	CSV_CUSTOM_STAT(fpstrueSignificance, AliveEnemies, Candidates.Num(), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, GameplayFull, GameplayCounts[0], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, GameplayReduced, GameplayCounts[1], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, GameplayBackground, GameplayCounts[2], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, RenderFull, RenderCounts[0], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, RenderReduced, RenderCounts[1], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, RenderBackground, RenderCounts[2], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, LOD0, MinLODCounts[0], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, LOD1, MinLODCounts[1], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, LOD2Plus, MinLODCounts[2], ECsvCustomStatOp::Set);
	// 旧 LOD* 列保留脚本兼容，含义一直是最低 LOD 约束；新增明确命名避免与实际选择混淆。
	CSV_CUSTOM_STAT(fpstrueSignificance, MinLOD0, MinLODCounts[0], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, MinLOD1, MinLODCounts[1], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, MinLOD2Plus, MinLODCounts[2], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, PredictedLOD0, PredictedLODCounts[0], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, PredictedLOD1, PredictedLODCounts[1], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, PredictedLOD2Plus, PredictedLODCounts[2], ECsvCustomStatOp::Set);
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
					AnimationSharingCoordinator.IsValid()
						? AnimationSharingCoordinator->GetRegisteredEnemyCount()
						: 0,
					ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, MeanScore, ScoreSum * InverseCandidateCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, MeanFrustumFactor, FrustumFactorSum * InverseCandidateCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, MeanScreenFactor, ScreenCoverageFactorSum * InverseCandidateCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, MeanRecentFactor, RecentFrustumFactorSum * InverseCandidateCount, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(fpstrueSignificance, MeanDistanceFactor, DistanceFactorSum * InverseCandidateCount, ECsvCustomStatOp::Set);
}

// ==================== 策略校验 ====================

// 在进入更新热路径前统一修正配置，保持阈值、距离和最低 LOD 的顺序关系。
void UfpstrueEnemySignificanceCoordinator::SanitizePolicy(FFPEnemyRenderSignificancePolicy& Policy)
{
	// 配置可能来自 CDO、蓝图或 Benchmark 命令行；进入热路径前集中修正，Update 中不再重复做防御性分支。
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

	// 入档与出档阈值分开，保留滞回区间，减少临界视角下反复升降档。
	Policy.FullEnterThreshold = FMath::Clamp(Policy.FullEnterThreshold, 0.0f, 1.0f);
	Policy.FullExitThreshold = FMath::Clamp(Policy.FullExitThreshold, 0.0f, Policy.FullEnterThreshold);
	Policy.ReducedEnterThreshold = FMath::Clamp(Policy.ReducedEnterThreshold, 0.0f, Policy.FullExitThreshold);
	Policy.ReducedExitThreshold = FMath::Clamp(Policy.ReducedExitThreshold, 0.0f, Policy.ReducedEnterThreshold);
	Policy.DemotionDelaySeconds = FMath::Max(Policy.DemotionDelaySeconds, 0.0f);
	Policy.MinimumTierHoldSeconds = FMath::Max(Policy.MinimumTierHoldSeconds, 0.0f);

	// 名额允许为零；低档位的最低 LOD 不得反而比高档位更精细。
	Policy.BudgetRetentionMargin = FMath::IsFinite(Policy.BudgetRetentionMargin)
		? FMath::Clamp(Policy.BudgetRetentionMargin, 0.0f, 1.0f) : 0.0f;
	Policy.MaxFullRenderEnemies = FMath::Max(Policy.MaxFullRenderEnemies, 0);
	Policy.MaxShadowCastingEnemies = FMath::Max(Policy.MaxShadowCastingEnemies, 0);
	Policy.ShadowMaxDistance = FMath::Max(Policy.ShadowMaxDistance, 0.0f);
	Policy.MaxRayTracingEnemies = FMath::Max(Policy.MaxRayTracingEnemies, 0);
	Policy.RayTracingMaxDistance = FMath::Max(Policy.RayTracingMaxDistance, 0.0f);
	Policy.FullMinLOD = FMath::Max(Policy.FullMinLOD, 0);
	Policy.ReducedMinLOD = FMath::Max(Policy.ReducedMinLOD, Policy.FullMinLOD);
	Policy.BackgroundMinLOD = FMath::Max(Policy.BackgroundMinLOD, Policy.ReducedMinLOD);
}

// Copyright Epic Games, Inc. All Rights Reserved.


namespace
{
constexpr float SharedMovingSpeedThreshold = 10.0f;
}

/*
 * Animation Sharing 插件的项目适配层。
 * 它把现有 AI 的 Idle/Chase 状态映射为共享动画状态，并只让满足渲染分级且不处于战斗保护期的普通敌人
 * 成为 Follower；攻击 Montage、Notify 和死亡表现仍使用敌人自己的骨骼动画实例。
 *
 * 共享的只是 Idle/Moving 姿态计算，不共享 Actor Transform、AIController、CharacterMovement、生命值或攻击状态。
 * Coordinator 统一拥有 World 级 Manager、运行时 Setup 和 Actor Handle；EnemyCharacter 只通过加入/退出接口参与，
 * 避免每个敌人各自创建一套 Sharing 配置。
 */

// ==================== 状态适配：读取实际移动速度 ====================

// 由插件采样敌人的移动表现；意图为追击但速度为零时输出待机，不参与高层行为选择。
void UfpstrueEnemyAnimationSharingStateProcessor::ProcessActorState_Implementation(int32& OutState, AActor* InActor, uint8 CurrentState,
																				   uint8 OnDemandState, bool& bShouldProcess)
{
	// 已到槽位的敌人输出待机；客户端只有移动复制，不通过不存在的 AIController 推断移动表现。
	bShouldProcess = IsValid(InActor);
	if (!bShouldProcess)
	{
		OutState = static_cast<int32>(EFPEnemyAIState::Idle);
		return;
	}

	const bool bMoving = InActor->GetVelocity().SizeSquared2D() > FMath::Square(SharedMovingSpeedThreshold);

	// 不把“正在追击”的意图当成真实位移，停在槽位或受阻时不应原地播放跑步。
	OutState = static_cast<int32>(bMoving ? EFPEnemyAIState::Chase : EFPEnemyAIState::Idle);
}

// 提供共享状态对应的原生枚举，确保状态整数与插件 Setup 使用同一编号。
UEnum* UfpstrueEnemyAnimationSharingStateProcessor::GetAnimationStateEnum_Implementation()
{
	return StaticEnum<EFPEnemyAIState>();
}

// ==================== 初始化与生命周期 ====================

// Coordinator 不逐帧运行；默认动画使用软引用，只有真正启用 Sharing 时才同步加载。
// 项目级素材路径来自 DefaultGame.ini；组件蓝图保存的同名属性仍可覆盖这些构造默认值。
UfpstrueEnemyAnimationSharingCoordinator::UfpstrueEnemyAnimationSharingCoordinator()
{
	PrimaryComponentTick.bCanEverTick = false;

	// 这里只读配置，不加载资源、不写配置；路径缺失时由原有 Setup 校验安全回退到独立 AnimBP。
	if (GConfig != nullptr)
	{
		FString IdlePath;
		FString MovingPath;
		GConfig->GetString(TEXT("fpstrue.EnemyAnimationSharing"), TEXT("IdleAnimation"), IdlePath, GGameIni);
		GConfig->GetString(TEXT("fpstrue.EnemyAnimationSharing"), TEXT("MovingAnimation"), MovingPath, GGameIni);
		IdleAnimation = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(IdlePath));
		MovingAnimation = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(MovingPath));
	}
}

// 启用并校验共享系统；只创建或复用自己拥有、骨架兼容的 World 共享池。
void UfpstrueEnemyAnimationSharingCoordinator::Start(TSubclassOf<AfpstrueEnemyCharacter> InEnemyClass)
{
	// 一个 World 只允许本项目创建一个 Manager；任一资产或骨架校验失败都保持原有 AnimBP，不留下半初始化状态。
	// 联网权威端的独立骨骼用于命中检测；专服无表现预算，监听主机也不能用共享姿态替换权威命中形状。
	if (bRunning || GetNetMode() == NM_DedicatedServer || GetNetMode() == NM_ListenServer)
	{
		return;
	}

	const FFPRuntimeOptions& BenchmarkConfig = FFPRuntimeOptions::Get();
	if (!bEnableAnimationSharing || BenchmarkConfig.bDisableEnemyAnimationSharing || BenchmarkConfig.bDisableAnimationOptimizations)
	{
		UE_LOG(LogTemp, Display, TEXT("Enemy Animation Sharing disabled: feature=%d ablation=%d animationOptimizationsOff=%d"),
			   bEnableAnimationSharing ? 1 : 0, BenchmarkConfig.bDisableEnemyAnimationSharing ? 1 : 0,
			   BenchmarkConfig.bDisableAnimationOptimizations ? 1 : 0);
		return;
	}

	if (!UAnimationSharingManager::AnimationSharingEnabled())
	{
		UE_LOG(LogTemp, Warning, TEXT("Enemy Animation Sharing skipped: a.Sharing.Enabled is false."));
		return;
	}

	// ★【重要边界】[资源所有权] 一个 World 的共享 Manager 只复用本组件创建且骨架兼容的实例，不能覆盖其他系统的 Setup。
	UAnimationSharingManager* WorldManager = UAnimationSharingManager::GetManagerForWorld(GetWorld());
	if (WorldManager != nullptr && WorldManager == SharingManager && RuntimeSetup != nullptr)
	{
		// Stop 只注销 Follower，不销毁 World 共享池；只复用自己创建且骨架一致的实例。
		const AfpstrueEnemyCharacter* Defaults = InEnemyClass.GetDefaultObject();
		const USkeletalMesh* Mesh = Defaults != nullptr && Defaults->GetMesh() != nullptr
			? Defaults->GetMesh()->GetSkeletalMeshAsset() : nullptr;
		if (Mesh != nullptr && Mesh->GetSkeleton() == SharingSkeleton)
		{
			bRunning = true;
			return;
		}
	}
	if (WorldManager != nullptr)
	{
		// 一个 World 只能有一个 Manager；不覆盖关卡或其他系统已经创建的 Setup。
		UE_LOG(LogTemp, Warning, TEXT("Enemy Animation Sharing skipped: this World already owns an Animation Sharing manager."));
		return;
	}

	if (!BuildRuntimeSetup(InEnemyClass) || !UAnimationSharingManager::CreateAnimationSharingManager(this, RuntimeSetup))
	{
		UE_LOG(LogTemp, Warning, TEXT("Enemy Animation Sharing setup could not be created."));
		RuntimeSetup = nullptr;
		SharingSkeleton = nullptr;
		return;
	}

	SharingManager = UAnimationSharingManager::GetManagerForWorld(GetWorld());
	if (SharingManager == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("Enemy Animation Sharing manager was not available after creation."));
		return;
	}
	bRunning = true;
	UE_LOG(LogTemp, Display, TEXT("Enemy Animation Sharing: running=%d skeleton=%s idleLeaders=%d movingLeaders=%d tickThreshold=%.2f"),
		   bRunning ? 1 : 0, *GetNameSafe(SharingSkeleton), FMath::Max(IdleRandomizedInstances, 1),
		   FMath::Max(MovingRandomizedInstances, 1), FMath::Clamp(LeaderTickSignificanceThreshold, 0.0f, 1.0f));
}

// 关闭接纳入口并注销全部已登记、待登记 Follower；允许注销过程同步更新其他句柄。
void UfpstrueEnemyAnimationSharingCoordinator::Stop()
{
	// 先停止接纳新对象，再逐个注销；插件在 RemoveAtSwap 期间仍会同步更新其他对象的句柄。
	bRunning = false;
	TArray<TWeakObjectPtr<AfpstrueEnemyCharacter>> RegisteredEnemies;
	RegisteredActorHandles.GenerateKeyArray(RegisteredEnemies);
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : PendingActorRegistrations)
	{
		RegisteredEnemies.AddUnique(EnemyPtr);
	}
	for (const TWeakObjectPtr<AfpstrueEnemyCharacter>& EnemyPtr : RegisteredEnemies)
	{
		SuspendEnemy(EnemyPtr.Get());
	}

	RegisteredActorHandles.Reset();
	PendingActorRegistrations.Reset();
}

// 退出时先注销敌人，再释放运行时配置引用；停止对局不等于立即销毁 World 共享池。
void UfpstrueEnemyAnimationSharingCoordinator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 世界退出时释放 Manager、运行时 Setup 和 Skeleton 的强引用，允许 UObject GC 回收临时配置。
	Stop();
	SharingManager = nullptr;
	RuntimeSetup = nullptr;
	SharingSkeleton = nullptr;
	Super::EndPlay(EndPlayReason);
}

// ==================== 运行时 Setup 构建 ====================

// 根据敌人默认骨架和配置动画创建待机、移动共享组；缺失或骨架不匹配时回退独立 AnimBP。
bool UfpstrueEnemyAnimationSharingCoordinator::BuildRuntimeSetup(TSubclassOf<AfpstrueEnemyCharacter> InEnemyClass)
{
	// Setup 在运行时从敌人 CDO 的 SkeletalMesh/Skeleton 构建，确保共享动画和实际敌人使用同一骨架。
	const AfpstrueEnemyCharacter* EnemyDefaults = InEnemyClass ? InEnemyClass->GetDefaultObject<AfpstrueEnemyCharacter>() : nullptr;
	const USkeletalMeshComponent* DefaultMeshComponent = EnemyDefaults != nullptr ? EnemyDefaults->GetMesh() : nullptr;
	USkeletalMesh* SkeletalMesh = DefaultMeshComponent != nullptr ? DefaultMeshComponent->GetSkeletalMeshAsset() : nullptr;
	SharingSkeleton = SkeletalMesh != nullptr ? SkeletalMesh->GetSkeleton() : nullptr;

	UAnimSequence* IdleSequence = IdleAnimation.LoadSynchronous();
	UAnimSequence* MovingSequence = MovingAnimation.LoadSynchronous();
	if (SkeletalMesh == nullptr || SharingSkeleton == nullptr || IdleSequence == nullptr || MovingSequence == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("Enemy Animation Sharing assets invalid: mesh=%s skeleton=%s idle=%s moving=%s"),
			   *GetNameSafe(SkeletalMesh), *GetNameSafe(SharingSkeleton), *GetNameSafe(IdleSequence), *GetNameSafe(MovingSequence));
		return false;
	}

	if (IdleSequence->GetSkeleton() != SharingSkeleton || MovingSequence->GetSkeleton() != SharingSkeleton)
	{
		UE_LOG(LogTemp, Warning, TEXT("Enemy Animation Sharing skeleton mismatch: enemy=%s idle=%s moving=%s"),
			   *GetNameSafe(SharingSkeleton), *GetNameSafe(IdleSequence->GetSkeleton()), *GetNameSafe(MovingSequence->GetSkeleton()));
		return false;
	}

	// 资产全部通过校验后才创建临时 Setup；随机实例数是共享姿态池配置，不是敌人数上限。
	RuntimeSetup = NewObject<UAnimationSharingSetup>(this, TEXT("EnemyAnimationSharingRuntimeSetup"));
	RuntimeSetup->ScalabilitySettings.UseBlendTransitions.Default = false;
	RuntimeSetup->ScalabilitySettings.MaximumNumberConcurrentBlends.Default = 1;
	RuntimeSetup->ScalabilitySettings.TickSignificanceValue.Default = FMath::Clamp(LeaderTickSignificanceThreshold, 0.0f, 1.0f);

	FPerSkeletonAnimationSharingSetup& SkeletonSetup = RuntimeSetup->SkeletonSetups.AddDefaulted_GetRef();
	SkeletonSetup.Skeleton = SharingSkeleton;
	SkeletonSetup.SkeletalMesh = SkeletalMesh;
	SkeletonSetup.StateProcessorClass = UfpstrueEnemyAnimationSharingStateProcessor::StaticClass();

	FAnimationStateEntry& IdleState = SkeletonSetup.AnimationStates.AddDefaulted_GetRef();
	IdleState.State = static_cast<uint8>(EFPEnemyAIState::Idle);
	FAnimationSetup& IdleSetup = IdleState.AnimationSetups.AddDefaulted_GetRef();
	IdleSetup.AnimSequence = IdleSequence;
	IdleSetup.NumRandomizedInstances.Default = FMath::Max(IdleRandomizedInstances, 1);
	IdleSetup.Enabled.Default = true;

	FAnimationStateEntry& MovingState = SkeletonSetup.AnimationStates.AddDefaulted_GetRef();
	MovingState.State = static_cast<uint8>(EFPEnemyAIState::Chase);
	FAnimationSetup& MovingSetup = MovingState.AnimationSetups.AddDefaulted_GetRef();
	MovingSetup.AnimSequence = MovingSequence;
	MovingSetup.NumRandomizedInstances.Default = FMath::Max(MovingRandomizedInstances, 1);
	MovingSetup.Enabled.Default = true;

	return true;
}

// ==================== Follower 注册与退出 ====================

// 资格变化时才加入或退出共享池；已登记敌人仅更新渲染显著性，不重复注册。
void UfpstrueEnemyAnimationSharingCoordinator::RefreshEnemyRegistration(AfpstrueEnemyCharacter* Enemy)
{
	if (!bRunning || SharingManager == nullptr || !IsValid(Enemy))
	{
		return;
	}

	USkeletalMeshComponent* CharacterMesh = Enemy->GetMesh();
	USkeletalMesh* SkeletalMesh = CharacterMesh != nullptr ? CharacterMesh->GetSkeletalMeshAsset() : nullptr;
	if (!Enemy->CanUseAnimationSharing() || SkeletalMesh == nullptr || SkeletalMesh->GetSkeleton() != SharingSkeleton)
	{
		SuspendEnemy(Enemy);
		return;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	if (!RegisteredActorHandles.Contains(EnemyKey) && !PendingActorRegistrations.Contains(EnemyKey))
	{
		PendingActorRegistrations.Add(EnemyKey);
		// ★【重要边界】[回调重入] 注册会同步交付句柄；先登记 pending，等插件调用返回后才能按新资格撤销，不能在句柄回调里反注册。
		SharingManager->RegisterActorWithSkeleton(
			Enemy, SharingSkeleton,
			FUpdateActorHandle::CreateUObject(this, &UfpstrueEnemyAnimationSharingCoordinator::HandleActorHandleUpdated, EnemyKey));
		// UE 5.5 在本次调用内同步交付句柄。失败时可能不回调，必须释放 pending 以允许下一轮重试。
		PendingActorRegistrations.Remove(EnemyKey);
		// 只有插件调用返回后才能撤销注册，不能在其正在修改内部数组的句柄回调里反注册。
		if (!bRunning || !IsValid(Enemy) || !Enemy->CanUseAnimationSharing())
		{
			SuspendEnemy(Enemy);
			return;
		}
	}

	if (const uint32* Handle = RegisteredActorHandles.Find(EnemyKey))
	{
		// 插件内部的 Leader Tick / Blend 预算只读取渲染重要性，不读取 AI 距离分层。
		SharingManager->UpdateSignificanceForActorHandle(*Handle, FMath::Clamp(Enemy->GetRenderSignificanceScore(), 0.0f, 1.0f));
	}
}

// 注销本敌人的 Follower，并把骨骼 LOD 控制权还给角色；重复调用不重复注销。
void UfpstrueEnemyAnimationSharingCoordinator::SuspendEnemy(AfpstrueEnemyCharacter* Enemy)
{
	if (SharingManager == nullptr || Enemy == nullptr)
	{
		return;
	}

	const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
	const bool bWasPending = PendingActorRegistrations.Remove(EnemyKey) > 0;
	const bool bWasRegistered = RegisteredActorHandles.Remove(EnemyKey) > 0;
	if (bWasPending || bWasRegistered)
	{
		// ★【重要边界】[资源所有权] 先移除本地登记再让插件注销；其余敌人的句柄可能同步更新，弱引用和 handle 都不是 Actor 所有权。
		SharingManager->UnregisterActor(Enemy);

		// Animation Sharing 注册或待注册时都可能接管此标志；退出后必须还给项目的骨骼 LOD 分级。
		TInlineComponentArray<USkeletalMeshComponent*, 4> OwnedMeshComponents;
		Enemy->GetComponents(OwnedMeshComponents);
		for (USkeletalMeshComponent* MeshComponent : OwnedMeshComponents)
		{
			if (MeshComponent != nullptr)
			{
				MeshComponent->bIgnoreLeaderPoseComponentLOD = false;
			}
		}
	}
}

// ==================== Handle 回调 ====================

// 接收插件注册或紧缩数组后的句柄变化；回调只更新本地登记，不能重入插件增删操作。
void UfpstrueEnemyAnimationSharingCoordinator::HandleActorHandleUpdated(int32 NewHandle, TWeakObjectPtr<AfpstrueEnemyCharacter> Enemy)
{
	// UE 5.5 注册/RemoveAtSwap 会同步回调，Stop 期间也要更新尚未注销者的句柄；实际资格与增删留在外层。
	AfpstrueEnemyCharacter* EnemyCharacter = Enemy.Get();
	if (EnemyCharacter == nullptr)
	{
		PendingActorRegistrations.Remove(Enemy);
		RegisteredActorHandles.Remove(Enemy);
		return;
	}

	const bool bKnownRegistration = PendingActorRegistrations.Remove(Enemy) > 0 || RegisteredActorHandles.Contains(Enemy);
	if (NewHandle == INDEX_NONE)
	{
		RegisteredActorHandles.Remove(Enemy);
		return;
	}

	if (!bKnownRegistration)
	{
		// 已经退出的登记不能由未知回调重新建立。
		return;
	}

	RegisteredActorHandles.FindOrAdd(Enemy) = static_cast<uint32>(NewHandle);
}
