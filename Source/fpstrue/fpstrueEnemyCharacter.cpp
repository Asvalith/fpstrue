// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemyCharacter.h"
#include "fpstrueBenchmarkConfig.h"
#include "fpstrueCharacter.h"
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

// ==================== 组件初始化与生命周期 ====================

AfpstrueEnemyCharacter::AfpstrueEnemyCharacter()
{
	PrimaryActorTick.bCanEverTick = false;
	AIControllerClass = AfpstrueEnemyAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;

	GetCapsuleComponent()->InitCapsuleSize(42.0f, 96.0f);

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
	Super::BeginPlay();
	SetActorTickEnabled(false);

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
	}

	if (HealthComponent != nullptr)
	{
		HealthComponent->OnDeath.AddUniqueDynamic(this, &AfpstrueEnemyCharacter::HandleDeath);
		HealthComponent->OnDamageReceived.AddUniqueDynamic(this, &AfpstrueEnemyCharacter::HandleDamageReceived);
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->MaxWalkSpeed = MoveSpeed;
		Movement->bOrientRotationToMovement = false;
		Movement->bUseControllerDesiredRotation = true;
		Movement->RotationRate = FRotator(0.0f, 540.0f, 0.0f);
	}
	bUseControllerRotationYaw = false;

	RegisterWithSignificanceManager();
}

void AfpstrueEnemyCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	SuspendAnimationSharing();
	UnregisterFromSignificanceManager();
	if (CombatComponent != nullptr)
	{
		CombatComponent->ResetCombat();
	}
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
	return HealthComponent != nullptr && HealthComponent->IsDead();
}

bool AfpstrueEnemyCharacter::IsAttacking() const
{
	return CombatComponent != nullptr && CombatComponent->IsAttacking();
}

bool AfpstrueEnemyCharacter::IsCombatActive() const
{
	return CombatComponent != nullptr && CombatComponent->IsCombatActive();
}

bool AfpstrueEnemyCharacter::RequiresGameplayAnimationProtection(float CurrentTime, float GraceSeconds) const
{
	const bool bRecentlyInteracted = CurrentTime - LastCombatRelevantTime <= FMath::Max(GraceSeconds, 0.0f);
	return IsCombatActive() || IsTargetInAttackRange() || bRecentlyInteracted;
}

float AfpstrueEnemyCharacter::GetDistanceToTarget2D() const
{
	return CombatComponent != nullptr ? CombatComponent->GetDistanceToTarget2D() : MAX_flt;
}

float AfpstrueEnemyCharacter::GetAttackRange() const
{
	return CombatComponent != nullptr ? CombatComponent->GetConfiguredAttackRange() : 0.0f;
}

float AfpstrueEnemyCharacter::GetEffectiveAttackRange() const
{
	return CombatComponent != nullptr ? CombatComponent->GetEffectiveAttackRange() : 0.0f;
}

void AfpstrueEnemyCharacter::FaceTarget()
{
	if (CombatComponent != nullptr)
	{
		CombatComponent->FaceTarget();
	}
}

// ==================== Gameplay Significance：目标距离与交互状态 ====================

void AfpstrueEnemyCharacter::RegisterWithSignificanceManager()
{
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
	if (IsDead())
	{
		return;
	}
	const float FullRateThreshold = 1.0f / (1.0f + FullRateMovementDistance);
	const float MidRateThreshold = 1.0f / (1.0f + MidRateMovementDistance);
	const bool bRequiresFullRate = IsCombatActive() || IsTargetInAttackRange();

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

bool AfpstrueEnemyCharacter::TryAttackTarget()
{
	return CombatComponent != nullptr && CombatComponent->TryAttackTarget();
}

void AfpstrueEnemyCharacter::HandleAttackFinishedNotify()
{
	if (CombatComponent != nullptr)
	{
		CombatComponent->HandleAttackFinishedNotify();
	}
}

void AfpstrueEnemyCharacter::BeginAttackWindow()
{
	if (CombatComponent != nullptr)
	{
		CombatComponent->BeginAttackWindow();
	}
}

void AfpstrueEnemyCharacter::UpdateAttackWindow()
{
	if (CombatComponent != nullptr)
	{
		CombatComponent->UpdateAttackWindow();
	}
}

void AfpstrueEnemyCharacter::EndAttackWindow()
{
	if (CombatComponent != nullptr)
	{
		CombatComponent->EndAttackWindow();
	}
}
void AfpstrueEnemyCharacter::SetAttackAnimationPriority(bool bHighPriority)
{
	if (bHighPriority)
	{
		// 独立 Montage/Notify 开始前先解除 LeaderPose，攻击逻辑不依赖共享动画。
		SuspendAnimationSharing();
	}

	if (USkeletalMeshComponent* CharacterMesh = GetMesh())
	{
		if (bDisableAnimationOptimizationsForBenchmark)
		{
			CharacterMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		}
		else
		{
			CharacterMesh->VisibilityBasedAnimTickOption = bHighPriority ? EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones
																		 : EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered;
		}

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
	if (HealthComponent != nullptr && HealthComponent->IsDead())
	{
		return;
	}
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
	if (AnimationSharingCoordinator != nullptr)
	{
		AnimationSharingCoordinator->RefreshEnemyRegistration(this);
	}
}

void AfpstrueEnemyCharacter::SuspendAnimationSharing()
{
	if (AnimationSharingCoordinator != nullptr)
	{
		AnimationSharingCoordinator->SuspendEnemy(this);
	}
}
