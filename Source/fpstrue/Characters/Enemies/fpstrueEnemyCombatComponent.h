// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Components/ActorComponent.h"
#include "fpstrueEnemyCombatComponent.generated.h"

class AfpstrueEnemyCharacter;
class UfpstrueEnemyCombatConfig;

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
 * 如何处理中断/重复 Notify，并在事务结束时通知 Controller 归还攻击名额。
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
	// 结合配置和胶囊尺寸返回实际近战距离。
	float GetEffectiveAttackRange() const;
	// AIController 读取追击接受距离所需的基础攻击范围。
	float GetConfiguredAttackRange() const { return AttackRange; }
	// 判断当前目标是否进入可攻击范围。
	bool IsTargetInAttackRange() const;
	// 提交攻击前使用实时距离重查冷却、目标和事务状态。
	bool CanStartAttack() const;
	// AI 可复用本轮已采样距离做预筛选；真正提交时仍重新检查目标与距离。
	bool CanStartAttackAtDistanceSquared(float DistanceSquared) const;

	// 由行为树动作请求开始一次攻击事务和动画表现。
	bool TryAttackTarget();
	// 由 AttackWindow Notify 打开武器轨迹检测。
	void BeginAttackWindow();
	// 由 AttackWindow Notify 每帧扫过上一采样点到当前采样点。
	void UpdateAttackWindow();
	// 只关闭伤害窗口；采样值不再使用，下次 Begin 会重新建立。
	void EndAttackWindow();
	// 由动画结束 Notify 提前完成攻击事务。
	void HandleAttackFinishedNotify();
	// 死亡、退出或外部中断时复位完整攻击事务。
	// 正常完成和中断共用状态、Timer、许可清理；冷却/动画恢复仍由调用者决定。
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
	// 完成攻击并通知 AI 释放攻击名额。
	void FinishAttack();

	// 攻击事务状态集中在组件中，EnemyCharacter 不再并行维护窗口、命中集合和结束计时器。
	float LastAttackTime = 0.0f;
	EFPEnemyAttackPhase AttackPhase = EFPEnemyAttackPhase::Idle;
	bool bHitTargetThisAttack = false;
	// 隔离伤害同步回调中的重置/新攻击；无参 Notify 仍要求攻击 Montage 不重叠。
	uint32 AttackSequence = 0;
	bool bDisableAttackSweepForBenchmark = false;
	FVector PreviousWeaponBase = FVector::ZeroVector;
	FVector PreviousWeaponTip = FVector::ZeroVector;
	FTimerHandle AttackFinishTimerHandle;
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
};
