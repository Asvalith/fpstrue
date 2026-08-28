// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemyCombatComponent.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyAIController.h"
#include "fpstrueEnemyCharacter.h"
#include "fpstrueHealthComponent.h"
#include "fpstruePerformanceStats.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"

DEFINE_STAT(STAT_fpstrueAttackSweepTime);
DEFINE_STAT(STAT_fpstrueAttackSweepCount);
DEFINE_STAT(STAT_fpstrueSweepReturnedHitCount);
DEFINE_STAT(STAT_fpstrueAttackWindowUpdateCount);
CSV_DEFINE_CATEGORY(fpstrueCombat, true);

UfpstrueEnemyCombatComponent::UfpstrueEnemyCombatComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UfpstrueEnemyCombatComponent::BeginPlay()
{
	Super::BeginPlay();
	if (const AfpstrueEnemyCharacter* Enemy = GetEnemy())
	{
		LastAttackTime = GetWorld()->GetTimeSeconds() - AttackInterval;
	}
}

void UfpstrueEnemyCombatComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ResetCombat();
	Super::EndPlay(EndPlayReason);
}

AfpstrueEnemyCharacter* UfpstrueEnemyCombatComponent::GetEnemy() const
{
	return Cast<AfpstrueEnemyCharacter>(GetOwner());
}

float UfpstrueEnemyCombatComponent::GetDistanceToTarget2D() const
{
	const AfpstrueEnemyCharacter* Enemy = GetEnemy();
	const AfpstrueCharacter* TargetCharacter = Enemy != nullptr ? Enemy->GetCombatTarget() : nullptr;
	if (Enemy == nullptr || TargetCharacter == nullptr)
	{
		return MAX_flt;
	}

	return FVector::Dist2D(Enemy->GetActorLocation(), TargetCharacter->GetActorLocation());
}

float UfpstrueEnemyCombatComponent::GetEffectiveAttackRange() const
{
	const AfpstrueEnemyCharacter* Enemy = GetEnemy();
	if (Enemy == nullptr)
	{
		return 0.0f;
	}

	const AfpstrueCharacter* TargetCharacter = Enemy->GetCombatTarget();
	const float EnemyRadius = Enemy->GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float TargetRadius = TargetCharacter != nullptr ? TargetCharacter->GetCapsuleComponent()->GetScaledCapsuleRadius() : 0.0f;
	const float MinimumReachableDistance = EnemyRadius + TargetRadius + 5.0f;
	return FMath::Max(AttackRange, MinimumReachableDistance);
}

bool UfpstrueEnemyCombatComponent::IsTargetInAttackRange() const
{
	const AfpstrueEnemyCharacter* Enemy = GetEnemy();
	const AfpstrueCharacter* TargetCharacter = Enemy != nullptr ? Enemy->GetCombatTarget() : nullptr;
	if (Enemy == nullptr || TargetCharacter == nullptr)
	{
		return false;
	}

	return FVector::DistSquared2D(Enemy->GetActorLocation(), TargetCharacter->GetActorLocation()) <=
		   FMath::Square(GetEffectiveAttackRange());
}

void UfpstrueEnemyCombatComponent::FaceTarget()
{
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	const AfpstrueCharacter* TargetCharacter = Enemy != nullptr ? Enemy->GetCombatTarget() : nullptr;
	if (Enemy == nullptr || TargetCharacter == nullptr)
	{
		return;
	}

	const FVector ToTarget = TargetCharacter->GetActorLocation() - Enemy->GetActorLocation();
	const FVector HorizontalToTarget(ToTarget.X, ToTarget.Y, 0.0f);
	if (!HorizontalToTarget.IsNearlyZero())
	{
		Enemy->SetActorRotation(HorizontalToTarget.GetSafeNormal().Rotation());
	}
}

bool UfpstrueEnemyCombatComponent::TryAttackTarget()
{
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	if (Enemy == nullptr || !CanAttack())
	{
		return false;
	}

	CancelAttackWindow();
	HitActorsThisAttack.Reset();
	bIsAttacking = true;
	bHitTargetThisAttack = false;
	if (const UWorld* World = GetWorld())
	{
		Enemy->LastCombatRelevantTime = World->GetTimeSeconds();
	}
	Enemy->SetAttackAnimationPriority(true);
	FaceTarget();

	if (UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
	}

	ScheduleAttackFinish(FMath::Max(AttackAnimationDuration, AttackFailSafeDuration));
	Enemy->OnAttackStarted();
	return true;
}

void UfpstrueEnemyCombatComponent::HandleAttackFinishedNotify()
{
	if (bIsAttacking)
	{
		FinishAttack();
	}
}

void UfpstrueEnemyCombatComponent::BeginAttackWindow()
{
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	if (Enemy == nullptr || Enemy->IsDead() || !bIsAttacking)
	{
		return;
	}

	CancelAttackWindow();

	FVector CurrentWeaponBase;
	FVector CurrentWeaponTip;
	if (!GetWeaponBladeSegment(CurrentWeaponBase, CurrentWeaponTip))
	{
		UE_LOG(LogTemp, Warning, TEXT("%s cannot start attack window: sockets '%s' and '%s' must both exist."), *Enemy->GetName(),
			   *WeaponTraceStartSocketName.ToString(), *WeaponTraceEndSocketName.ToString());
		return;
	}

	bAttackWindowActive = true;
	bHasPreviousWeaponSample = true;
	PreviousWeaponBase = CurrentWeaponBase;
	PreviousWeaponTip = CurrentWeaponTip;
}

void UfpstrueEnemyCombatComponent::UpdateAttackWindow()
{
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	if (Enemy == nullptr || !bAttackWindowActive || Enemy->IsDead() || !bIsAttacking)
	{
		return;
	}

	INC_DWORD_STAT(STAT_fpstrueAttackWindowUpdateCount);
	CSV_CUSTOM_STAT(fpstrueCombat, AttackWindowUpdateCount, 1, ECsvCustomStatOp::Accumulate);

	FVector CurrentWeaponBase;
	FVector CurrentWeaponTip;
	if (!GetWeaponBladeSegment(CurrentWeaponBase, CurrentWeaponTip))
	{
		CancelAttackWindow();
		return;
	}

	if (!bHasPreviousWeaponSample)
	{
		PreviousWeaponBase = CurrentWeaponBase;
		PreviousWeaponTip = CurrentWeaponTip;
		bHasPreviousWeaponSample = true;
		return;
	}

	if (bDisableAttackSweepForBenchmark)
	{
		PreviousWeaponBase = CurrentWeaponBase;
		PreviousWeaponTip = CurrentWeaponTip;
		return;
	}

	const int32 SampleCount = FMath::Clamp(WeaponTraceSampleCount, 2, 8);
	for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
	{
		const float Alpha = static_cast<float>(SampleIndex) / static_cast<float>(SampleCount - 1);
		const FVector PreviousSample = FMath::Lerp(PreviousWeaponBase, PreviousWeaponTip, Alpha);
		const FVector CurrentSample = FMath::Lerp(CurrentWeaponBase, CurrentWeaponTip, Alpha);
		SweepWeaponSegment(PreviousSample, CurrentSample);
	}

	SweepWeaponSegment(CurrentWeaponBase, CurrentWeaponTip);
	PreviousWeaponBase = CurrentWeaponBase;
	PreviousWeaponTip = CurrentWeaponTip;
}

void UfpstrueEnemyCombatComponent::EndAttackWindow()
{
	if (bAttackWindowActive)
	{
		CancelAttackWindow();
	}
}

void UfpstrueEnemyCombatComponent::SetAttackSweepDisabledForBenchmark(bool bDisabled)
{
	bDisableAttackSweepForBenchmark = bDisabled;
}

void UfpstrueEnemyCombatComponent::ResetCombat()
{
	CancelAttackWindow();
	bIsAttacking = false;
	bHitTargetThisAttack = false;
	HitActorsThisAttack.Reset();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(AttackFinishTimerHandle);
	}
	if (AfpstrueEnemyCharacter* Enemy = GetEnemy())
	{
		if (AfpstrueEnemyAIController* AIController = Cast<AfpstrueEnemyAIController>(Enemy->GetController()))
		{
			AIController->ReleaseAttackPermission();
		}
	}
}

bool UfpstrueEnemyCombatComponent::CanAttack() const
{
	const AfpstrueEnemyCharacter* Enemy = GetEnemy();
	const UWorld* World = GetWorld();
	if (Enemy == nullptr || World == nullptr)
	{
		return false;
	}

	const AfpstrueCharacter* TargetCharacter = Enemy->GetCombatTarget();
	return TargetCharacter != nullptr && TargetCharacter->GetHealthComponent() != nullptr &&
		   !TargetCharacter->GetHealthComponent()->IsDead() && !Enemy->IsDead() && !bIsAttacking && IsTargetInAttackRange() &&
		   World->GetTimeSeconds() - LastAttackTime >= AttackInterval;
}

bool UfpstrueEnemyCombatComponent::GetWeaponBladeSegment(FVector& OutBladeBase, FVector& OutBladeTip) const
{
	const AfpstrueEnemyCharacter* Enemy = GetEnemy();
	const USkeletalMeshComponent* CharacterMesh = Enemy != nullptr ? Enemy->GetMesh() : nullptr;
	if (CharacterMesh == nullptr || !CharacterMesh->DoesSocketExist(WeaponTraceStartSocketName) ||
		!CharacterMesh->DoesSocketExist(WeaponTraceEndSocketName))
	{
		return false;
	}

	OutBladeBase = CharacterMesh->GetSocketLocation(WeaponTraceStartSocketName);
	OutBladeTip = CharacterMesh->GetSocketLocation(WeaponTraceEndSocketName);
	return true;
}

void UfpstrueEnemyCombatComponent::SweepWeaponSegment(const FVector& TraceStart, const FVector& TraceEnd)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FpstrueEnemy_AttackSweep);
	CSV_SCOPED_TIMING_STAT(fpstrueCombat, AttackSweepTime);
	SCOPE_CYCLE_COUNTER(STAT_fpstrueAttackSweepTime);
	INC_DWORD_STAT(STAT_fpstrueAttackSweepCount);
	CSV_CUSTOM_STAT(fpstrueCombat, AttackSweepCount, 1, ECsvCustomStatOp::Accumulate);

	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	UWorld* World = GetWorld();
	if (Enemy == nullptr || World == nullptr)
	{
		return;
	}

	FCollisionObjectQueryParams ObjectQueryParams;
	ObjectQueryParams.AddObjectTypesToQuery(ECC_Pawn);
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(EnemyWeaponTrace), false, Enemy);
	QueryParams.AddIgnoredActor(Enemy);

	TArray<FHitResult> HitResults;
	const bool bHitAnyPawn = World->SweepMultiByObjectType(HitResults, TraceStart, TraceEnd, FQuat::Identity, ObjectQueryParams,
														   FCollisionShape::MakeSphere(WeaponTraceRadius), QueryParams);

	INC_DWORD_STAT_BY(STAT_fpstrueSweepReturnedHitCount, HitResults.Num());
	CSV_CUSTOM_STAT(fpstrueCombat, SweepReturnedHitCount, HitResults.Num(), ECsvCustomStatOp::Accumulate);

	if (bDrawAttackTrace)
	{
		const FColor DebugColor = bHitAnyPawn ? FColor::Green : FColor::Yellow;
		DrawDebugLine(World, TraceStart, TraceEnd, DebugColor, false, 0.1f, 0, 1.5f);
		DrawDebugSphere(World, TraceStart, WeaponTraceRadius, 8, DebugColor, false, 0.1f);
		DrawDebugSphere(World, TraceEnd, WeaponTraceRadius, 8, DebugColor, false, 0.1f);
	}

	if (!bHitAnyPawn)
	{
		return;
	}

	for (const FHitResult& HitResult : HitResults)
	{
		TryApplyAttackDamage(HitResult.GetActor());
	}
}

bool UfpstrueEnemyCombatComponent::TryApplyAttackDamage(AActor* HitActor)
{
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	AfpstrueCharacter* TargetCharacter = Enemy != nullptr ? Enemy->GetCombatTarget() : nullptr;
	if (Enemy == nullptr || HitActor == nullptr || TargetCharacter == nullptr || HitActor != TargetCharacter || TargetCharacter->IsDead() ||
		bHitTargetThisAttack)
	{
		return false;
	}

	const TWeakObjectPtr<AActor> WeakHitActor(HitActor);
	if (HitActorsThisAttack.Contains(WeakHitActor))
	{
		return false;
	}

	const float AppliedDamage = UGameplayStatics::ApplyDamage(HitActor, AttackDamage, Enemy->GetController(), Enemy, nullptr);
	if (AppliedDamage <= 0.0f)
	{
		return false;
	}

	HitActorsThisAttack.Add(WeakHitActor);
	bHitTargetThisAttack = true;
	return true;
}

void UfpstrueEnemyCombatComponent::CancelAttackWindow()
{
	bAttackWindowActive = false;
	bHasPreviousWeaponSample = false;
	PreviousWeaponBase = FVector::ZeroVector;
	PreviousWeaponTip = FVector::ZeroVector;
}

void UfpstrueEnemyCombatComponent::ScheduleAttackFinish(float DurationSeconds)
{
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	if (Enemy == nullptr)
	{
		return;
	}

	GetWorld()->GetTimerManager().SetTimer(AttackFinishTimerHandle, this, &UfpstrueEnemyCombatComponent::FinishAttack,
										   FMath::Max(0.01f, DurationSeconds + AttackCompletionGracePeriod), false);
}

void UfpstrueEnemyCombatComponent::FinishAttack()
{
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	if (Enemy == nullptr || !bIsAttacking)
	{
		return;
	}

	EndAttackWindow();
	bIsAttacking = false;
	Enemy->SetAttackAnimationPriority(false);
	GetWorld()->GetTimerManager().ClearTimer(AttackFinishTimerHandle);
	if (UWorld* World = GetWorld())
	{
		LastAttackTime = World->GetTimeSeconds();
	}
	HitActorsThisAttack.Reset();
	if (AfpstrueEnemyAIController* AIController = Cast<AfpstrueEnemyAIController>(Enemy->GetController()))
	{
		// 动画 Notify 和失败保护计时器最终都汇入这里，统一归还攻击预算。
		AIController->ReleaseAttackPermission();
	}
}
