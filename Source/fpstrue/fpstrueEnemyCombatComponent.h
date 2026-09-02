// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "fpstrueEnemyCombatComponent.generated.h"

class AfpstrueEnemyCharacter;

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
	bool IsAttacking() const { return bIsAttacking; }
	// 结合配置和胶囊尺寸返回实际近战距离。
	float GetEffectiveAttackRange() const;
	// AIController 通过 EnemyCharacter 转发读取基础攻击范围。
	float GetConfiguredAttackRange() const { return AttackRange; }
	// 判断当前目标是否进入可攻击范围。
	bool IsTargetInAttackRange() const;
	// AI 在申请攻击名额前检查冷却、目标、距离和事务状态。
	bool CanStartAttack() const;

	// 由 AI FSM 请求开始一次攻击事务和动画表现。
	bool TryAttackTarget();
	// 由动画结束 Notify 提前完成攻击事务。
	void HandleAttackFinishedNotify();
	// 由 AttackWindow Notify 打开武器轨迹检测。
	void BeginAttackWindow();
	// 由 AttackWindow Notify 每帧扫过上一采样点到当前采样点。
	void UpdateAttackWindow();
	// 由 AttackWindow Notify 关闭本次轨迹检测。
	void EndAttackWindow();
	// Benchmark 消融入口：只关闭攻击 Sweep，不改变动画和 AI。
	void SetAttackSweepDisabledForBenchmark(bool bDisabled);
	// 死亡、退出或外部中断时复位完整攻击事务。
	void ResetCombat();

protected:
	// 缓存并校验 Owner EnemyCharacter。
	virtual void BeginPlay() override;
	// 清理攻击 Timer 和临时命中状态。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ==================== 攻击配置 ====================

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	float AttackRange = 230.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	float AttackDamage = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	float AttackInterval = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Animation")
	float AttackAnimationDuration = 1.2f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Animation", meta = (ClampMin = "0.1"))
	float AttackFailSafeDuration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Animation", meta = (ClampMin = "0.0"))
	float AttackCompletionGracePeriod = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace")
	FName WeaponTraceStartSocketName = TEXT("weapontop");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace")
	FName WeaponTraceEndSocketName = TEXT("weaponend");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace", meta = (ClampMin = "1.0"))
	float WeaponTraceRadius = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Weapon Trace", meta = (ClampMin = "2", ClampMax = "8"))
	int32 WeaponTraceSampleCount = 4;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Debug")
	bool bDrawAttackTrace = false;

private:
	// 返回强类型 Owner，供内部战斗逻辑使用。
	AfpstrueEnemyCharacter* GetEnemy() const;
	// 从骨骼 Socket 读取本帧武器根部和尖端位置。
	bool GetWeaponBladeSegment(FVector& OutBladeBase, FVector& OutBladeTip) const;
	// 对武器移动线段执行分段 Sweep，结果交给伤害去重。
	void SweepWeaponSegment(const FVector& TraceStart, const FVector& TraceEnd);
	// 对首次命中的有效目标施加伤害，并防止同一攻击重复命中。
	bool TryApplyAttackDamage(AActor* HitActor);
	// 关闭窗口并清空上一帧武器采样。
	void CancelAttackWindow();
	// 安排动画 Notify 缺失时的攻击结束保护 Timer。
	void ScheduleAttackFinish(float DurationSeconds);
	// 完成攻击并通知 AI 释放攻击名额。
	void FinishAttack();

	// 攻击事务状态集中在组件中，EnemyCharacter 不再并行维护窗口、命中集合和结束计时器。
	float LastAttackTime = 0.0f;
	bool bIsAttacking = false;
	bool bHitTargetThisAttack = false;
	bool bAttackWindowActive = false;
	bool bHasPreviousWeaponSample = false;
	bool bDisableAttackSweepForBenchmark = false;
	FVector PreviousWeaponBase = FVector::ZeroVector;
	FVector PreviousWeaponTip = FVector::ZeroVector;
	FTimerHandle AttackFinishTimerHandle;
};
