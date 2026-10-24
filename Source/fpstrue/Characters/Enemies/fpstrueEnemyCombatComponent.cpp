// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifyQueue.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Runtime/fpstrueRuntime.h"
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
CSV_DECLARE_CATEGORY_EXTERN(fpstrueAI);

/*
 * 单个敌人的近战事务组件。
 * 行为树选择攻击分支、AIController 提交请求，本组件负责从开始、动画窗口、连续轨迹检测到结束的完整生命周期，
 * 并集中处理重复命中、Notify 丢失和死亡中断，避免 Character 与 Controller 各维护一份攻击状态。
 *
 * 攻击链：AI 持有内圈席位并对齐目标 -> TryAttackTarget 复核资格、取得出手名额并建立事务
 *       -> 蓝图用事务 ID 请求播放并绑定 Montage 实例
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

// 选择整组配置来源并初始化攻击冷却；未指定 Data Asset 时沿用蓝图默认值。
void UfpstrueEnemyCombatComponent::BeginPlay()
{
	// 配置先于冷却初始化：开局满足其他条件时可以立即攻击。
	Super::BeginPlay();
	// 只在 BeginPlay 选一次参数来源，热路径继续读取原有字段。
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
	if (const UWorld* World = GetWorld())
	{
		LastAttackTime = World->GetTimeSeconds() - AttackInterval;
	}
}

// 先禁止新攻击，再统一清理动画、计时和许可，令迟到通知成为无操作。
void UfpstrueEnemyCombatComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEndingPlay = true;
	ResetCombat();
	Super::EndPlay(EndPlayReason);
}

// 返回战斗组件所属的敌人角色；不兼容 Owner 返回空指针。
AfpstrueEnemyCharacter* UfpstrueEnemyCombatComponent::GetEnemy() const
{
	return Cast<AfpstrueEnemyCharacter>(GetOwner());
}

// 采样当前可见目标的距离和有效范围，再校验高度与环境遮挡；输出供本轮 AI 决策复用。
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
	return OutDistanceSquared <= FMath::Square(OutEffectiveRange) && HasClearAttackPath(*Enemy, *Target, EnemyLocation, TargetLocation);
}

// 使用调用者传入的同次位置检查高度及墙体阻挡，不能隔墙或跨过不合理高度发动近战。
bool UfpstrueEnemyCombatComponent::HasClearAttackPath(const AfpstrueEnemyCharacter& Enemy, const AfpstrueCharacter& Target,
	const FVector& EnemyLocation, const FVector& TargetLocation) const
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return false;
	}
	const float MaxHeightDifference = Enemy.GetCapsuleComponent()->GetScaledCapsuleHalfHeight() +
		Target.GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	if (FMath::Abs(EnemyLocation.Z - TargetLocation.Z) > MaxHeightDifference)
	{
		return false;
	}
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(EnemyMeleeEnvironment), false, &Enemy);
	QueryParams.AddIgnoredActor(&Target);
	return !World->LineTraceTestByChannel(EnemyLocation, TargetLocation, ECC_Visibility, QueryParams);
}

// 复用空间采样规则，只返回目标当前是否在可攻击范围，不创建攻击事务。
bool UfpstrueEnemyCombatComponent::IsTargetInAttackRange() const
{
	float DistanceSquared, EffectiveRange;
	return SampleAttackReach(DistanceSquared, EffectiveRange);
}

// 实时提交检查与 AI 快照预筛选共用资格规则，但采样时机不同。
bool UfpstrueEnemyCombatComponent::CanStartAttack() const
{
	// 提交入口重新采样真实距离和环境，不依赖 AI 较早生成的决策快照。
	const AfpstrueEnemyCharacter* Enemy = GetEnemy();
	const AfpstrueEnemyAIController* Controller = Enemy != nullptr ? Cast<AfpstrueEnemyAIController>(Enemy->GetController()) : nullptr;
	return IsAttackReady() && Controller->IsTacticalAttackPositionReady() && IsTargetInAttackRange();
}

// 检查存活目标、命令入口、空闲事务和冷却；不执行射线或重复距离采样。
bool UfpstrueEnemyCombatComponent::IsAttackReady() const
{
	const AfpstrueEnemyCharacter* Enemy = GetEnemy();
	const UWorld* World = GetWorld();
	const AfpstrueEnemyAIController* Controller = Enemy != nullptr ? Cast<AfpstrueEnemyAIController>(Enemy->GetController()) : nullptr;
	if (bEndingPlay || bStartingPlayback || Enemy == nullptr || !Enemy->HasAuthority() || World == nullptr || Controller == nullptr || !Controller->AcceptsCombatCommands())
	{
		return false;
	}

	const AfpstrueCharacter* TargetCharacter = Enemy->GetCombatTarget();
	return TargetCharacter != nullptr && !TargetCharacter->IsDead() && !Enemy->IsDead() && !IsAttacking() &&
		   World->GetTimeSeconds() - LastAttackTime >= AttackInterval;
}

// ==================== 攻击事务与动画窗口 ====================
// 实时资格通过后建立唯一攻击编号、目标和许可归属，再请求蓝图播放；回调替换事务则返回失败。
bool UfpstrueEnemyCombatComponent::TryAttackTarget()
{
	if (!CanStartAttack())
	{
		return false;
	}
	// 上面的资格检查已确认 Owner/World；提交前没有外部回调，直接使用本次有效上下文。
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	UWorld* World = GetWorld();
	const AfpstrueEnemyAIController* Controller = CastChecked<AfpstrueEnemyAIController>(Enemy->GetController());
	AfpstrueSurroundManager* PermissionSource = Controller->GetSurroundManager();
	// 最终提交点统一取得本次出手许可；Controller 不再先抢名额、转向失败又立即归还。
	if (PermissionSource != nullptr && !PermissionSource->TryAcquireAttackPermission(Enemy))
	{
		INC_DWORD_STAT(STAT_fpstrueAIAttackBudgetRejectedCount);
		CSV_CUSTOM_STAT(fpstrueAI, AttackBudgetRejectedCount, 1, ECsvCustomStatOp::Accumulate);
		return false;
	}

	// ★【重要边界】[状态提交] 广播播放请求前先提交攻击阶段、编号、目标和许可来源，观察者看到完整事务。
	AttackPhase = EFPEnemyAttackPhase::Windup;
	++AttackSequence;
	const uint32 StartingSequence = AttackSequence;
	bHitTargetThisAttack = false;
	AttackTarget = Enemy->GetCombatTarget();
	AttackPermissionSource = PermissionSource;
	AttackTacticalRevision = PermissionSource != nullptr ? Controller->GetTacticalAssignmentRevision() : 0;

	// 停止当前自主移动；不宣称该调用能够阻止后续外力或其他角色推挤。
	if (UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
	}

	// 先恢复战斗动画/移动，再通知蓝图播放；外部回调必须看到完整的攻击状态。
	Enemy->LastCombatRelevantTime = World->GetTimeSeconds();
	Enemy->SetAttackAnimationPriority(true);
	// 在动画预计时长之后设置一次性保护 Timer，Notify 丢失或 Montage 中断也不会永久占用攻击名额。
	ScheduleAttackFailSafe();
	Enemy->OnAttackPlaybackRequested(static_cast<int64>(StartingSequence));
	// ★【重要边界】[回调重入] 蓝图播放请求返回后复核攻击编号与状态；已中断或被替换的旧调用不能继续推进。
	return AttackSequence == StartingSequence && IsAttacking();
}

// 校验事务编号后播放并绑定本次 Montage 实例；播放失败清理当前攻击，旧命令不能干预新动作。
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
	// 捕获这次实际播放的实例，后续通知不得仅按 Montage 资产名称判断归属。
	USkeletalMeshComponent* Mesh = Enemy->GetMesh();
	UAnimInstance* Anim = Mesh->GetAnimInstance();
	// UE 5.5 的 true 只停止同 SlotGroup 的播放：保留原攻击/受击互斥，不影响其他组。
	const bool bPlayFailed = Anim == nullptr ||
		Anim->Montage_Play(Montage, PlayRate, EMontagePlayReturnType::MontageLength, 0.0f, true) <= 0.0f;
	FFPActionPlayback StartedPlayback;
	FAnimMontageInstance* Instance = bPlayFailed ? nullptr : StartedPlayback.TryBind(static_cast<uint32>(AttackId), Mesh, Montage);
	if (Instance == nullptr)
	{
		if (AttackId == static_cast<int64>(AttackSequence)) ResetCombat();
		return false;
	}
	// ★【重要边界】[回调重入] 播放 Montage 可触发旧动画的停止委托；复核事务后才把新实例登记为当前攻击播放。
	if (AttackId != static_cast<int64>(AttackSequence) || !IsAttacking() || bEndingPlay)
	{
		// 播放过程中旧 Montage 的停止委托可以取消动作，但不能递归开始另一轮动作。
		StartedPlayback.Stop();
		return false;
	}
	AttackPlayback = StartedPlayback;
	BindAttackPlaybackCompletion(*Instance);
	return true;
}

// 蓝图已显式播放时绑定其准确实例；拒绝其他 Mesh、过期事务或冲突播放，重复相同绑定不再订阅。
bool UfpstrueEnemyCombatComponent::BindAttackMontageForAttack(int64 AttackId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* Montage)
{
	const AfpstrueEnemyCharacter* Enemy = GetEnemy();
	if (bStartingPlayback || bEndingPlay || !IsAttacking() || AttackId != static_cast<int64>(AttackSequence) ||
		Enemy == nullptr || PlaybackMesh != Enemy->GetMesh()) return false;
	const bool bAlreadyBound = AttackPlayback.IsSet();
	FAnimMontageInstance* Instance = AttackPlayback.TryBind(AttackSequence, PlaybackMesh, Montage);
	if (Instance == nullptr) return false;
	if (!bAlreadyBound) BindAttackPlaybackCompletion(*Instance);
	return true;
}

// 订阅明确播放实例的完成回调并更新有限保护时长；清理事务后才调用原观察者。
void UfpstrueEnemyCombatComponent::BindAttackPlaybackCompletion(FAnimMontageInstance& Instance)
{
	const FFPActionPlayback ExpectedPlayback = AttackPlayback;
	ScheduleAttackFailSafe(&Instance);
	if (AfpstrueEnemyCharacter* Enemy = GetEnemy())
	{
		Enemy->PublishAttackPresentation(AttackSequence, Instance.Montage, Instance.GetPlayRate(), true);
	}
	// 只订阅明确实例，不订阅只含 Montage 资产的全局事件；同资源旧播放结束不能结束新事务。
	// 显式 Bind 路径已有的观察回调保留，但必须在本组件提交完成/中断后通知。
	const FOnMontageEnded PreviousObserver = Instance.OnMontageEnded;
	Instance.OnMontageEnded.BindWeakLambda(this, [this, ExpectedPlayback, PreviousObserver](UAnimMontage* Montage, bool bInterrupted)
	{
		// ★【重要边界】[异步身份] 动画完成必须同时匹配动作编号、Mesh、AnimInstance 和播放实例，不能仅凭同一动画资源收尾。
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

// 根据配置或真实剩余播放时长安排一次性保护；计时回调只结束其捕获编号对应的攻击。
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
	// 捕获当前事务编号，旧 Timer 即使迟到也不能结束新一轮攻击。
	const uint32 ScheduledSequence = AttackSequence;
	World->GetTimerManager().SetTimer(AttackFinishTimerHandle, FTimerDelegate::CreateWeakLambda(this, [this, ScheduledSequence]()
	{
		if (AttackSequence == ScheduledSequence) FinishAttack();
	}), FinishDelay, false);
}

// 同时校验攻击仍有效、动作编号和播放实例身份，过滤旧动画通知。
bool UfpstrueEnemyCombatComponent::IsAttackNotifyCurrent(USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference) const
{
	return IsAttacking() && AttackPlayback.Matches(AttackSequence, PlaybackMesh, EventReference);
}

// 打开当前攻击的伤害窗口并建立刀刃初始采样；已命中或重复 Begin 不重置一次性命中标记。
void UfpstrueEnemyCombatComponent::BeginAttackWindow()
{
	// NotifyState Begin 记录刀刃首个采样位置；后续 Tick 才能用上一帧到当前帧的轨迹补足快速运动区域。
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	if (Enemy == nullptr || !Enemy->HasAuthority() || Enemy->IsDead() || !IsAttacking() || AttackPhase == EFPEnemyAttackPhase::Active || bHitTargetThisAttack)
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

// 在有效动画窗口内扫描相邻姿态之间的刀刃轨迹；命中或中断后停止，旧调用栈不覆盖新事务采样。
// 【设计取舍】被射击的活体盒体可以简化，主动近战仍使用真实武器 Socket 轨迹，不能退化成距离内直接扣血。
// 窗口内有限多点 Sweep 换取低帧率下较少漏判，不保证任意高速运动绝对连续；窗口外不付常驻查询成本。
void UfpstrueEnemyCombatComponent::UpdateAttackWindow()
{
	// 只在动画有效帧执行连续 Sweep；已命中唯一玩家后会立即关闭窗口，避免后续帧重复扣血和无效查询。
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	if (Enemy == nullptr || !Enemy->HasAuthority() || AttackPhase != EFPEnemyAttackPhase::Active || Enemy->IsDead())
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

	// 消融只跳过碰撞查询，仍推进轨迹样本，避免恢复查询后扫过过长的历史运动。
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

// 从有效窗口进入收招阶段，只停止伤害检测；完整动作仍等待结束通知、完成回调或保护 Timer。
void UfpstrueEnemyCombatComponent::EndAttackWindow()
{
	if (AttackPhase == EFPEnemyAttackPhase::Active)
	{
		AttackPhase = EFPEnemyAttackPhase::Recovery;
	}
}

// ==================== 武器轨迹、碰撞查询与伤害去重 ====================

// 从本敌人的当前骨骼姿态取得刀根和刀尖世界坐标；Socket 缺失时拒绝检测。
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

// 对连续刀刃线段做球形 Sweep，按环境阻挡截断命中，再交给事务级目标及伤害去重。
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

	// 对象查询只收集 Pawn 候选，墙体是否阻挡由后续独立 Visibility 查询决定。
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

// 只给事务开始时捕获的玩家结算一次有效伤害；先标记再广播，伤害无效时仅回滚原事务标记。
bool UfpstrueEnemyCombatComponent::TryApplyAttackDamage(AActor* HitActor)
{
	// Sweep 可命中多个 Pawn，但当前事务只接受指定玩家且每次攻击最多成功扣血一次。
	AfpstrueEnemyCharacter* Enemy = GetEnemy();
	AfpstrueCharacter* TargetCharacter = AttackTarget.Get();
	// 准入撤销立即封住旧窗口，不等下一轮行为树；版本同时防止释放后重新授予的 ABA。
	if (AttackTacticalRevision != 0 && (!AttackPermissionSource.IsValid() ||
		!AttackPermissionSource->HasInnerSlot(Enemy, AttackTacticalRevision))) return false;
	if (Enemy == nullptr || !Enemy->HasAuthority() || HitActor == nullptr || TargetCharacter == nullptr || HitActor != TargetCharacter || TargetCharacter->IsDead() ||
		AttackPhase != EFPEnemyAttackPhase::Active || bHitTargetThisAttack ||
		!HasClearAttackPath(*Enemy, *TargetCharacter, Enemy->GetActorLocation(), TargetCharacter->GetActorLocation()))
	{
		return false;
	}

	const uint32 CommittingSequence = AttackSequence;
	// ★【重要边界】[结算去重] 扣血前锁存本次命中；重复 Sweep 和同步伤害回调不能让同一攻击再次扣血。
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

// 有身份结束通知只有匹配当前播放实例时才进入正常完成出口。
void UfpstrueEnemyCombatComponent::HandleAttackFinishedNotify(USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference)
{
	if (IsAttackNotifyCurrent(PlaybackMesh, EventReference))
	{
		FinishAttack();
	}
}

// 正常完成记录冷却起点，再复用统一清理；无有效事务时重复完成无副作用。
void UfpstrueEnemyCombatComponent::FinishAttack()
{
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

// 中断与生命周期退出的统一出口：先撤销事务身份、清 Timer 并归还原许可，最后停止旧动画。
void UfpstrueEnemyCombatComponent::ResetCombat()
{
	if (!IsAttacking()) return; // 空闲阶段没有攻击事务，不重复推进编号或清理 Timer。
	const FFPActionPlayback PlaybackToStop = AttackPlayback;
	AfpstrueSurroundManager* PermissionSource = AttackPermissionSource.Get();
	// ★【重要边界】[生命周期] 先使攻击编号失效并清目标、播放和 Timer，再归还许可及停止动画，旧回调只能退出。
	AttackPhase = EFPEnemyAttackPhase::Idle;
	++AttackSequence;
	bHitTargetThisAttack = false;
	AttackTarget.Reset();
	AttackPermissionSource.Reset();
	AttackTacticalRevision = 0;
	AttackPlayback.Reset();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(AttackFinishTimerHandle);
	}
	if (AfpstrueEnemyCharacter* Enemy = GetEnemy())
	{
		Enemy->PublishAttackPresentation(AttackSequence, nullptr, 1.0f, false);
		if (PermissionSource != nullptr)
		{
			// ★【重要边界】[资源归还] 许可只归还给攻击开始时捕获的管理器，不从已改变的 Controller 反查新来源。
			PermissionSource->ReleaseAttackPermission(Enemy);
		}
		if (!Enemy->IsDead())
		{
			Enemy->SetAttackAnimationPriority(false);
		}
	}
	// 清理只停止本次捕获的播放实例，不通过共享 Montage 资产误停新的动作。
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

// 将分支点 Payload 的原始 Montage 实例编号写入通知上下文，复用普通通知的身份校验。
FAnimNotifyEventReference MakeAttackNotifyReference(const FBranchingPointNotifyPayload& Payload)
{
	FAnimNotifyEventReference Reference(Payload.NotifyEvent, Payload.SequenceAsset);
	Reference.AddContextData<UE::Anim::FAnimNotifyMontageInstanceContext>(Payload.MontageInstanceID);
	return Reference;
}
} // namespace

// 普通动画通知保留引擎事件引用，交给所属敌人的 Combat 校验后完成攻击。
void UfpstrueAnimNotify_AttackFinished::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);
	if (UfpstrueEnemyCombatComponent* Combat = GetCombatForNotify(MeshComp))
	{
		Combat->HandleAttackFinishedNotify(MeshComp, EventReference);
	}
}

// 分支点完成通知只补播放身份，后续仍走同一攻击完成规则。
void UfpstrueAnimNotify_AttackFinished::BranchingPointNotify(FBranchingPointNotifyPayload& Payload)
{
	Notify(Payload.SkelMeshComponent, Payload.SequenceAsset, MakeAttackNotifyReference(Payload));
}

// 分支点窗口开始保留持续时间与播放实例编号，再复用普通窗口开始入口。
void UfpstrueAnimNotifyState_AttackWindow::BranchingPointNotifyBegin(FBranchingPointNotifyPayload& Payload)
{
	NotifyBegin(Payload.SkelMeshComponent, Payload.SequenceAsset,
		Payload.NotifyEvent != nullptr ? Payload.NotifyEvent->GetDuration() : 0.0f, MakeAttackNotifyReference(Payload));
}

// 分支点窗口更新携带原播放身份，不从当前 Montage 反推所属攻击。
void UfpstrueAnimNotifyState_AttackWindow::BranchingPointNotifyTick(FBranchingPointNotifyPayload& Payload, float FrameDeltaTime)
{
	NotifyTick(Payload.SkelMeshComponent, Payload.SequenceAsset, FrameDeltaTime, MakeAttackNotifyReference(Payload));
}

// 分支点窗口结束只关闭匹配实例的伤害窗口，不能结束或修改另一轮攻击。
void UfpstrueAnimNotifyState_AttackWindow::BranchingPointNotifyEnd(FBranchingPointNotifyPayload& Payload)
{
	NotifyEnd(Payload.SkelMeshComponent, Payload.SequenceAsset, MakeAttackNotifyReference(Payload));
}

// 动画进入有效帧区间时建立采样点；同一攻击的命中标志只在事务开始时清空。
void UfpstrueAnimNotifyState_AttackWindow::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
													   const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);

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

	if (UfpstrueEnemyCombatComponent* Combat = GetCombatForNotify(MeshComp); Combat != nullptr && Combat->IsAttackNotifyCurrent(MeshComp, EventReference))
	{
		Combat->EndAttackWindow();
	}
}
