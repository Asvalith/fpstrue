// Copyright Epic Games, Inc. All Rights Reserved.

#include "fpstrueEnemyCombatComponent.h"
#include "fpstrueCharacter.h"
#include "fpstrueEnemyAIController.h"
#include "fpstrueEnemyCharacter.h"
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

/*
 * 单个敌人的近战事务组件。
 * AIController 只负责“何时攻击”，本组件负责一次攻击从开始、动画窗口、连续轨迹检测到结束的完整生命周期，
 * 并集中处理重复命中、Notify 丢失和死亡中断，避免 Character 与 Controller 各维护一份攻击状态。
 *
 * 攻击链：AI 获得攻击名额 -> TryAttackTarget 建立事务 -> 蓝图播放 Montage
 *       -> AnimNotifyState Begin/Tick/End 驱动有效窗口 -> Sweep 命中后 ApplyDamage
 *       -> 结束 Notify 或保护 Timer 汇入 FinishAttack -> 归还攻击名额。
 * bIsAttacking、bAttackWindowActive 和 bHitTargetThisAttack 都只由本组件写入，分别表示事务、有效判定窗口和
 * 单次命中提交状态，避免把“动画正在播放”和“本帧可以造成伤害”混成一个布尔值。
 */

// ==================== 生命周期与攻击范围 ====================

UfpstrueEnemyCombatComponent::UfpstrueEnemyCombatComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UfpstrueEnemyCombatComponent::BeginPlay()
{
	Super::BeginPlay();
	if (const UWorld* World = GetWorld())
	{
		LastAttackTime = World->GetTimeSeconds() - AttackInterval;
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

// ==================== 攻击事务与动画窗口 ====================

bool UfpstrueEnemyCombatComponent::TryAttackTarget()
{
	// 只有冷却、目标和事务状态均满足才开始；成功后先建立 C++ 状态，再通知蓝图播放动画。
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	if (Enemy == nullptr || !CanStartAttack())
	{
		return false;
	}

	CancelAttackWindow();
	bIsAttacking = true;
	bHitTargetThisAttack = false;
	if (const UWorld* World = GetWorld())
	{
		Enemy->LastCombatRelevantTime = World->GetTimeSeconds();
	}
	Enemy->SetAttackAnimationPriority(true);

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
	// FinishAttack 统一校验事务状态，Notify 和保护 Timer 共用同一出口。
	FinishAttack();
}

void UfpstrueEnemyCombatComponent::BeginAttackWindow()
{
	// NotifyState Begin 记录刀刃首个采样位置；后续 Tick 才能用上一帧到当前帧的轨迹补足快速运动区域。
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
	// 只在动画有效帧执行连续 Sweep；已命中唯一玩家后会立即关闭窗口，避免后续帧重复扣血和无效查询。
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
		if (!bAttackWindowActive)
		{
			// 当前攻击已经命中唯一玩家目标，无需继续提交本帧剩余 Sweep。
			return;
		}
	}

	SweepWeaponSegment(CurrentWeaponBase, CurrentWeaponTip);
	PreviousWeaponBase = CurrentWeaponBase;
	PreviousWeaponTip = CurrentWeaponTip;
}

void UfpstrueEnemyCombatComponent::EndAttackWindow()
{
	CancelAttackWindow();
}

// Benchmark 开关只跳过 Sweep；攻击动画、状态和 Timer 仍正常运行，保证消融只改变一个消费者。
void UfpstrueEnemyCombatComponent::SetAttackSweepDisabledForBenchmark(bool bDisabled)
{
	bDisableAttackSweepForBenchmark = bDisabled;
}

void UfpstrueEnemyCombatComponent::ResetCombat()
{
	CancelAttackWindow();
	bIsAttacking = false;
	bHitTargetThisAttack = false;
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

bool UfpstrueEnemyCombatComponent::CanStartAttack() const
{
	// 该查询不修改状态，AIController 可在申请全局攻击名额前先排除冷却、死亡和距离不满足的敌人。
	const AfpstrueEnemyCharacter* Enemy = GetEnemy();
	const UWorld* World = GetWorld();
	if (Enemy == nullptr || World == nullptr)
	{
		return false;
	}

	const AfpstrueCharacter* TargetCharacter = Enemy->GetCombatTarget();
	return TargetCharacter != nullptr && !TargetCharacter->IsDead() && !Enemy->IsDead() && !bIsAttacking && IsTargetInAttackRange() &&
		   World->GetTimeSeconds() - LastAttackTime >= AttackInterval;
}

// ==================== 武器轨迹、碰撞查询与伤害去重 ====================

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
	// Sweep 使用刀刃上一帧到当前帧的连续路径，而不是只依赖当前帧 Overlap，降低快速挥砍和低帧率下的漏判。
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
		if (TryApplyAttackDamage(HitResult.GetActor()))
		{
			// 当前玩法只允许命中唯一玩家目标；成功后关闭窗口，后续帧不再做无效 Sweep。
			CancelAttackWindow();
			break;
		}
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

	const float AppliedDamage = UGameplayStatics::ApplyDamage(HitActor, AttackDamage, Enemy->GetController(), Enemy, nullptr);
	if (AppliedDamage <= 0.0f)
	{
		return false;
	}

	bHitTargetThisAttack = true;
	return true;
}

// ==================== 中断清理与超时兜底 ====================

void UfpstrueEnemyCombatComponent::CancelAttackWindow()
{
	bAttackWindowActive = false;
	bHasPreviousWeaponSample = false;
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
	// 动画 Notify 与失败保护 Timer 共用该幂等出口：结束窗口、更新时间、恢复渲染策略并归还全局攻击名额。
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
	if (AfpstrueEnemyAIController* AIController = Cast<AfpstrueEnemyAIController>(Enemy->GetController()))
	{
		// 动画 Notify 和失败保护计时器最终都汇入这里，统一归还攻击预算。
		AIController->ReleaseAttackPermission();
	}
}
