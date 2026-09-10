// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemyCharacter.h"
#include "fpstrueBenchmarkConfig.h"
#include "fpstrueCharacter.h"
#include "fpstrueCollisionChannels.h"
#include "fpstrueEnemyAIController.h"
#include "fpstrueEnemyAnimationSharingCoordinator.h"
#include "fpstrueEnemyCombatComponent.h"
#include "fpstrueHealthComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Math/RotationMatrix.h"
#include "SignificanceManager.h"

/*
 * 敌人 Pawn 与各子系统之间的桥接层。
 * HealthComponent/CombatComponent 分别拥有生命和攻击事务，AIController 拥有目标与状态机；本类负责组件装配、
 * 受击死亡表现，以及把 Gameplay/Render Significance 结果转换成移动、骨骼、阴影和 RT 组件设置。
 *
 * 本类刻意不保存第二份 AI、血量或攻击状态：对外查询都转发到真正所有者，跨模块操作通过窄接口完成。
 * Gameplay Significance 只影响决策/移动节奏；Render Significance 只影响骨骼、动画、阴影、RT 和动画共享。
 * 攻击与近期战斗通过 GameplayAnimationProtection 临时恢复完整动画，防止性能策略破坏 Notify 和 Socket 判定。
 */

// ==================== 组件初始化与生命周期 ====================

// 构造默认 Mesh/Movement 行为以及可复用生命、战斗组件；高层决策由自动生成的 AIController 承担。
AfpstrueEnemyCharacter::AfpstrueEnemyCharacter()
{
	PrimaryActorTick.bCanEverTick = false;
	AIControllerClass = AfpstrueEnemyAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;

	GetCapsuleComponent()->InitCapsuleSize(42.0f, 96.0f);
	// 每个敌人的 CharacterMovement 使用 RVO 做局部避让；AIController 仍负责路径请求和高层状态。
	// OnPossess 会再次显式开启，避免已保存蓝图中的旧默认值让同一构建出现混合模式。
	GetCharacterMovement()->bUseRVOAvoidance = true;

	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		CharacterMesh->bEnableUpdateRateOptimizations = true;
		CharacterMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered;
	}

	HealthComponent = CreateDefaultSubobject<UfpstrueHealthComponent>(TEXT("HealthComponent"));
	CombatComponent = CreateDefaultSubobject<UfpstrueEnemyCombatComponent>(TEXT("CombatComponent"));
}

void AfpstrueEnemyCharacter::BeginPlay()
{
	// 初始化顺序：读取 Benchmark 覆盖 -> 配置 Mesh/Movement -> 绑定 Health -> 注册 Gameplay Significance。
	Super::BeginPlay();

	const FFPBenchmarkConfig& BenchmarkConfig = FFPBenchmarkConfig::Get();
	if (BenchmarkConfig.bDisableMovementTiering)
	{
		bEnableMovementUpdateTiering = false;
	}
	bDisableEnemyRayTracingForBenchmark = BenchmarkConfig.bDisableEnemyRayTracing;
	bDisableEnemyShadowsForBenchmark = BenchmarkConfig.bDisableEnemyShadows;
	bDisableAnimationOptimizationsForBenchmark = BenchmarkConfig.bDisableAnimationOptimizations;

	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		if (bDisableEnemyRayTracingForBenchmark)
		{
			CharacterMesh->SetVisibleInRayTracing(false);
		}
		if (bDisableEnemyShadowsForBenchmark)
		{
			CharacterMesh->SetCastShadow(false);
		}
		if (bDisableAnimationOptimizationsForBenchmark)
		{
			CharacterMesh->bEnableUpdateRateOptimizations = false;
			CharacterMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		}
		CharacterMesh->SetSimulatePhysics(false);
		CharacterMesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		CharacterMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		// 射击绕过移动胶囊并命中 Physics Asset，HitResult 才能携带用于部位伤害的 BoneName。
		CharacterMesh->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Block);
	}
	GetCapsuleComponent()->SetCollisionResponseToChannel(FpstrueCollisionChannels::WeaponTrace, ECR_Ignore);

	if (HealthComponent != nullptr)
	{
		HealthComponent->OnDeath.AddUniqueDynamic(this, &AfpstrueEnemyCharacter::HandleDeath);
		HealthComponent->OnDamageReceived.AddUniqueDynamic(this, &AfpstrueEnemyCharacter::HandleDamageReceived);
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->MaxWalkSpeed = MoveSpeed;
		Movement->bOrientRotationToMovement = false;
		Movement->bUseControllerDesiredRotation = false;
		Movement->RotationRate = FRotator(0.0f, 540.0f, 0.0f);
	}
	bUseControllerRotationYaw = false;

	RegisterWithSignificanceManager();
}

void AfpstrueEnemyCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 退出时先断开外部管理器和委托；组件自身的 Timer/攻击事务由各组件 EndPlay 继续清理。
	SuspendAnimationSharing();
	UnregisterFromSignificanceManager();
	if (HealthComponent != nullptr)
	{
		HealthComponent->OnDeath.RemoveDynamic(this, &AfpstrueEnemyCharacter::HandleDeath);
		HealthComponent->OnDamageReceived.RemoveDynamic(this, &AfpstrueEnemyCharacter::HandleDamageReceived);
	}

	Super::EndPlay(EndPlayReason);
}

float AfpstrueEnemyCharacter::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator,
										 AActor* DamageCauser)
{
	// AActor 伤害入口只补充命中方向/骨骼信息；实际血量写入仍由通用 HealthComponent 通过 OnTakeAnyDamage 完成。
	if (DamageEvent.IsOfType(FPointDamageEvent::ClassID))
	{
		const FPointDamageEvent* PointDamageEvent = static_cast<const FPointDamageEvent*>(&DamageEvent);
		LastDamageDirection = PointDamageEvent->ShotDirection.GetSafeNormal();
		LastDamageLocation = PointDamageEvent->HitInfo.ImpactPoint;
		LastDamageBoneName = PointDamageEvent->HitInfo.BoneName;
	}
	else if (DamageCauser != nullptr)
	{
		LastDamageDirection = (GetActorLocation() - DamageCauser->GetActorLocation()).GetSafeNormal();
		LastDamageLocation = GetActorLocation();
		LastDamageBoneName = NAME_None;
	}

	if (LastDamageDirection.IsNearlyZero())
	{
		LastDamageDirection = GetActorForwardVector() * -1.0f;
	}

	if (LastDamageLocation.IsNearlyZero())
	{
		LastDamageLocation = GetActorLocation();
	}

	return Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
}

// ==================== 状态查询与玩法接口 ====================

bool AfpstrueEnemyCharacter::IsDead() const
{
	// 死亡状态始终转发到 HealthComponent，EnemyCharacter 不保存第二份生命事实。
	return HealthComponent != nullptr && HealthComponent->IsDead();
}

bool AfpstrueEnemyCharacter::IsAttacking() const
{
	// 攻击事务状态属于 CombatComponent，角色只提供给 AI 和显著性系统读取。
	return CombatComponent != nullptr && CombatComponent->IsAttacking();
}

bool AfpstrueEnemyCharacter::RequiresGameplayAnimationProtection(float CurrentTime, float GraceSeconds) const
{
	// 攻击中、已贴近目标或刚发生交互时禁止动画降级，保护 Montage、Notify 和刀刃 Socket 更新。
	const bool bRecentlyInteracted = CurrentTime - LastCombatRelevantTime <= FMath::Max(GraceSeconds, 0.0f);
	return IsAttacking() || IsTargetInAttackRange() || bRecentlyInteracted;
}

float AfpstrueEnemyCharacter::GetAttackRange() const
{
	// 返回设计配置的基础攻击半径，供追击接受距离等非碰撞规则参考。
	return CombatComponent != nullptr ? CombatComponent->GetConfiguredAttackRange() : 0.0f;
}

float AfpstrueEnemyCharacter::GetEffectiveAttackRange() const
{
	// 返回 CombatComponent 结合双方胶囊体修正后的实际可达攻击距离。
	return CombatComponent != nullptr ? CombatComponent->GetEffectiveAttackRange() : 0.0f;
}

// ==================== Gameplay Significance：目标距离与交互状态 ====================

void AfpstrueEnemyCharacter::RegisterWithSignificanceManager()
{
	// 每个敌人只注册一个轻量距离函数；集中 Coordinator 调用 Manager::Update，敌人本身没有 Significance Tick。
	if (bRegisteredWithSignificanceManager || GetWorld() == nullptr)
	{
		return;
	}

	USignificanceManager* Manager = USignificanceManager::Get(GetWorld());
	if (Manager == nullptr)
	{
		return;
	}

	Manager->RegisterObject(
		this, TEXT("Enemy"),
		[](USignificanceManager::FManagedObjectInfo* ObjectInfo, const FTransform& Viewpoint)
		{
			const AfpstrueEnemyCharacter* Enemy = Cast<AfpstrueEnemyCharacter>(ObjectInfo->GetObject());
			if (!IsValid(Enemy) || Enemy->IsDead())
			{
				return 0.0f;
			}

			const float Distance = FVector::Dist2D(Enemy->GetActorLocation(), Viewpoint.GetLocation());
			return 1.0f / (1.0f + Distance);
		},
		USignificanceManager::EPostSignificanceType::Sequential,
		[](USignificanceManager::FManagedObjectInfo* ObjectInfo, float, float NewSignificance, bool bUnregister)
		{
			AfpstrueEnemyCharacter* Enemy = Cast<AfpstrueEnemyCharacter>(ObjectInfo->GetObject());
			if (!bUnregister && IsValid(Enemy))
			{
				Enemy->ApplySignificance(NewSignificance);
			}
		});
	bRegisteredWithSignificanceManager = true;
}

void AfpstrueEnemyCharacter::UnregisterFromSignificanceManager()
{
	// EndPlay 和死亡共用的幂等注销入口，阻止 Manager 后续再回调本对象。
	if (!bRegisteredWithSignificanceManager)
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		if (USignificanceManager* Manager = USignificanceManager::Get(World))
		{
			Manager->UnregisterObject(this);
		}
	}
	bRegisteredWithSignificanceManager = false;
}

void AfpstrueEnemyCharacter::ApplySignificance(float Significance)
{
	// 距离决定自然档位，但攻击中或已进入攻击范围必须保持 Full，保证近战响应不被后台档降频。
	if (IsDead())
	{
		return;
	}
	const float FullRateThreshold = 1.0f / (1.0f + FullRateMovementDistance);
	const float MidRateThreshold = 1.0f / (1.0f + MidRateMovementDistance);
	const bool bRequiresFullRate = IsAttacking() || IsTargetInAttackRange();

	EFPEnemySignificanceTier NewTier = EFPEnemySignificanceTier::Background;
	if (bRequiresFullRate || Significance >= FullRateThreshold)
	{
		NewTier = EFPEnemySignificanceTier::Full;
	}
	else if (Significance >= MidRateThreshold)
	{
		NewTier = EFPEnemySignificanceTier::Reduced;
	}

	ApplySignificanceTier(NewTier);
}

void AfpstrueEnemyCharacter::ApplySignificanceTier(EFPEnemySignificanceTier NewTier)
{
	// 档位未变化立即返回；变化时一次性下发 Movement Tick 间隔和 AI 决策倍率。
	if (IsDead())
	{
		return;
	}
	if (SignificanceTier == NewTier)
	{
		return;
	}

	SignificanceTier = NewTier;
	ApplyGameplaySignificanceIntervals();

	if (AfpstrueEnemyAIController* EnemyAIController = Cast<AfpstrueEnemyAIController>(GetController()))
	{
		float DecisionMultiplier = 1.0f;
		switch (SignificanceTier)
		{
		case EFPEnemySignificanceTier::Reduced:
			DecisionMultiplier = ReducedDecisionIntervalMultiplier;
			break;

		case EFPEnemySignificanceTier::Background:
			DecisionMultiplier = BackgroundDecisionIntervalMultiplier;
			break;

		case EFPEnemySignificanceTier::Full:
		default:
			break;
		}
		EnemyAIController->SetSignificanceDecisionMultiplier(DecisionMultiplier);
	}
}

void AfpstrueEnemyCharacter::ApplyGameplaySignificanceIntervals()
{
	// Gameplay 档位只改变 CharacterMovement 的更新间隔；攻击状态始终恢复为逐帧移动更新。
	if (IsDead())
	{
		return;
	}

	float MovementTickInterval = 0.0f;
	if (!IsAttacking() && bEnableMovementUpdateTiering)
	{
		switch (SignificanceTier)
		{
		case EFPEnemySignificanceTier::Reduced:
			MovementTickInterval = MidRateMovementTickInterval;
			break;

		case EFPEnemySignificanceTier::Background:
			MovementTickInterval = FarRateMovementTickInterval;
			break;

		case EFPEnemySignificanceTier::Full:
		default:
			break;
		}
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->SetComponentTickInterval(MovementTickInterval);
	}
}

// ==================== Render Significance：可见性、相机距离与渲染预算 ====================

FFPEnemyRenderSignificanceSample AfpstrueEnemyCharacter::EvaluateRenderSignificance(const FFPEnemyRenderViewContext& ViewContext,
																					const FFPEnemyRenderSignificancePolicy& Policy)
{
	// 这里只采样并返回纯渲染数据，不改 AI 状态；预算排序与最终应用由 Coordinator 在全体候选收集后完成。
	FFPEnemyRenderSignificanceSample Sample;
	if (IsDead())
	{
		return Sample;
	}

	const USkeletalMeshComponent* CharacterMesh = GetMesh();
	const FVector BoundsOrigin = CharacterMesh != nullptr ? CharacterMesh->Bounds.Origin : GetActorLocation();
	const float BoundsRadius = CharacterMesh != nullptr ? FMath::Max(CharacterMesh->Bounds.SphereRadius, 1.0f) : 100.0f;

	const FVector ToEnemy = BoundsOrigin - ViewContext.ViewLocation;
	Sample.Distance = ToEnemy.Size();

	const FRotationMatrix ViewRotationMatrix(ViewContext.ViewRotation);
	const float ForwardDistance = FVector::DotProduct(ToEnemy, ViewRotationMatrix.GetUnitAxis(EAxis::X));
	const float HorizontalDistance = FVector::DotProduct(ToEnemy, ViewRotationMatrix.GetUnitAxis(EAxis::Y));
	const float VerticalDistance = FVector::DotProduct(ToEnemy, ViewRotationMatrix.GetUnitAxis(EAxis::Z));

	const float HalfHorizontalFOVRadians = FMath::DegreesToRadians(FMath::Clamp(ViewContext.HorizontalFOVDegrees, 5.0f, 170.0f) * 0.5f);
	const float HorizontalTangent = FMath::Max(FMath::Tan(HalfHorizontalFOVRadians), 0.01f);
	const float VerticalTangent = HorizontalTangent / FMath::Max(ViewContext.AspectRatio, 0.1f);
	const float SafeForwardDistance = FMath::Max(ForwardDistance, 1.0f);
	const float ProjectedHorizontalRadius = BoundsRadius / (SafeForwardDistance * HorizontalTangent);
	const float ProjectedVerticalRadius = BoundsRadius / (SafeForwardDistance * VerticalTangent);
	const float NormalizedHorizontalPosition = HorizontalDistance / (SafeForwardDistance * HorizontalTangent);
	const float NormalizedVerticalPosition = VerticalDistance / (SafeForwardDistance * VerticalTangent);

	const bool bBoundsInFront = ForwardDistance + BoundsRadius > 0.0f;
	const auto IntersectsFrustum = [bBoundsInFront, NormalizedHorizontalPosition, NormalizedVerticalPosition, ProjectedHorizontalRadius,
									ProjectedVerticalRadius](float Margin)
	{
		return bBoundsInFront && FMath::Abs(NormalizedHorizontalPosition) <= 1.0f + Margin + ProjectedHorizontalRadius &&
			   FMath::Abs(NormalizedVerticalPosition) <= 1.0f + Margin + ProjectedVerticalRadius;
	};

	Sample.bInPrimaryFrustum = IntersectsFrustum(0.0f);
	Sample.bInExpandedFrustum = IntersectsFrustum(FMath::Max(Policy.ExpandedFrustumMargin, 0.0f));
	Sample.FrustumFactor = Sample.bInPrimaryFrustum ? 1.0f : (Sample.bInExpandedFrustum ? 0.5f : 0.0f);

	if (Sample.bInPrimaryFrustum)
	{
		LastPrimaryFrustumTime = ViewContext.TimeSeconds;
	}
	const float RecentGraceSeconds = FMath::Max(Policy.RecentFrustumGraceSeconds, 0.0f);
	if (Sample.bInPrimaryFrustum)
	{
		Sample.RecentFrustumFactor = 1.0f;
	}
	else if (RecentGraceSeconds > 0.0f)
	{
		const float TimeSincePrimaryFrustum = ViewContext.TimeSeconds - LastPrimaryFrustumTime;
		Sample.RecentFrustumFactor = 1.0f - FMath::Clamp(TimeSincePrimaryFrustum / RecentGraceSeconds, 0.0f, 1.0f);
	}

	const float ProjectedScreenRadius = ForwardDistance > 0.0f ? FMath::Max(ProjectedHorizontalRadius, ProjectedVerticalRadius) : 0.0f;
	Sample.ScreenCoverageFactor =
		Sample.bInExpandedFrustum ? FMath::Clamp(ProjectedScreenRadius / FMath::Max(Policy.ScreenRadiusForFullScore, 0.001f), 0.0f, 1.0f)
								  : 0.0f;

	const float NearDistance = FMath::Max(Policy.NearDistance, 0.0f);
	const float FarDistance = FMath::Max(Policy.FarDistance, NearDistance + 1.0f);
	Sample.DistanceFactor = 1.0f - FMath::Clamp((Sample.Distance - NearDistance) / (FarDistance - NearDistance), 0.0f, 1.0f);

	const float FrustumWeight = FMath::Max(Policy.FrustumWeight, 0.0f);
	const float ScreenCoverageWeight = FMath::Max(Policy.ScreenCoverageWeight, 0.0f);
	const float RecentFrustumWeight = FMath::Max(Policy.RecentFrustumWeight, 0.0f);
	const float DistanceWeight = FMath::Max(Policy.DistanceWeight, 0.0f);
	const float WeightSum = FMath::Max(FrustumWeight + ScreenCoverageWeight + RecentFrustumWeight + DistanceWeight, KINDA_SMALL_NUMBER);
	Sample.Score = FMath::Clamp((Sample.FrustumFactor * FrustumWeight + Sample.ScreenCoverageFactor * ScreenCoverageWeight +
								 Sample.RecentFrustumFactor * RecentFrustumWeight + Sample.DistanceFactor * DistanceWeight) /
									WeightSum,
								0.0f, 1.0f);

	RenderSignificanceScore = Sample.Score;
	return Sample;
}

EFPEnemyRenderSignificanceTier AfpstrueEnemyCharacter::ResolveNaturalRenderSignificanceTier(const FFPEnemyRenderSignificanceSample& Sample,
																							const FFPEnemyRenderSignificancePolicy& Policy)
{
	// 升档立即响应，降档需要满足退出阈值、最短保持时间和延迟，避免视锥边缘反复切 LOD/阴影。
	const UWorld* World = GetWorld();
	const float CurrentTime = World != nullptr ? World->GetTimeSeconds() : 0.0f;
	if (!Policy.bEnableRenderTiering)
	{
		if (NaturalRenderSignificanceTier != EFPEnemyRenderSignificanceTier::Full)
		{
			LastNaturalRenderTierChangeTime = CurrentTime;
		}
		NaturalRenderSignificanceTier = EFPEnemyRenderSignificanceTier::Full;
		PendingRenderDemotionTier = NaturalRenderSignificanceTier;
		PendingRenderDemotionStartTime = -MAX_flt;
		return NaturalRenderSignificanceTier;
	}

	const float FullEnterThreshold = FMath::Clamp(Policy.FullEnterThreshold, 0.0f, 1.0f);
	const float FullExitThreshold = FMath::Min(FMath::Clamp(Policy.FullExitThreshold, 0.0f, 1.0f), FullEnterThreshold);
	const float ReducedEnterThreshold = FMath::Min(FMath::Clamp(Policy.ReducedEnterThreshold, 0.0f, 1.0f), FullExitThreshold);
	const float ReducedExitThreshold = FMath::Min(FMath::Clamp(Policy.ReducedExitThreshold, 0.0f, 1.0f), ReducedEnterThreshold);

	EFPEnemyRenderSignificanceTier DesiredTier = NaturalRenderSignificanceTier;
	switch (NaturalRenderSignificanceTier)
	{
	case EFPEnemyRenderSignificanceTier::Full:
		if (Sample.Score < ReducedExitThreshold)
		{
			DesiredTier = EFPEnemyRenderSignificanceTier::Background;
		}
		else if (Sample.Score < FullExitThreshold)
		{
			DesiredTier = EFPEnemyRenderSignificanceTier::Reduced;
		}
		break;

	case EFPEnemyRenderSignificanceTier::Reduced:
		if (Sample.Score >= FullEnterThreshold)
		{
			DesiredTier = EFPEnemyRenderSignificanceTier::Full;
		}
		else if (Sample.Score < ReducedExitThreshold)
		{
			DesiredTier = EFPEnemyRenderSignificanceTier::Background;
		}
		break;

	case EFPEnemyRenderSignificanceTier::Background:
	default:
		if (Sample.Score >= FullEnterThreshold)
		{
			DesiredTier = EFPEnemyRenderSignificanceTier::Full;
		}
		else if (Sample.Score >= ReducedEnterThreshold)
		{
			DesiredTier = EFPEnemyRenderSignificanceTier::Reduced;
		}
		break;
	}

	if (DesiredTier == NaturalRenderSignificanceTier)
	{
		PendingRenderDemotionTier = NaturalRenderSignificanceTier;
		PendingRenderDemotionStartTime = -MAX_flt;
		return NaturalRenderSignificanceTier;
	}

	const auto GetTierPriority = [](EFPEnemyRenderSignificanceTier Tier)
	{
		switch (Tier)
		{
		case EFPEnemyRenderSignificanceTier::Full:
			return 2;
		case EFPEnemyRenderSignificanceTier::Reduced:
			return 1;
		case EFPEnemyRenderSignificanceTier::Background:
		default:
			return 0;
		}
	};

	if (GetTierPriority(DesiredTier) > GetTierPriority(NaturalRenderSignificanceTier))
	{
		NaturalRenderSignificanceTier = DesiredTier;
		LastNaturalRenderTierChangeTime = CurrentTime;
		PendingRenderDemotionTier = NaturalRenderSignificanceTier;
		PendingRenderDemotionStartTime = -MAX_flt;
		return NaturalRenderSignificanceTier;
	}

	if (PendingRenderDemotionTier != DesiredTier || PendingRenderDemotionStartTime < 0.0f)
	{
		PendingRenderDemotionTier = DesiredTier;
		PendingRenderDemotionStartTime = CurrentTime;
	}

	const bool bMinimumHoldElapsed = CurrentTime - LastNaturalRenderTierChangeTime >= FMath::Max(Policy.MinimumTierHoldSeconds, 0.0f);
	const bool bDemotionDelayElapsed = CurrentTime - PendingRenderDemotionStartTime >= FMath::Max(Policy.DemotionDelaySeconds, 0.0f);
	if (bMinimumHoldElapsed && bDemotionDelayElapsed)
	{
		NaturalRenderSignificanceTier = DesiredTier;
		LastNaturalRenderTierChangeTime = CurrentTime;
		PendingRenderDemotionTier = NaturalRenderSignificanceTier;
		PendingRenderDemotionStartTime = -MAX_flt;
	}

	return NaturalRenderSignificanceTier;
}

void AfpstrueEnemyCharacter::ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier NewTier, bool bShouldCastShadow,
														 bool bShouldBeVisibleInRayTracing, bool bInForceFullAnimationAndLOD,
														 const FFPEnemyRenderSignificancePolicy& Policy)
{
	// Coordinator 在同一轮排序后统一提交结果；本对象只把策略转换成 SkeletalMeshComponent 的实际设置。
	if (IsDead())
	{
		return;
	}

	RenderSignificanceTier = Policy.bEnableRenderTiering ? NewTier : EFPEnemyRenderSignificanceTier::Full;
	bRenderShouldCastShadow = bShouldCastShadow;
	bRenderShouldBeVisibleInRayTracing = bShouldBeVisibleInRayTracing;
	bGameplayAnimationProtection = bInForceFullAnimationAndLOD;
	LastRenderSignificancePolicy = Policy;
	bHasRenderSignificancePolicy = true;
	ApplyRenderSignificanceSettings();
	RefreshAnimationSharingRegistration();
}

void AfpstrueEnemyCharacter::ApplyRenderSignificanceSettings()
{
	// 攻击和战斗保护优先于性能档：此时动画 Tick 与 LOD 恢复完整，阴影/RT 仍遵循各自独立预算。
	if (IsDead() || !bHasRenderSignificancePolicy)
	{
		return;
	}

	USkeletalMeshComponent* CharacterMesh = GetMesh();
	if (CharacterMesh == nullptr)
	{
		return;
	}

	float AnimationTickInterval = 0.0f;
	int32 RequestedMinLOD = LastRenderSignificancePolicy.FullMinLOD;
	const bool bUseFullAnimationAndLOD = IsAttacking() || bGameplayAnimationProtection;
	if (!bUseFullAnimationAndLOD && LastRenderSignificancePolicy.bEnableRenderTiering)
	{
		switch (RenderSignificanceTier)
		{
		case EFPEnemyRenderSignificanceTier::Reduced:
			AnimationTickInterval = MidRateAnimationTickInterval;
			RequestedMinLOD = LastRenderSignificancePolicy.ReducedMinLOD;
			break;

		case EFPEnemyRenderSignificanceTier::Background:
			AnimationTickInterval = FarRateAnimationTickInterval;
			RequestedMinLOD = LastRenderSignificancePolicy.BackgroundMinLOD;
			break;

		case EFPEnemyRenderSignificanceTier::Full:
		default:
			break;
		}
	}

	CharacterMesh->SetComponentTickInterval(
		bDisableAnimationOptimizationsForBenchmark || !LastRenderSignificancePolicy.bEnableAnimationTickTiering ? 0.0f
																												: AnimationTickInterval);

	if (!LastRenderSignificancePolicy.bEnableSkeletalLOD || bUseFullAnimationAndLOD)
	{
		RequestedMinLOD = 0;
	}
	const int32 LODCount = FMath::Max(CharacterMesh->GetNumLODs(), 1);
	const int32 SafeMinLOD = FMath::Clamp(RequestedMinLOD, 0, LODCount - 1);
	if (AppliedMinimumLOD != SafeMinLOD)
	{
		CharacterMesh->OverrideMinLOD(SafeMinLOD);
		AppliedMinimumLOD = SafeMinLOD;
	}

	const bool bShouldCastShadow =
		bDisableEnemyShadowsForBenchmark ? false : (!LastRenderSignificancePolicy.bEnableShadowBudget || bRenderShouldCastShadow);
	if (CharacterMesh->CastShadow != bShouldCastShadow)
	{
		CharacterMesh->SetCastShadow(bShouldCastShadow);
	}

	const bool bShouldBeVisibleInRayTracing = !bDisableEnemyRayTracingForBenchmark &&
											  (!LastRenderSignificancePolicy.bEnableRayTracingBudget || bRenderShouldBeVisibleInRayTracing);
	if (CharacterMesh->bVisibleInRayTracing != bShouldBeVisibleInRayTracing)
	{
		// 光栅可见性保持不变；这里只控制动态骨骼是否进入硬件光追场景和 BLAS 更新链。
		CharacterMesh->SetVisibleInRayTracing(bShouldBeVisibleInRayTracing);
	}
}

// ==================== Benchmark 诊断开关 ====================

void AfpstrueEnemyCharacter::ApplyBenchmarkDiagnosticOverrides(bool bDisableAttackSweep, bool bDisablePawnCollision,
															   bool bDisableCharacterMovementTick)
{
	// 破坏性关闭只用于定位消费者成本上界；正式160敌人基线不会传入这些参数。
	if (CombatComponent != nullptr)
	{
		CombatComponent->SetAttackSweepDisabledForBenchmark(bDisableAttackSweep);
	}

	if (bDisablePawnCollision)
	{
		GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->SetComponentTickEnabled(!bDisableCharacterMovementTick);
	}
}

// ==================== 战斗接口与动画优先级桥接 ====================

bool AfpstrueEnemyCharacter::CanStartAttack() const
{
	// 将只读攻击条件查询转发给事务所有者，AI 可在申请全局名额前排除不合格对象。
	return CombatComponent != nullptr && CombatComponent->CanStartAttack();
}

bool AfpstrueEnemyCharacter::TryAttackTarget()
{
	// AIController 通过窄接口启动攻击，不直接操作 CombatComponent 的内部状态和 Timer。
	return CombatComponent != nullptr && CombatComponent->TryAttackTarget();
}

void AfpstrueEnemyCharacter::HandleAttackFinishedNotify()
{
	// 蓝图动画结束 Notify 进入 C++ 统一收口；重复或迟到回调由 CombatComponent 幂等处理。
	if (CombatComponent != nullptr)
	{
		CombatComponent->HandleAttackFinishedNotify();
	}
}

void AfpstrueEnemyCharacter::BeginAttackWindow()
{
	// AnimNotifyState Begin 经角色桥接到 CombatComponent，开始记录刀刃连续轨迹。
	if (CombatComponent != nullptr)
	{
		CombatComponent->BeginAttackWindow();
	}
}

void AfpstrueEnemyCharacter::UpdateAttackWindow()
{
	// AnimNotifyState Tick 只在有效动画区间调用，角色本身不为近战检测开启常驻 Tick。
	if (CombatComponent != nullptr)
	{
		CombatComponent->UpdateAttackWindow();
	}
}

void AfpstrueEnemyCharacter::EndAttackWindow()
{
	// AnimNotifyState End 关闭伤害窗口，但完整攻击事务仍由结束 Notify 或保护 Timer 完成。
	if (CombatComponent != nullptr)
	{
		CombatComponent->EndAttackWindow();
	}
}
void AfpstrueEnemyCharacter::SetAttackAnimationPriority(bool bHighPriority)
{
	// 攻击前退出动画共享并恢复完整动画/移动更新，结束后再按当前 Significance 重新应用策略。
	if (bHighPriority)
	{
		// 独立 Montage/Notify 开始前先解除 LeaderPose，攻击逻辑不依赖共享动画。
		SuspendAnimationSharing();
	}

	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		CharacterMesh->VisibilityBasedAnimTickOption = (bDisableAnimationOptimizationsForBenchmark || bHighPriority)
			? EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones
			: EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered;

		if (bHighPriority)
		{
			CharacterMesh->SetComponentTickInterval(0.0f);
			if (AppliedMinimumLOD != 0)
			{
				CharacterMesh->OverrideMinLOD(0);
				AppliedMinimumLOD = 0;
			}
		}
	}

	if (bHighPriority)
	{
		if (UCharacterMovementComponent* Movement = GetCharacterMovement())
		{
			Movement->SetComponentTickInterval(0.0f);
		}
	}
	else
	{
		ApplyGameplaySignificanceIntervals();
		ApplyRenderSignificanceSettings();
		RefreshAnimationSharingRegistration();
	}
}

bool AfpstrueEnemyCharacter::IsTargetInAttackRange() const
{
	// 统一复用 CombatComponent 的二维距离规则，避免 AI、角色和显著性各写一套阈值判断。
	return CombatComponent != nullptr && CombatComponent->IsTargetInAttackRange();
}

AfpstrueCharacter* AfpstrueEnemyCharacter::GetCombatTarget() const
{
	// AIController 是目标状态的唯一拥有者；角色只在执行战斗和重要性判断时读取。
	const AfpstrueEnemyAIController* EnemyController = Cast<AfpstrueEnemyAIController>(GetController());
	return EnemyController != nullptr ? EnemyController->GetTargetCharacter() : nullptr;
}

// ==================== 受击与死亡 ====================

void AfpstrueEnemyCharacter::HandleDamageReceived(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy)
{
	// 受击会刷新战斗保护时间、退出动画共享并触发表现事件；生命扣减已由 HealthComponent 完成。
	if (const UWorld* World = GetWorld())
	{
		LastCombatRelevantTime = World->GetTimeSeconds();
	}
	// 受击表现由原 AnimBP/Montage 独立播放；下一次渲染重要性更新会进入 Full 保护期。
	SuspendAnimationSharing();

	ApplyHitReactionImpulse();
	OnEnemyDamaged(DamageAmount, DamageCauser, InstigatedBy);
}

void AfpstrueEnemyCharacter::ApplyHitReactionImpulse()
{
	// 活着时通过 CharacterMovement 施加水平冲量，不直接切换物理模拟，保持导航移动仍可继续。
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	if (Movement == nullptr || HitReactionImpulseStrength <= 0.0f || Movement->MovementMode == MOVE_None)
	{
		return;
	}

	FVector HitDirection(LastDamageDirection.X, LastDamageDirection.Y, 0.0f);
	if (!HitDirection.Normalize())
	{
		return;
	}

	Movement->AddImpulse(HitDirection * HitReactionImpulseStrength, true);
}

void AfpstrueEnemyCharacter::HandleDeath()
{
	// 死亡顺序先停止会继续回调的系统，再关闭移动/碰撞，最后进入 Ragdoll 并广播给 GameMode/蓝图。
	if (bDeathEffectsApplied)
	{
		return;
	}

	bDeathEffectsApplied = true;
	SuspendAnimationSharing();
	UnregisterFromSignificanceManager();
	if (CombatComponent != nullptr)
	{
		CombatComponent->ResetCombat();
	}
	SetAttackAnimationPriority(false);

	if (AfpstrueEnemyAIController* EnemyAIController = Cast<AfpstrueEnemyAIController>(GetController()))
	{
		EnemyAIController->StopAI();
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->MaxWalkSpeed = 0.0f;
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
		Movement->SetComponentTickEnabled(false);
	}

	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		CharacterMesh->SetComponentTickInterval(0.0f);
		CharacterMesh->SetCollisionProfileName(TEXT("Ragdoll"));
		CharacterMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		CharacterMesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		CharacterMesh->SetEnableGravity(true);
		CharacterMesh->SetSimulatePhysics(true);
	}

	OnEnemyDeathReported.Broadcast(this);
	OnEnemyDied();
	GetWorldTimerManager().SetTimerForNextTick(this, &AfpstrueEnemyCharacter::ApplyDeathImpulse);

	if (bDestroyOnDeath)
	{
		SetLifeSpan(DestroyDelay);
	}
}

void AfpstrueEnemyCharacter::ApplyDeathImpulse()
{
	// 延迟到下一帧，确保 Ragdoll 刚体已经创建并唤醒后再向实际命中位置施加死亡冲量。
	USkeletalMeshComponent* CharacterMesh = GetMesh();
	if (CharacterMesh == nullptr || !CharacterMesh->IsSimulatingPhysics())
	{
		return;
	}

	CharacterMesh->SetEnableGravity(true);
	CharacterMesh->WakeAllRigidBodies();

	const FVector ImpulseDirection = (LastDamageDirection + FVector::UpVector * DeathImpulseUpwardBias).GetSafeNormal();
	if (ImpulseDirection.IsNearlyZero())
	{
		return;
	}

	CharacterMesh->AddImpulseAtLocation(ImpulseDirection * FMath::Clamp(DeathImpulseStrength, 0.0f, 15000.0f), LastDamageLocation,
										LastDamageBoneName);
}

// ==================== Animation Sharing 接入桥 ====================

bool AfpstrueEnemyCharacter::CanUseAnimationSharing() const
{
	// 只有低渲染档、非战斗保护、非死亡且未模拟物理的敌人才可成为 Follower；攻击/布娃娃必须使用独立姿态。
	if (IsDead() || !bHasRenderSignificancePolicy || bDisableAnimationOptimizationsForBenchmark ||
		RenderSignificanceTier == EFPEnemyRenderSignificanceTier::Full ||
		RequiresGameplayAnimationProtection(GetWorld() != nullptr ? GetWorld()->GetTimeSeconds() : 0.0f,
											LastRenderSignificancePolicy.CombatPriorityGraceSeconds))
	{
		return false;
	}

	const USkeletalMeshComponent* CharacterMesh = GetMesh();
	return CharacterMesh != nullptr && !CharacterMesh->IsSimulatingPhysics();
}

void AfpstrueEnemyCharacter::RefreshAnimationSharingRegistration()
{
	// 语法复习：弱指针可能在不通知观察者的情况下失效，先 Get() 成局部裸指针再使用。
	if (UfpstrueEnemyAnimationSharingCoordinator* Coordinator = AnimationSharingCoordinator.Get())
	{
		Coordinator->RefreshEnemyRegistration(this);
	}
}

void AfpstrueEnemyCharacter::SetAnimationSharingCoordinator(UfpstrueEnemyAnimationSharingCoordinator* InCoordinator)
{
	// 此处已 include 协调器完整类型，编译器可以验证它能安全转换为 UObject 弱引用。
	AnimationSharingCoordinator = InCoordinator;
}

void AfpstrueEnemyCharacter::SuspendAnimationSharing()
{
	// 攻击、受击、死亡或退出时强制解除 Follower；Coordinator 负责实际注销句柄。
	if (UfpstrueEnemyAnimationSharingCoordinator* Coordinator = AnimationSharingCoordinator.Get())
	{
		Coordinator->SuspendEnemy(this);
	}
}
