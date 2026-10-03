// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Characters/Enemies/fpstrueEnemyCombatConfig.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifyQueue.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Runtime/fpstruePerformanceStats.h"
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
 * 攻击链：AI 获得攻击名额 -> TryAttackTarget 建立事务 -> 蓝图用事务 ID 请求播放并绑定 Montage 实例
 *       -> AnimNotifyState Begin/Tick/End 驱动有效窗口 -> Sweep 命中后 ApplyDamage
 *       -> 同实例的自然完成/结束 Notify 或保护 Timer 汇入 FinishAttack；中断走 ResetCombat，均归还原许可。
 * AttackPhase 统一描述前摇/有效窗口/收招；命中提交与消融开关是独立事实，不混入阶段枚举。
 */

// ==================== 生命周期与攻击范围 ====================

// 近战判定只在 AnimNotifyState 有效窗口内更新，因此组件本身不需要常驻 Tick。
UfpstrueEnemyCombatComponent::UfpstrueEnemyCombatComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UfpstrueEnemyCombatComponent::BeginPlay()
{
	// 配置先于冷却初始化：开局满足其他条件时可以立即攻击。
	Super::BeginPlay();
	ApplyCombatConfiguration();
	if (const UWorld* World = GetWorld())
	{
		LastAttackTime = World->GetTimeSeconds() - AttackInterval;
	}
}

void UfpstrueEnemyCombatComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 退出时清理事务和 Timer；迟到 Notify 在无事务时成为无操作。
	bEndingPlay = true;
	ResetCombat();
	Super::EndPlay(EndPlayReason);
}

void UfpstrueEnemyCombatComponent::ApplyCombatConfiguration()
{
	if (CombatConfiguration != nullptr)
	{
		AttackRange = CombatConfiguration->AttackRange;
		AttackDamage = CombatConfiguration->AttackDamage;
		AttackInterval = CombatConfiguration->AttackInterval;
		AttackAnimationDuration = CombatConfiguration->AttackAnimationDuration;
		AttackFailSafeDuration = CombatConfiguration->AttackFailSafeDuration;
		AttackCompletionGracePeriod = CombatConfiguration->AttackCompletionGracePeriod;
		WeaponTraceStartSocketName = CombatConfiguration->WeaponTraceStartSocketName;
		WeaponTraceEndSocketName = CombatConfiguration->WeaponTraceEndSocketName;
		WeaponTraceRadius = CombatConfiguration->WeaponTraceRadius;
		WeaponTraceSampleCount = CombatConfiguration->WeaponTraceSampleCount;
	}
	// 每个实例初始化一次，记录真正选中的来源；不在攻击热路径持续打日志。
	UE_LOG(LogTemp, Log, TEXT("EnemyCombatConfig Owner=%s Source=%s Range=%.1f Damage=%.1f Interval=%.2f Samples=%d"),
		   *GetNameSafe(GetOwner()), CombatConfiguration != nullptr ? *CombatConfiguration->GetPathName() : TEXT("BlueprintDefaults"),
		   AttackRange, AttackDamage, AttackInterval, WeaponTraceSampleCount);
}

AfpstrueEnemyCharacter* UfpstrueEnemyCombatComponent::GetEnemy() const
{
	// CombatComponent 只允许挂在敌人角色上；集中 Cast 后供其他规则函数复用。
	return Cast<AfpstrueEnemyCharacter>(GetOwner());
}

bool UfpstrueEnemyCombatComponent::SampleAttackReach(float& OutDistanceSquared, float& OutEffectiveRange) const
{
	// 一次解析目标、位置和胶囊，距离/范围供 AI 复用；只有距离与高度合格才查询环境遮挡。
	OutDistanceSquared = MAX_flt;
	OutEffectiveRange = 0.0f;
	const AfpstrueEnemyCharacter* Enemy = GetEnemy();
	const AfpstrueCharacter* Target = Enemy != nullptr ? Enemy->GetCombatTarget() : nullptr;
	if (Enemy == nullptr || !IsValid(Target))
	{
		return false;
	}

	const FVector EnemyLocation = Enemy->GetActorLocation();
	const FVector TargetLocation = Target->GetActorLocation();
	const UCapsuleComponent* EnemyCapsule = Enemy->GetCapsuleComponent();
	const UCapsuleComponent* TargetCapsule = Target->GetCapsuleComponent();
	OutDistanceSquared = FVector::DistSquared2D(EnemyLocation, TargetLocation);
	// 距离至少覆盖双方胶囊半径，避免已经相贴却永远进入不了攻击范围。
	OutEffectiveRange = FMath::Max(AttackRange, EnemyCapsule->GetScaledCapsuleRadius() + TargetCapsule->GetScaledCapsuleRadius() + 5.0f);
	return OutDistanceSquared <= FMath::Square(OutEffectiveRange) && HasClearAttackPath(Target);
}

bool UfpstrueEnemyCombatComponent::HasClearAttackPath(const AfpstrueCharacter* Target) const
{
	const AfpstrueEnemyCharacter* Enemy = GetEnemy();
	const UWorld* World = GetWorld();
	if (Enemy == nullptr || !IsValid(Target) || World == nullptr)
	{
		return false;
	}
	const FVector EnemyLocation = Enemy->GetActorLocation();
	const FVector TargetLocation = Target->GetActorLocation();
	const float MaxHeightDifference = Enemy->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() +
		Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	if (FMath::Abs(EnemyLocation.Z - TargetLocation.Z) > MaxHeightDifference)
	{
		return false;
	}
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(EnemyMeleeEnvironment), false, Enemy);
	QueryParams.AddIgnoredActor(Target);
	return !World->LineTraceTestByChannel(EnemyLocation, TargetLocation, ECC_Visibility, QueryParams);
}

bool UfpstrueEnemyCombatComponent::IsTargetInAttackRange() const
{
	float DistanceSquared, EffectiveRange;
	return SampleAttackReach(DistanceSquared, EffectiveRange);
}

// 实时提交检查与 AI 快照预筛选共用资格规则，但采样时机不同。
bool UfpstrueEnemyCombatComponent::CanStartAttack() const
{
	// 提交入口重新采样真实距离和环境，不依赖 AI 较早生成的决策快照。
	return IsAttackReady() && IsTargetInAttackRange();
}

bool UfpstrueEnemyCombatComponent::IsAttackReady() const
{
	// 资格规则仍集中于组件；不重复空间采样，也不允许该查询直接提交攻击。
	const AfpstrueEnemyCharacter* Enemy = GetEnemy();
	const UWorld* World = GetWorld();
	const AfpstrueEnemyAIController* Controller = Enemy != nullptr ? Cast<AfpstrueEnemyAIController>(Enemy->GetController()) : nullptr;
	if (bEndingPlay || bStartingPlayback || Enemy == nullptr || World == nullptr || Controller == nullptr || !Controller->AcceptsCombatCommands())
	{
		return false;
	}

	const AfpstrueCharacter* TargetCharacter = Enemy->GetCombatTarget();
	return TargetCharacter != nullptr && !TargetCharacter->IsDead() && !Enemy->IsDead() && !IsAttacking() &&
		   World->GetTimeSeconds() - LastAttackTime >= AttackInterval;
}

// ==================== 攻击事务与动画窗口 ====================
//攻击准备
bool UfpstrueEnemyCombatComponent::TryAttackTarget()
{
	// 只有冷却、目标和事务状态均满足才开始；成功后先建立 C++ 状态，再通知蓝图播放动画。
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	UWorld* World = GetWorld();
	if (Enemy == nullptr || World == nullptr || !CanStartAttack())
	{
		return false;
	}

	// 建立事务本身即确保伤害窗口尚未开启，不再先关闭一次窗口。
	AttackPhase = EFPEnemyAttackPhase::Windup;
	++AttackSequence;
	const uint32 StartingSequence = AttackSequence;
	bHitTargetThisAttack = false;
	AttackTarget = Enemy->GetCombatTarget();
	const AfpstrueEnemyAIController* Controller = Cast<AfpstrueEnemyAIController>(Enemy->GetController());
	AttackPermissionSource = Controller != nullptr ? Controller->GetSurroundManager() : nullptr;

	//停止移动，避免攻击过程中角色漂移或被物理推开，保证动画和轨迹检测的准确性。
	if (UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
	}

	// 先恢复战斗动画/移动，再通知蓝图播放；外部回调必须看到完整的攻击状态。
	Enemy->LastCombatRelevantTime = World->GetTimeSeconds();
	Enemy->SetAttackAnimationPriority(true);
	// 安排动画 Notify 缺失时的攻击结束保护 Timer。
	// 在动画预计时长之后设置一次性保护 Timer，Notify 丢失或 Montage 中断也不会永久占用攻击名额。
	ScheduleAttackFailSafe();
	Enemy->OnAttackPlaybackRequested(static_cast<int64>(StartingSequence));
	if (AttackSequence != StartingSequence || !IsAttacking())
	{
		return false; // 蓝图回调可以同步停止 AI 或替换上下文，不能继续绑定另一次播放。
	}
	// 观察通知不承担播放命令；资产迁移后此事件不得再自行播放攻击或完成事务。
	Enemy->OnAttackStarted();
	return AttackSequence == StartingSequence && IsAttacking();
}

bool UfpstrueEnemyCombatComponent::PlayAttackMontageForAttack(int64 AttackId, UAnimMontage* Montage, float PlayRate)
{
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	if (bStartingPlayback || bEndingPlay || !IsAttacking() || AttackId != static_cast<int64>(AttackSequence) || AttackPlayback.IsSet())
	{
		return false;
	}
	TGuardValue<bool> PlaybackGuard(bStartingPlayback, true);
	if (Enemy == nullptr || !IsValid(Montage) || !FMath::IsFinite(PlayRate) || PlayRate <= 0.0f)
	{
		ResetCombat(); // 当前命令确实失败，不能占着攻击名额等 5 秒保护超时。
		return false;
	}
	USkeletalMeshComponent* Mesh = Enemy->GetMesh();
	UAnimInstance* Anim = Mesh->GetAnimInstance();
	// UE 5.5 的 true 只停止同 SlotGroup 的播放：保留原攻击/受击互斥，不影响其他组。
	if (Anim == nullptr || Anim->Montage_Play(Montage, PlayRate, EMontagePlayReturnType::MontageLength, 0.0f, true) <= 0.0f)
	{
		if (AttackId == static_cast<int64>(AttackSequence)) ResetCombat();
		return false;
	}
	FFPActionPlayback StartedPlayback;
	if (!StartedPlayback.TryBind(static_cast<uint32>(AttackId), Mesh, Montage))
	{
		if (AttackId == static_cast<int64>(AttackSequence)) ResetCombat();
		return false;
	}
	if (AttackId != static_cast<int64>(AttackSequence) || !IsAttacking() || bEndingPlay)
	{
		// 播放过程中旧 Montage 的停止委托可以取消动作，但不能递归开始另一轮动作。
		StartedPlayback.Stop();
		return false;
	}
	AttackPlayback = StartedPlayback;
	BindAttackPlaybackCompletion();
	return true;
}

bool UfpstrueEnemyCombatComponent::BindAttackMontageForAttack(int64 AttackId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* Montage)
{
	const AfpstrueEnemyCharacter* Enemy = GetEnemy();
	if (bStartingPlayback || bEndingPlay || !IsAttacking() || AttackId != static_cast<int64>(AttackSequence) ||
		Enemy == nullptr || PlaybackMesh != Enemy->GetMesh()) return false;
	const bool bAlreadyBound = AttackPlayback.IsSet();
	if (!AttackPlayback.TryBind(AttackSequence, PlaybackMesh, Montage)) return false;
	if (!bAlreadyBound) BindAttackPlaybackCompletion();
	return true;
}

void UfpstrueEnemyCombatComponent::BindAttackPlaybackCompletion()
{
	const FFPActionPlayback ExpectedPlayback = AttackPlayback;
	FAnimMontageInstance* Instance = ExpectedPlayback.GetBoundInstance();
	if (Instance == nullptr) return;
	ScheduleAttackFailSafe(Instance);
	// 只订阅明确实例，不订阅只含 Montage 资产的全局事件；同资源旧播放结束不能结束新事务。
	// 显式 Bind 路径已有的观察回调保留，但必须在本组件提交完成/中断后通知。
	const FOnMontageEnded PreviousObserver = Instance->OnMontageEnded;
	Instance->OnMontageEnded.BindWeakLambda(this, [this, ExpectedPlayback, PreviousObserver](UAnimMontage* Montage, bool bInterrupted)
	{
		if (IsAttacking() && AttackSequence == ExpectedPlayback.ActionId &&
			AttackPlayback.MontageInstanceId == ExpectedPlayback.MontageInstanceId &&
			AttackPlayback.Mesh == ExpectedPlayback.Mesh && AttackPlayback.AnimInstance == ExpectedPlayback.AnimInstance &&
			Montage == ExpectedPlayback.Montage.Get())
		{
			if (bInterrupted) ResetCombat();
			else FinishAttack();
		}
		PreviousObserver.ExecuteIfBound(Montage, bInterrupted);
	});
}

void UfpstrueEnemyCombatComponent::ScheduleAttackFailSafe(const FAnimMontageInstance* Playback)
{
	UWorld* World = GetWorld();
	if (World == nullptr || !IsAttacking()) return;
	float MinimumDuration = FMath::Max(AttackAnimationDuration, AttackFailSafeDuration);
	if (Playback != nullptr && Playback->Montage != nullptr)
	{
		const UAnimMontage* Montage = Playback->Montage;
		const double EffectiveRate = static_cast<double>(Playback->GetPlayRate()) * Montage->RateScale;
		if (FMath::IsFinite(EffectiveRate) && EffectiveRate != 0.0)
		{
			const double RemainingTrack = EffectiveRate > 0.0 ? Montage->GetPlayLength() - Playback->GetPosition() : Playback->GetPosition();
			const double RemainingDuration = FMath::Max(0.0, RemainingTrack) / FMath::Abs(EffectiveRate) + Montage->BlendOut.GetBlendTime();
			if (FMath::IsFinite(RemainingDuration))
			{
				MinimumDuration = FMath::Max(MinimumDuration, static_cast<float>(FMath::Min(RemainingDuration, static_cast<double>(MAX_flt))));
			}
		}
	}
	// 本接口用于一次性攻击；循环/后续改速不会无限续租名额，最多获得本次绑定计算出的有限播放预算。
	const float FinishDelay = static_cast<float>(FMath::Clamp(static_cast<double>(MinimumDuration) +
		FMath::Max(0.0f, AttackCompletionGracePeriod), 0.01, static_cast<double>(MAX_flt)));
	const uint32 ScheduledSequence = AttackSequence;
	World->GetTimerManager().SetTimer(AttackFinishTimerHandle, FTimerDelegate::CreateWeakLambda(this, [this, ScheduledSequence]()
	{
		if (AttackSequence == ScheduledSequence) FinishAttack();
	}), FinishDelay, false);
}

bool UfpstrueEnemyCombatComponent::IsAttackNotifyCurrent(USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference) const
{
	return IsAttacking() && AttackPlayback.Matches(AttackSequence, PlaybackMesh, EventReference);
}

void UfpstrueEnemyCombatComponent::BeginAttackWindow()
{
	// NotifyState Begin 记录刀刃首个采样位置；后续 Tick 才能用上一帧到当前帧的轨迹补足快速运动区域。
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	if (Enemy == nullptr || Enemy->IsDead() || !IsAttacking() || AttackPhase == EFPEnemyAttackPhase::Active || bHitTargetThisAttack)
	{
		return;
	}

	FVector CurrentWeaponBase;
	FVector CurrentWeaponTip;
	if (!GetWeaponBladeSegment(CurrentWeaponBase, CurrentWeaponTip))
	{
		UE_LOG(LogTemp, Warning, TEXT("%s cannot start attack window: sockets '%s' and '%s' must both exist."), *Enemy->GetName(),
			   *WeaponTraceStartSocketName.ToString(), *WeaponTraceEndSocketName.ToString());
		return;
	}

	// 重复 Begin 不覆盖历史采样；已经命中过也不能重新打开同一事务。
	AttackPhase = EFPEnemyAttackPhase::Active;
	PreviousWeaponBase = CurrentWeaponBase;
	PreviousWeaponTip = CurrentWeaponTip;
}

void UfpstrueEnemyCombatComponent::UpdateAttackWindow()
{
	// 只在动画有效帧执行连续 Sweep；已命中唯一玩家后会立即关闭窗口，避免后续帧重复扣血和无效查询。
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	if (Enemy == nullptr || AttackPhase != EFPEnemyAttackPhase::Active || Enemy->IsDead())
	{
		return;
	}

	INC_DWORD_STAT(STAT_fpstrueAttackWindowUpdateCount);
	CSV_CUSTOM_STAT(fpstrueCombat, AttackWindowUpdateCount, 1, ECsvCustomStatOp::Accumulate);

	FVector CurrentWeaponBase;
	FVector CurrentWeaponTip;
	if (!GetWeaponBladeSegment(CurrentWeaponBase, CurrentWeaponTip))
	{
		EndAttackWindow();
		return;
	}

	if (bDisableAttackSweepForBenchmark)
	{
		PreviousWeaponBase = CurrentWeaponBase;
		PreviousWeaponTip = CurrentWeaponTip;
		return;
	}

	//socket之间插值出采样点：每个采样点计算上一帧到当前帧的轨迹，降低快速挥砍和低帧率下的漏判。
	const int32 SampleCount = FMath::Clamp(WeaponTraceSampleCount, 2, 8);
	const uint32 UpdatingSequence = AttackSequence;
	for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
	{
		const float Alpha = static_cast<float>(SampleIndex) / static_cast<float>(SampleCount - 1);
		const FVector PreviousSample = FMath::Lerp(PreviousWeaponBase, PreviousWeaponTip, Alpha);
		const FVector CurrentSample = FMath::Lerp(CurrentWeaponBase, CurrentWeaponTip, Alpha);
		SweepWeaponSegment(PreviousSample, CurrentSample);
		if (AttackSequence != UpdatingSequence || AttackPhase != EFPEnemyAttackPhase::Active)
		{
			// 命中/中断或伤害回调启动了另一事务，旧更新不能继续查询或覆盖新采样。
			return;
		}
	}

	// 点的跨帧轨迹与当前完整刀身覆盖不同空间，保留第二类 Sweep。
	SweepWeaponSegment(CurrentWeaponBase, CurrentWeaponTip);
	if (AttackSequence == UpdatingSequence && AttackPhase == EFPEnemyAttackPhase::Active)
	{
		PreviousWeaponBase = CurrentWeaponBase;
		PreviousWeaponTip = CurrentWeaponTip;
	}
}

void UfpstrueEnemyCombatComponent::EndAttackWindow()
{
	// NotifyState 结束只关闭伤害有效窗口，不直接结束完整攻击事务或冷却。
	// 关闭后不再使用上一帧轨迹样本；下一次 Begin 会重新建立起始位置。
	if (AttackPhase == EFPEnemyAttackPhase::Active)
	{
		AttackPhase = EFPEnemyAttackPhase::Recovery;
	}
}

// ==================== 武器轨迹、碰撞查询与伤害去重 ====================

bool UfpstrueEnemyCombatComponent::GetWeaponBladeSegment(FVector& OutBladeBase, FVector& OutBladeTip) const
{
	// 从当前动画姿态读取刀根和刀尖 Socket 世界坐标，构造这一帧的实际刀刃线段。
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

	// Pawn 对象查询负责找受击者，Visibility 阻挡负责截断刀刃轨迹；不能把墙也加入对象查询后仍遍历全部 Pawn。
	FCollisionQueryParams BlockerParams(QueryParams);
	if (AttackTarget.IsValid())
	{
		BlockerParams.AddIgnoredActor(AttackTarget.Get());
	}
	FHitResult BlockingHit;
	const bool bBlocked = World->SweepSingleByChannel(BlockingHit, TraceStart, TraceEnd, FQuat::Identity,
		ECC_Visibility, FCollisionShape::MakeSphere(WeaponTraceRadius), BlockerParams);
	const uint32 SweepingSequence = AttackSequence;
	for (const FHitResult& HitResult : HitResults)
	{
		if (bBlocked && HitResult.Time >= BlockingHit.Time)
		{
			continue;
		}
		const bool bAppliedDamage = TryApplyAttackDamage(HitResult.GetActor());
		if (bAppliedDamage || AttackSequence != SweepingSequence || AttackPhase != EFPEnemyAttackPhase::Active)
		{
			// 命中由提交函数关闭窗口；旧调用栈不能在回调后关闭另一轮攻击的窗口。
			break;
		}
	}
}

bool UfpstrueEnemyCombatComponent::TryApplyAttackDamage(AActor* HitActor)
{
	// Sweep 可命中多个 Pawn，但当前事务只接受指定玩家且每次攻击最多成功扣血一次。
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	AfpstrueCharacter* TargetCharacter = AttackTarget.Get();
	if (Enemy == nullptr || HitActor == nullptr || TargetCharacter == nullptr || HitActor != TargetCharacter || TargetCharacter->IsDead() ||
		AttackPhase != EFPEnemyAttackPhase::Active || bHitTargetThisAttack || !HasClearAttackPath(TargetCharacter))
	{
		return false;
	}

	const uint32 CommittingSequence = AttackSequence;
	// ApplyDamage 会同步调用外部监听者；先提交标记阻止同一事务重入扣血。
	bHitTargetThisAttack = true;
	const float AppliedDamage = UGameplayStatics::ApplyDamage(HitActor, AttackDamage, Enemy->GetController(), Enemy, nullptr);
	if (AttackSequence != CommittingSequence)
	{
		return AppliedDamage > 0.0f;
	}
	if (AppliedDamage <= 0.0f)
	{
		// 未造成伤害仍保留原来的重试语义，但只能回滚当前事务自己的提交标记。
		bHitTargetThisAttack = false;
		return false;
	}

	EndAttackWindow();
	return true;
}

// ==================== 中断清理与超时兜底 ====================

void UfpstrueEnemyCombatComponent::HandleAttackFinishedNotify()
{
	if (!bReportedLegacyFinish)
	{
		bReportedLegacyFinish = true;
		UE_LOG(LogTemp, Warning, TEXT("%s ignored deprecated identity-free attack finish; migrate to Enemy Attack Finished Notify."), *GetNameSafe(GetOwner()));
	}
}

void UfpstrueEnemyCombatComponent::HandleAttackFinishedNotify(USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference)
{
	if (IsAttackNotifyCurrent(PlaybackMesh, EventReference))
	{
		FinishAttack();
	}
}

void UfpstrueEnemyCombatComponent::FinishAttack()
{
	// 动画 Notify 与失败保护 Timer 共用该幂等出口：结束窗口、更新时间、恢复渲染策略并归还全局攻击名额。
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	if (Enemy == nullptr || !IsAttacking())
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		LastAttackTime = World->GetTimeSeconds();
	}
	ResetCombat();
}

void UfpstrueEnemyCombatComponent::ResetCombat()
{
	// 先清状态再执行动画停止回调；许可固定归还给开始时的 Manager，不读取可变化的 Controller。
	const bool bWasAttacking = IsAttacking();
	const FFPActionPlayback PlaybackToStop = AttackPlayback;
	AfpstrueSurroundManager* PermissionSource = AttackPermissionSource.Get();
	AttackPhase = EFPEnemyAttackPhase::Idle;
	++AttackSequence;
	bHitTargetThisAttack = false;
	AttackTarget.Reset();
	AttackPermissionSource.Reset();
	AttackPlayback.Reset();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(AttackFinishTimerHandle);
	}
	if (AfpstrueEnemyCharacter* Enemy = GetEnemy())
	{
		if (PermissionSource != nullptr)
		{
			PermissionSource->ReleaseAttackPermission(Enemy);
		}
		if (bWasAttacking && !Enemy->IsDead())
		{
			Enemy->SetAttackAnimationPriority(false);
		}
	}
	PlaybackToStop.Stop();
}

// Benchmark 开关只跳过 Sweep；攻击动画、状态和 Timer 仍正常运行，保证消融只改变一个消费者。
void UfpstrueEnemyCombatComponent::SetAttackSweepDisabledForBenchmark(bool bDisabled)
{
	bDisableAttackSweepForBenchmark = bDisabled;
}

// ==================== 动画窗口适配 ====================

namespace
{
// Notify 收到播放动画的 Mesh；事务属于其 Enemy Owner，不能保存在共享的 Notify 对象中。
UfpstrueEnemyCombatComponent* GetCombatForNotify(USkeletalMeshComponent* MeshComp)
{
	AfpstrueEnemyCharacter* Enemy = MeshComp != nullptr ? Cast<AfpstrueEnemyCharacter>(MeshComp->GetOwner()) : nullptr;
	return Enemy != nullptr ? Enemy->GetCombatComponent() : nullptr;
}

FAnimNotifyEventReference MakeAttackNotifyReference(const FBranchingPointNotifyPayload& Payload)
{
	FAnimNotifyEventReference Reference(Payload.NotifyEvent, Payload.SequenceAsset);
	Reference.AddContextData<UE::Anim::FAnimNotifyMontageInstanceContext>(Payload.MontageInstanceID);
	return Reference;
}
} // namespace

void UfpstrueAnimNotify_AttackFinished::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);
	if (UfpstrueEnemyCombatComponent* Combat = GetCombatForNotify(MeshComp))
	{
		Combat->HandleAttackFinishedNotify(MeshComp, EventReference);
	}
}

void UfpstrueAnimNotify_AttackFinished::BranchingPointNotify(FBranchingPointNotifyPayload& Payload)
{
	Notify(Payload.SkelMeshComponent, Payload.SequenceAsset, MakeAttackNotifyReference(Payload));
}

void UfpstrueAnimNotifyState_AttackWindow::BranchingPointNotifyBegin(FBranchingPointNotifyPayload& Payload)
{
	NotifyBegin(Payload.SkelMeshComponent, Payload.SequenceAsset,
		Payload.NotifyEvent != nullptr ? Payload.NotifyEvent->GetDuration() : 0.0f, MakeAttackNotifyReference(Payload));
}

void UfpstrueAnimNotifyState_AttackWindow::BranchingPointNotifyTick(FBranchingPointNotifyPayload& Payload, float FrameDeltaTime)
{
	NotifyTick(Payload.SkelMeshComponent, Payload.SequenceAsset, FrameDeltaTime, MakeAttackNotifyReference(Payload));
}

void UfpstrueAnimNotifyState_AttackWindow::BranchingPointNotifyEnd(FBranchingPointNotifyPayload& Payload)
{
	NotifyEnd(Payload.SkelMeshComponent, Payload.SequenceAsset, MakeAttackNotifyReference(Payload));
}

// 动画进入有效帧区间时建立采样点；同一攻击的命中标志只在事务开始时清空。
void UfpstrueAnimNotifyState_AttackWindow::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
													   const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);

	// AnimNotifyState Begin 直接交给 CombatComponent，开始记录刀刃连续轨迹。
	if (UfpstrueEnemyCombatComponent* Combat = GetCombatForNotify(MeshComp); Combat != nullptr && Combat->IsAttackNotifyCurrent(MeshComp, EventReference))
	{
		Combat->BeginAttackWindow();
	}
}

// 有效区间内每个动画更新步推进一次连续 Sweep，覆盖相邻姿态之间刀刃扫过的空间。
void UfpstrueAnimNotifyState_AttackWindow::NotifyTick(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float FrameDeltaTime,
													  const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyTick(MeshComp, Animation, FrameDeltaTime, EventReference);

	// AnimNotifyState Tick 只在有效动画区间调用，角色本身不为近战检测开启常驻 Tick。
	if (UfpstrueEnemyCombatComponent* Combat = GetCombatForNotify(MeshComp); Combat != nullptr && Combat->IsAttackNotifyCurrent(MeshComp, EventReference))
	{
		Combat->UpdateAttackWindow();
	}
}

// 离开有效帧区间后立即关闭检测，攻击事务本身仍由结束 Notify 或保护 Timer 收尾。
void UfpstrueAnimNotifyState_AttackWindow::NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
													 const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyEnd(MeshComp, Animation, EventReference);

	// AnimNotifyState End 关闭伤害窗口，但完整攻击事务仍由结束 Notify 或保护 Timer 完成。
	if (UfpstrueEnemyCombatComponent* Combat = GetCombatForNotify(MeshComp); Combat != nullptr && Combat->IsAttackNotifyCurrent(MeshComp, EventReference))
	{
		Combat->EndAttackWindow();
	}
}
