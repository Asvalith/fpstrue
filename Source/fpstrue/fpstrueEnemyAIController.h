// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "fpstrueEnemyAIController.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyCharacter;
class AfpstrueSurroundManager;

// 敌人玩法状态机；GameMode、CombatComponent 和 Animation Sharing 都读取这份唯一状态。
UENUM(BlueprintType)
enum class EFPEnemyAIState : uint8
{
	Idle UMETA(DisplayName = "Idle"),
	Chase UMETA(DisplayName = "Chase"),
	Attack UMETA(DisplayName = "Attack"),
	Dead UMETA(DisplayName = "Dead")
};

/** 敌人决策模块：用低频 Timer 驱动 Idle/Chase/Attack/Dead，并协调寻路、包围槽和攻击名额。 */
UCLASS()
class FPSTRUE_API AfpstrueEnemyAIController : public AAIController
{
	GENERATED_BODY()

public:
	// 创建无 Actor Tick 的 AIController。
	AfpstrueEnemyAIController();

	// 接管敌人 Pawn，缓存组件并启动决策循环。
	virtual void OnPossess(APawn* InPawn) override;
	// 失去 Pawn 前停止决策、移动并释放共享资源。
	virtual void OnUnPossess() override;
	// 世界退出时清理 Timer 和管理器引用。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// 外部统一停止 AI；敌人死亡和 GameMode 结束都会调用。
	void StopAI();

	// 由 GameMode 注入玩家目标和全局包围管理器，避免每个 AI 自行查找。
	void InitializeCombatContext(AfpstrueCharacter* NewTargetCharacter, AfpstrueSurroundManager* NewSurroundManager);

	// Benchmark 消融入口：单独关闭 PathFollowingComponent Tick。
	void ApplyBenchmarkPathFollowingTickOverride(bool bDisablePathFollowingTick);

	// Gameplay Significance 调整决策间隔，远处敌人减少 CPU 更新频率。
	void SetSignificanceDecisionMultiplier(float NewMultiplier);

	// 战斗目标只由 Controller 持有，EnemyCharacter 通过只读接口使用，避免双份状态漂移。
	AfpstrueCharacter* GetTargetCharacter() const { return TargetCharacter; }
	// Animation Sharing 等只读消费者通过这里获取当前 AI FSM 状态。
	EFPEnemyAIState GetAIState() const { return AIState; }
	// 攻击结束或 AI 停止时归还 SurroundManager 的攻击名额。
	void ReleaseAttackPermission();

protected:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI|Decision", meta = (ClampMin = "0.05"))
	float AttackDecisionInterval = 0.1f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI|Decision", meta = (ClampMin = "0.05"))
	float ChaseDecisionInterval = 0.25f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI|Decision", meta = (ClampMin = "0.05"))
	float FarDecisionInterval = 0.5f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI|Decision", meta = (ClampMin = "0.05"))
	float IdleDecisionInterval = 1.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI|Decision", meta = (ClampMin = "0.0"))
	float FarDecisionDistance = 3000.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "0.0"))
	float MoveAcceptanceRadius = 75.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "0.0"))
	float CombatMoveAcceptanceRadius = 15.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "25.0"))
	float PathRefreshDistance = 150.0f;

private:
	struct FDecisionContext
	{
		float DistanceSquared = MAX_flt;
		bool bInAttackRange = false;
		bool bInChaseRange = false;
	};

	// 启动第一次 AI 决策。
	void StartDecisionTimer();
	// 根据当前档位安排下一次一次性 Timer。
	void ScheduleNextDecision(float Delay);
	// 清除尚未执行的决策 Timer。
	void ClearDecisionTimer();
	// FSM 主入口：按优先级处理死亡、攻击、包围移动和追击。
	void UpdateAI();
	// 汇总目标距离与攻击/追击范围，供本轮决策复用。
	FDecisionContext BuildDecisionContext() const;
	// 根据状态、距离和 Significance 返回下一次决策间隔。
	float GetNextDecisionInterval(const FDecisionContext& Context) const;
	// 校验敌人、目标和管理器，失败时把 AI 恢复到安全状态。
	bool PrepareDecisionContext();
	// 维持正在进行的攻击，并阻止同一轮继续切换移动状态。
	bool HandleActiveAttack();
	// 尝试获取攻击位或包围槽，并向共享位置移动。
	bool HandleSurroundMovement();
	// 没有专属槽位时，使用共享目标快照进行低成本追击。
	bool HandleSharedPursuit();
	// 近战状态下让敌人朝向玩家目标。
	void UpdateFacingTarget();
	// 修改唯一 AI 状态，供决策和动画共享读取。
	void SetAIState(EFPEnemyAIState NewState);
	// 在目标变化足够大且通过预算时提交 MoveTo 请求。
	void MoveToGoal(const FVector& GoalLocation, float AcceptanceRadius, bool bCombatPriority);
	// 仅在确实移动时调用 StopMovement，避免重复请求。
	void StopMovementIfNeeded();
	// 归还当前敌人在 SurroundManager 中占用的槽位。
	void ReleaseSurroundSlot();
	// 向 SurroundManager 申请并发攻击名额。
	bool TryAcquireAttackPermission();
	// 返回 GameMode 注入的玩家目标，必要时执行一次安全解析。
	AfpstrueCharacter* ResolveTarget() const;
	// 返回共享包围管理器，供槽位和预算逻辑使用。
	AfpstrueSurroundManager* ResolveSurroundManager() const;
	// 判断目标是否有效、存活且可作为当前战斗对象。
	bool IsTargetUsable(const AfpstrueCharacter* Target) const;

	UPROPERTY()
	AfpstrueEnemyCharacter* ControlledEnemy = nullptr;

	UPROPERTY()
	AfpstrueCharacter* TargetCharacter = nullptr;

	UPROPERTY()
	AfpstrueSurroundManager* SurroundManager = nullptr;

	EFPEnemyAIState AIState = EFPEnemyAIState::Idle;
	FVector LastMoveGoal = FVector::ZeroVector;
	uint32 LastSharedTargetGeneration = 0;
	bool bHasMoveGoal = false;
	bool bDisableDecisionThrottlingForBenchmark = false;
	float SignificanceDecisionMultiplier = 1.0f;
	FTimerHandle DecisionTimerHandle;
};
