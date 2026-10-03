// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Components/ActorComponent.h"
#include "Characters/Shared/fpstrueActionPlayback.h"
#include "fpstrueEnemyCombatComponent.generated.h"

class AfpstrueEnemyCharacter;
class AfpstrueCharacter;
class AfpstrueSurroundManager;
class UAnimMontage;
class UfpstrueEnemyCombatConfig;
struct FAnimMontageInstance;

// 伤害窗口属于事务中的一个阶段，不能独立于攻击事务存在。
enum class EFPEnemyAttackPhase : uint8
{
	Idle,
	Windup,
	Active,
	Recovery
};

/**
 * 敌人近战模块：独占攻击事务、动画窗口、武器轨迹采样和一次攻击内的去重伤害。
 *
 * AIController 只请求开始攻击；AnimNotifyState 只报告动画窗口；本组件统一决定能否攻击、何时造成伤害、
 * 如何处理中断/重复 Notify，并向开始时捕获的 SurroundManager 归还攻击名额。
 */
UCLASS(ClassGroup = (Combat))
class FPSTRUE_API UfpstrueEnemyCombatComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	// 创建无常驻 Tick 的近战组件。
	UfpstrueEnemyCombatComponent();

	// AIController 判断攻击事务是否尚未结束。
	bool IsAttacking() const { return AttackPhase != EFPEnemyAttackPhase::Idle; }
	// 一次采样距离、胶囊修正后的范围和近战可达性，供本轮 BT 决策复用；不提交攻击。
	bool SampleAttackReach(float& OutDistanceSquared, float& OutEffectiveRange) const;
	// AIController 读取追击接受距离所需的基础攻击范围。
	float GetConfiguredAttackRange() const { return AttackRange; }
	// 判断当前目标是否进入可攻击范围。
	bool IsTargetInAttackRange() const;
	// 提交攻击前使用实时距离重查冷却、目标和事务状态。
	bool CanStartAttack() const;
	// 事务/目标/冷却资格与空间可达性分开；BT 先用本轮快照筛选，再申请许可。
	bool IsAttackReady() const;

	// 由行为树动作请求开始一次攻击事务和动画表现。
	bool TryAttackTarget();
	// 播放命令携带开始时的事务 ID；不从“当前 Montage”猜测本次攻击属于哪个动画。
	UFUNCTION(BlueprintCallable, Category = "Combat|Animation")
	bool PlayAttackMontageForAttack(int64 AttackId, UAnimMontage* Montage, float PlayRate = 1.0f);
	// 仅用于蓝图自行显式播放后立即绑定；迟到命令或已经绑定其他播放时拒绝。
	UFUNCTION(BlueprintCallable, Category = "Combat|Animation")
	bool BindAttackMontageForAttack(int64 AttackId, USkeletalMeshComponent* PlaybackMesh, UAnimMontage* Montage);
	// 由 AttackWindow Notify 打开武器轨迹检测。
	void BeginAttackWindow();
	// 由 AttackWindow Notify 每帧扫过上一采样点到当前采样点。
	void UpdateAttackWindow();
	// 只关闭伤害窗口；采样值不再使用，下次 Begin 会重新建立。
	void EndAttackWindow();
	// 只保留旧资产符号用于迁移诊断；没有身份的异步通知不能结束当前攻击。
	void HandleAttackFinishedNotify();
	void HandleAttackFinishedNotify(USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference);
	// 原生 Notify 使用动画携带的播放实例身份；不从当前事务倒填旧回调的身份。
	bool IsAttackNotifyCurrent(USkeletalMeshComponent* PlaybackMesh, const FAnimNotifyEventReference& EventReference) const;
	// 死亡、退出或外部中断时复位完整攻击事务。
	// 中断不消费正常完成冷却；存活角色的动画优先级也在此恢复。
	void ResetCombat();
	// Benchmark 消融入口：只关闭攻击 Sweep，不改变动画和 AI。
	void SetAttackSweepDisabledForBenchmark(bool bDisabled);

protected:
	// 应用配置并初始化冷却，保证开局可攻击。
	virtual void BeginPlay() override;
	// 清理攻击 Timer 和临时命中状态。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ==================== 攻击配置 ====================
	// 指定资产后在 BeginPlay 整组覆盖；否则保留原蓝图配置，不逐字段混合、不运行时热更新。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	TObjectPtr<UfpstrueEnemyCombatConfig> CombatConfiguration;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat", meta = (EditCondition = "CombatConfiguration == nullptr"))
	float AttackRange = 230.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat", meta = (EditCondition = "CombatConfiguration == nullptr"))
	float AttackDamage = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat", meta = (EditCondition = "CombatConfiguration == nullptr"))
	float AttackInterval = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Animation", meta = (EditCondition = "CombatConfiguration == nullptr"))
	float AttackAnimationDuration = 1.2f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Animation",
			  meta = (ClampMin = "0.1", EditCondition = "CombatConfiguration == nullptr"))
	float AttackFailSafeDuration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Animation",
			  meta = (ClampMin = "0.0", EditCondition = "CombatConfiguration == nullptr"))
	float AttackCompletionGracePeriod = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace", meta = (EditCondition = "CombatConfiguration == nullptr"))
	FName WeaponTraceStartSocketName = TEXT("weapontop");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace", meta = (EditCondition = "CombatConfiguration == nullptr"))
	FName WeaponTraceEndSocketName = TEXT("weaponend");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace",
			  meta = (ClampMin = "1.0", EditCondition = "CombatConfiguration == nullptr"))
	float WeaponTraceRadius = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace",
			  meta = (ClampMin = "2", ClampMax = "8", EditCondition = "CombatConfiguration == nullptr"))
	int32 WeaponTraceSampleCount = 4;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Debug")
	bool bDrawAttackTrace = false;

private:
	friend class FFpstrueEnemyCombatLifecycleTest;
	friend class FFpstrueEnemyCombatStateConfigTest;
	friend class FFpstrueEnemyAttackPlaybackTest;
	friend class FFpstrueEnemyAttackAssetIntegrationTest;
	// 只在 BeginPlay 选一次参数来源，热路径继续读取原有字段。
	void ApplyCombatConfiguration();
	// 返回强类型 Owner，供内部战斗逻辑使用。
	AfpstrueEnemyCharacter* GetEnemy() const;
	// 从骨骼 Socket 读取本帧武器根部和尖端位置。
	bool GetWeaponBladeSegment(FVector& OutBladeBase, FVector& OutBladeTip) const;
	// 对武器移动线段执行分段 Sweep，结果交给伤害去重。
	void SweepWeaponSegment(const FVector& TraceStart, const FVector& TraceEnd);
	// 对首次命中的有效目标施加伤害，并防止同一攻击重复命中。
	bool TryApplyAttackDamage(AActor* HitActor);
	// 起手和命中均检查高度/墙体；命中使用事务捕获目标，不能用 Controller 后来的目标代替。
	// 调用者校验双方并传入本次位置，距离与遮挡复用同一采样，不跨提交边界缓存。
	bool HasClearAttackPath(const AfpstrueEnemyCharacter& Enemy, const AfpstrueCharacter& Target,
		const FVector& EnemyLocation, const FVector& TargetLocation) const;
	// 绑定刚捕获实例的原生完成回调；观察者在内部清理之后执行，不能承担事务收尾职责。
	void BindAttackPlaybackCompletion(FAnimMontageInstance& Instance);
	// 未绑定时使用配置兜底，绑定后覆盖实际剩余播放时间；始终只保留一个有限的一次性 Timer。
	void ScheduleAttackFailSafe(const FAnimMontageInstance* Playback = nullptr);
	// 完成攻击并通知 AI 释放攻击名额。
	void FinishAttack();

	// 攻击事务状态集中在组件中，EnemyCharacter 不再并行维护窗口、命中集合和结束计时器。
	float LastAttackTime = 0.0f;
	EFPEnemyAttackPhase AttackPhase = EFPEnemyAttackPhase::Idle;
	bool bHitTargetThisAttack = false;
	// 事务身份处理同步重入，播放实例身份处理原生 Notify 的异步陈旧回调。
	uint32 AttackSequence = 0;
	TWeakObjectPtr<AfpstrueCharacter> AttackTarget;
	TWeakObjectPtr<AfpstrueSurroundManager> AttackPermissionSource;
	FFPActionPlayback AttackPlayback;
	bool bEndingPlay = false;
	bool bStartingPlayback = false;
	bool bReportedLegacyFinish = false;
	bool bDisableAttackSweepForBenchmark = false;
	FVector PreviousWeaponBase = FVector::ZeroVector;
	FVector PreviousWeaponTip = FVector::ZeroVector;
	FTimerHandle AttackFinishTimerHandle;
};

/** 带 Montage 播放身份的攻击结束通知；替代动画蓝图中的无参数结束事件。 */
UCLASS(meta = (DisplayName = "Enemy Attack Finished"))
class FPSTRUE_API UfpstrueAnimNotify_AttackFinished : public UAnimNotify
{
	GENERATED_BODY()

public:
	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
		const FAnimNotifyEventReference& EventReference) override;
	virtual void BranchingPointNotify(FBranchingPointNotifyPayload& Payload) override;
};

/** 敌人近战攻击窗口：由动画时间段驱动武器 Sweep 的开始、更新和结束。 */
UCLASS(meta = (DisplayName = "Enemy Attack Window"))
class FPSTRUE_API UfpstrueAnimNotifyState_AttackWindow : public UAnimNotifyState
{
	GENERATED_BODY()

public:
	// 动画进入攻击窗口时初始化本次近战检测。
	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration,
							 const FAnimNotifyEventReference& EventReference) override;

	// 动画窗口持续期间更新武器轨迹检测。
	virtual void NotifyTick(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float FrameDeltaTime,
							const FAnimNotifyEventReference& EventReference) override;

	// 动画离开攻击窗口时停止检测；完整攻击事务仍由结束 Notify 或保护 Timer 收尾。
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
						   const FAnimNotifyEventReference& EventReference) override;

	// UE 5.5 默认 BranchingPoint 桥不会填 Montage 上下文，因此在这里保留原 Payload 的播放身份。
	virtual void BranchingPointNotifyBegin(FBranchingPointNotifyPayload& Payload) override;
	virtual void BranchingPointNotifyTick(FBranchingPointNotifyPayload& Payload, float FrameDeltaTime) override;
	virtual void BranchingPointNotifyEnd(FBranchingPointNotifyPayload& Payload) override;
};
