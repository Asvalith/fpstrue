// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Characters/Enemies/fpstrueEnemySignificance.h"
#include "fpstrueEnemyCharacter.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyAIController;
class AfpstrueEnemyCharacter;
class UfpstrueEnemyAnimationSharingCoordinator;
class UfpstrueEnemyCombatComponent;
class UfpstrueHealthComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEnemyDeathReported, AfpstrueEnemyCharacter*, DeadEnemy);

enum class EFPEnemySignificanceTier : uint8
{
	Full,
	Reduced,
	Background
};

/**
 * 敌人实体模块：组合 Health、Combat、移动和渲染分级，并桥接 AI、动画共享与蓝图表现。
 *
 * EnemyCharacter 不复制子系统状态：死亡查询来自 HealthComponent，攻击查询来自 CombatComponent，AI状态来自 Controller。
 * 本类主要负责 Actor/组件生命周期、受击死亡表现，以及把 Gameplay/Render Significance 结果应用到具体组件。
 */
UCLASS(Blueprintable)
class FPSTRUE_API AfpstrueEnemyCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	// 创建健康/战斗组件并设置基础骨骼动画优化。
	AfpstrueEnemyCharacter();

	// ==================== 战斗与玩法接口 ====================

	// 动画结束 Notify 通过角色入口通知 CombatComponent 完成攻击。
	UFUNCTION(BlueprintCallable, Category = "Combat")
	void HandleAttackFinishedNotify();

	// AttackWindow Notify 通过角色入口开启武器 Sweep。
	void BeginAttackWindow();

	// AttackWindow Notify 通过角色入口更新武器 Sweep。
	void UpdateAttackWindow();

	// AttackWindow Notify 通过角色入口结束武器 Sweep。
	void EndAttackWindow();

	// AI、GameMode 和 Significance 读取 HealthComponent 的死亡事实。
	bool IsDead() const;

	// AI FSM 判断攻击事务是否仍在进行。
	bool IsAttacking() const;

	// AI 与动画保护判断玩家是否已经进入有效攻击范围。
	bool IsTargetInAttackRange() const;

	// AIController 读取配置的追击范围。
	float GetChaseRange() const { return ChaseRange; }

	// AIController 读取基础攻击范围。
	float GetAttackRange() const;
	// CombatComponent 和 AI 使用包含碰撞体修正的实际攻击范围。
	float GetEffectiveAttackRange() const;

	// ==================== Benchmark 诊断接口 ====================

	// BenchmarkRunner 单独关闭攻击 Sweep、Pawn 碰撞或移动 Tick。
	void ApplyBenchmarkDiagnosticOverrides(bool bDisableAttackSweep, bool bDisablePawnCollision, bool bDisableCharacterMovementTick);

	// ==================== Render Significance 接口 ====================

	// Coordinator 调用：从相机视锥、屏占比和距离生成纯渲染样本。
	FFPEnemyRenderSignificanceSample EvaluateRenderSignificance(const FFPEnemyRenderViewContext& ViewContext,
																const FFPEnemyRenderSignificancePolicy& Policy);
	// Coordinator 调用：结合滞回和延迟得到自然 Render Tier。
	EFPEnemyRenderSignificanceTier ResolveNaturalRenderSignificanceTier(const FFPEnemyRenderSignificanceSample& Sample,
																		const FFPEnemyRenderSignificancePolicy& Policy);
	// Coordinator 调用：应用全局预算后的 LOD、动画、阴影和 RT 结果。
	void ApplyRenderSignificanceTier(EFPEnemyRenderSignificanceTier NewTier, bool bShouldCastShadow, bool bShouldBeVisibleInRayTracing,
									 bool bForceFullAnimationAndLOD, const FFPEnemyRenderSignificancePolicy& Policy);

	// CSV 统计读取当前 Gameplay Tier。
	EFPEnemySignificanceTier GetGameplaySignificanceTier() const { return SignificanceTier; }
	// CSV 统计与共享系统读取当前 Render Tier。
	EFPEnemyRenderSignificanceTier GetRenderSignificanceTier() const { return RenderSignificanceTier; }
	// Animation Sharing 用该分数控制 Leader Tick。
	float GetRenderSignificanceScore() const { return RenderSignificanceScore; }
	// CSV 统计读取最终应用到骨骼 Mesh 的最低 LOD。
	int32 GetAppliedMinimumLOD() const { return AppliedMinimumLOD; }
	// Coordinator 和共享系统检查攻击、范围及最近受击保护期。
	bool RequiresGameplayAnimationProtection(float CurrentTime, float GraceSeconds) const;
	// GameMode 注入同一 World 的共享协调器；弱引用只表达协作关系，不取得所有权。
	void SetAnimationSharingCoordinator(UfpstrueEnemyAnimationSharingCoordinator* InCoordinator);

	// ==================== 对外事件 ====================

	UPROPERTY(BlueprintAssignable, Category = "AI")
	FOnEnemyDeathReported OnEnemyDeathReported;

protected:
	// ==================== Actor 生命周期 ====================

	// 读取 Benchmark 开关、初始化移动/碰撞并注册 Gameplay Significance。
	virtual void BeginPlay() override;
	// 退出共享系统、Significance 和组件事件。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	// 在通用伤害处理前记录命中方向、位置和骨骼，供受击/死亡冲量使用。
	virtual float TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator,
							 AActor* DamageCauser) override;

	// HealthComponent 死亡事件入口：停止 AI、启用布娃娃并通知 GameMode。
	UFUNCTION()
	void HandleDeath();

	// HealthComponent 受伤事件入口：记录保护时间并播放受击反馈。
	UFUNCTION()
	void HandleDamageReceived(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);

	// ==================== 组件 ====================

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Health")
	TObjectPtr<UfpstrueHealthComponent> HealthComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat")
	TObjectPtr<UfpstrueEnemyCombatComponent> CombatComponent;

	// ==================== AI 与战斗配置 ====================

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float MoveSpeed = 500.0f;

	// 在敌人蓝图 Class Defaults 中配置；BeginPlay 写入 CharacterMovement 的 Yaw 转速（度/秒）。
	// AI 仍决定朝向来源，移动和站定面向目标都由 CharacterMovement 按此转速执行。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI|Rotation", meta = (ClampMin = "1.0", ClampMax = "3600.0", Units = "deg/s"))
	float MovementYawRotationRate = 540.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float ChaseRange = 10000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	float DestroyDelay = 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AI")
	bool bDestroyOnDeath = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Death|Physics", meta = (ClampMin = "0.0", ClampMax = "15000.0"))
	float DeathImpulseStrength = 10000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Death|Physics", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DeathImpulseUpwardBias = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat|Hit Reaction", meta = (ClampMin = "0.0", ClampMax = "1000.0"))
	float HitReactionImpulseStrength = 120.0f;

	// ==================== 性能分级配置 ====================

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance")
	bool bEnableMovementUpdateTiering = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float FullRateMovementDistance = 1500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float MidRateMovementDistance = 5000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float MidRateMovementTickInterval = 1.0f / 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float FarRateMovementTickInterval = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float MidRateAnimationTickInterval = 1.0f / 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "0.0"))
	float FarRateAnimationTickInterval = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "1.0"))
	float ReducedDecisionIntervalMultiplier = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Performance", meta = (ClampMin = "1.0"))
	float BackgroundDecisionIntervalMultiplier = 2.0f;

	// ==================== Blueprint 表现事件 ====================

	// 蓝图开始播放敌人攻击动画。
	UFUNCTION(BlueprintImplementableEvent, Category = "AI")
	void OnAttackStarted();

	// 蓝图播放敌人受伤表现。
	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnEnemyDamaged(float DamageAmount, AActor* DamageCauser, AController* InstigatedBy);

	// 蓝图播放敌人死亡表现。
	UFUNCTION(BlueprintImplementableEvent, Category = "Health")
	void OnEnemyDied();

private:
	// ==================== 受控协作者 ====================

	// 内部战斗与动画桥接仅开放给 Controller、共享协调器和 CombatComponent。
	friend class AfpstrueEnemyAIController;
	friend class UfpstrueEnemyAnimationSharingCoordinator;
	friend class UfpstrueEnemyCombatComponent;

	// ==================== 战斗、重要性与动画内部桥接 ====================

	// AIController 通过角色入口请求 CombatComponent 开始攻击。
	bool TryAttackTarget();
	// AIController 在申请攻击名额前检查距离、冷却和事务状态。
	bool CanStartAttack() const;
	// 从 AIController 读取唯一玩家目标，供战斗和距离判断使用。
	AfpstrueCharacter* GetCombatTarget() const;
	// 攻击前恢复全速动画/LOD，结束后重新应用性能档位。
	void SetAttackAnimationPriority(bool bHighPriority);
	// 向 UE Significance Manager 注册 Gameplay 距离评分回调。
	void RegisterWithSignificanceManager();
	// 死亡和 EndPlay 时注销 Gameplay Significance。
	void UnregisterFromSignificanceManager();
	// Significance Manager 回调：把距离分数转换为 Gameplay Tier。
	void ApplySignificance(float Significance);
	// 应用 Gameplay Tier，并把决策倍率传给 AIController。
	void ApplySignificanceTier(EFPEnemySignificanceTier NewTier);
	// 根据 Gameplay Tier 调整 CharacterMovement Tick 间隔。
	void ApplyGameplaySignificanceIntervals();
	// 把当前 Render Tier 和预算结果写入 SkeletalMeshComponent。
	void ApplyRenderSignificanceSettings();
	// 判断本敌人是否满足成为低风险 Animation Sharing Follower 的条件。
	bool CanUseAnimationSharing() const;
	// Render Tier 变化后让 Coordinator 刷新共享注册状态。
	void RefreshAnimationSharingRegistration();
	// 战斗、受击或死亡前立即退出 Animation Sharing。
	void SuspendAnimationSharing();
	// 根据最近伤害方向给 CharacterMovement 添加轻量受击冲量。
	void ApplyHitReactionImpulse();
	// 布娃娃启用后的下一帧施加死亡冲量。
	void ApplyDeathImpulse();

	// ==================== 运行时状态 ====================

	// HealthComponent 保存死亡事实；这里只记录死亡副作用是否已经执行。
	bool bDeathEffectsApplied = false;
	bool bDisableAnimationOptimizationsForBenchmark = false;
	bool bRegisteredWithSignificanceManager = false;
	EFPEnemySignificanceTier SignificanceTier = EFPEnemySignificanceTier::Full;
	EFPEnemyRenderSignificanceTier NaturalRenderSignificanceTier = EFPEnemyRenderSignificanceTier::Full;
	EFPEnemyRenderSignificanceTier RenderSignificanceTier = EFPEnemyRenderSignificanceTier::Full;
	EFPEnemyRenderSignificanceTier PendingRenderDemotionTier = EFPEnemyRenderSignificanceTier::Full;
	float RenderSignificanceScore = 1.0f;
	float LastPrimaryFrustumTime = -MAX_flt;
	float LastCombatRelevantTime = -MAX_flt;
	float LastNaturalRenderTierChangeTime = 0.0f;
	float PendingRenderDemotionStartTime = -MAX_flt;
	bool bRenderShouldCastShadow = true;
	bool bDisableEnemyShadowsForBenchmark = false;
	bool bDisableEnemyRayTracingForBenchmark = false;
	bool bHasRenderSignificancePolicy = false;
	bool bRenderShouldBeVisibleInRayTracing = true;
	bool bGameplayAnimationProtection = false;
	int32 AppliedMinimumLOD = INDEX_NONE;
	FFPEnemyRenderSignificancePolicy LastRenderSignificancePolicy;

	// 观察 GameMode 注入的共享协调器，不延长其生命周期。
	UPROPERTY(Transient)
	TWeakObjectPtr<UfpstrueEnemyAnimationSharingCoordinator> AnimationSharingCoordinator;

	FVector LastDamageDirection = FVector::ForwardVector;
	FVector LastDamageLocation = FVector::ZeroVector;
	FName LastDamageBoneName = NAME_None;
};
