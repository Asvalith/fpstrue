// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "fpstrueEnemyAIController.generated.h"

class AfpstrueCharacter;
class AfpstrueEnemyCharacter;
class AfpstrueSurroundManager;
class UBehaviorTree;
class UBlackboardComponent;
enum class EFPEnemyBehaviorAction : uint8;

// 当前行为的只读表现状态；行为选择由 BehaviorTree 拥有，Animation Sharing 只读消费。
UENUM(BlueprintType)
enum class EFPEnemyAIState : uint8
{
	Idle UMETA(DisplayName = "Idle"),
	Chase UMETA(DisplayName = "Chase"),
	Attack UMETA(DisplayName = "Attack"),
	Dead UMETA(DisplayName = "Dead")
};

/**
 * 敌人行为执行桥：BehaviorTree 选择行为，本类协调寻路、包围槽和攻击名额。
 *
 * 本类拥有单个敌人的目标、AIState、移动目标缓存和失败退避；跨敌人的槽位/预算归 SurroundManager，
 * 攻击窗口与伤害事务归 EnemyCombatComponent。Controller 不启用 Actor Tick；BT 用间隔任务降频。
 */
UCLASS()
class FPSTRUE_API AfpstrueEnemyAIController : public AAIController
{
	GENERATED_BODY()

public:
	// 高层决策不使用 Actor Tick；移动沿用 AAIController 的默认 PathFollowing，由角色 Movement 执行 RVO 避让。
	AfpstrueEnemyAIController();

	// 接管敌人 Pawn，缓存组件并启动决策循环。
	virtual void OnPossess(APawn* InPawn) override;
	// 失去 Pawn 前停止决策、移动并释放共享资源。
	virtual void OnUnPossess() override;
	// 世界退出时停止行为树并清理管理器引用。
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// 由 GameMode 注入玩家目标和全局包围管理器，避免每个 AI 自行查找。
	void InitializeCombatContext(AfpstrueCharacter* NewTargetCharacter, AfpstrueSurroundManager* NewSurroundManager);

	// Benchmark 消融入口：单独关闭 PathFollowingComponent Tick。
	void ApplyBenchmarkPathFollowingTickOverride(bool bDisablePathFollowingTick);

	// Gameplay Significance 调整决策间隔，远处敌人减少 CPU 更新频率。
	void SetSignificanceDecisionMultiplier(float NewMultiplier);

	// BT 采样节点的唯一入口，一轮距离和范围计算结果写入 Blackboard。
	void RefreshBehaviorDecision(UBlackboardComponent& DecisionBlackboard);
	// BT 叶子节点执行桥；不在 Controller 中再次选择高层行为。
	bool ExecuteBehaviorAction(EFPEnemyBehaviorAction Action);
	float GetBehaviorDecisionDelay() const { return NextBehaviorDecisionDelay; }

	// 战斗目标只由 Controller 持有，EnemyCharacter 通过只读接口使用，避免双份状态漂移。
	AfpstrueCharacter* GetTargetCharacter() const { return TargetCharacter; }
	// Animation Sharing 等只读消费者通过这里获取当前表现状态。
	EFPEnemyAIState GetAIState() const { return AIState; }

	// PathFollowing 失败时安排退避；成功到位后的朝向由下一次决策结合到达检测更新。
	virtual void OnMoveCompleted(FAIRequestID RequestID, const FPathFollowingResult& Result) override;
	// 外部统一停止 AI；敌人死亡和 GameMode 结束都会调用。
	void StopAI();
	// 攻击结束或 AI 停止时归还 SurroundManager 的攻击名额。
	void ReleaseAttackPermission();

protected:
	// 可替换为策划配置的行为树；未配置时使用原生默认树。自定义树需要同名 Blackboard 键。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI|Behavior Tree")
	TObjectPtr<UBehaviorTree> BehaviorTreeAsset;

	// 角色实际 Yaw 与目标方位的最大误差（度）；不是 Controller 的期望角度。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI|Rotation", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float AttackFacingToleranceDegrees = 15.0f;

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

	// 有效攻击范围的倍率；该范围内使用战斗响应间隔，默认保留原来的 1.5 倍。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI|Decision", meta = (ClampMin = "1.0", ClampMax = "10.0"))
	float CombatResponseRangeMultiplier = 1.5f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "0.0"))
	float MoveAcceptanceRadius = 75.0f;

	// 共享目标追击的接受距离 = Max(MoveAcceptanceRadius, 基础攻击范围 * 此倍率)。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PursuitAcceptanceRangeMultiplier = 0.8f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "0.0"))
	float CombatMoveAcceptanceRadius = 15.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "25.0"))
	float PathRefreshDistance = 75.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AI", meta = (ClampMin = "0.1"))
	float FailedMoveRetryDelay = 0.5f;

private:
	struct FDecisionContext
	{
		float DistanceSquared = MAX_flt;
		float EffectiveAttackRange = 0.0f;
		bool bInAttackRange = false;
		bool bInChaseRange = false;
	};

	// 启动唯一决策驱动；默认树和外部树不能与旧 Timer/FSM 并行运行。
	void StartBehaviorLogic();

	// 单轮采样：解析引用、计算快照和下轮间隔。
	// 校验并解析敌人、目标和管理器，再汇总距离与攻击/追击范围，供本轮决策复用。
	// 返回是否有有效目标；失败时清空采样，清理与停路统一由 Idle 叶子处理。
	bool BuildDecisionContext();
	// 根据状态、距离和 Significance 返回下一次决策间隔。
	float GetNextDecisionInterval(const FDecisionContext& Context) const;
	// 返回共享包围管理器，供槽位和预算逻辑使用。
	AfpstrueSurroundManager* ResolveSurroundManager() const;
	// 判断目标是否有效、存活且可作为当前战斗对象。
	bool IsTargetUsable(const AfpstrueCharacter* Target) const;

	// 叶子实现：不在这里重新建立一套高层行为优先级。
	// 维持正在进行的攻击，并阻止同一轮继续切换移动状态。
	bool HandleActiveAttack();
	// 冷却或未获攻击名额时共用：有槽位则继续走，无槽位则站定面向目标。
	void MaintainCombatPosition();
	// 尝试获取攻击位或包围槽，并向共享位置移动。
	bool HandleSurroundMovement();
	// 没有专属槽位时，使用共享目标快照进行低成本追击。
	void HandleSharedPursuit();

	// 移动与朝向：沿路径行进，到位后再面向目标。
	// 复用有效路径/到位缓存；需要刷新时先预算校验，再提交显式 MoveTo 请求并缓存投影结果。
	void MoveToGoal(const FVector& GoalLocation, float AcceptanceRadius, bool bCombatPriority);
	// 停止路径及地面残余移动，再切换到目标朝向；返回角色是否已转入攻击角度容差。
	bool FaceTargetAtRest();
	// 更新 Controller 的期望 Yaw；由 Movement 平滑旋转，并返回真实 Actor Yaw 是否对齐。
	bool UpdateFacingTarget();
	// 修改唯一 AI 状态，供决策和动画共享读取。
	void SetAIState(EFPEnemyAIState NewState);
	// Chase 行进时面向路径，到位后面向目标；到位不是新攻击事务，不新增一份 AI 状态。
	void ApplyRotationPolicy(EFPEnemyAIState NewState, bool bAtRestFacingTarget = false);

	// 公共清理：由停止入口和叶子按需调用，不重复维护移动缓存和群体资源。
	// 仅在确实移动时调用 StopMovement；攻击可保留已经提交的目标缓存。
	void StopMovementIfNeeded(bool bPreserveMoveGoal = false);
	// 归还当前敌人在 SurroundManager 中占用的槽位。
	void ReleaseSurroundSlot();

	// Possess 期间缓存敌人、目标与共享管理器，OnUnPossess 主动清空这些运行时引用。
	UPROPERTY(Transient)
	TObjectPtr<AfpstrueEnemyCharacter> ControlledEnemy;

	UPROPERTY(Transient)
	TObjectPtr<AfpstrueCharacter> TargetCharacter;

	UPROPERTY(Transient)
	TObjectPtr<AfpstrueSurroundManager> SurroundManager;

	// 持有原生默认树，防止世界内共享模板被 GC；每个 Controller 的运行状态仍相互独立。
	UPROPERTY(Transient)
	TObjectPtr<UBehaviorTree> ActiveBehaviorTree;

	EFPEnemyAIState AIState = EFPEnemyAIState::Idle;
	// 原始业务目标只用于请求去重；实际导航目标用于到达检查，避免导航投影偏移造成反复重寻路。
	FVector LastMoveGoal = FVector::ZeroVector;
	FVector LastResolvedMoveGoal = FVector::ZeroVector;
	float NextMoveRetryTime = 0.0f;
	bool bHasMoveGoal = false;
	bool bLastMoveGoalWasCombatPriority = false;
	bool bDisableDecisionThrottlingForBenchmark = false;
	float SignificanceDecisionMultiplier = 1.0f;
	FDecisionContext DecisionContext;
	float NextBehaviorDecisionDelay = 0.1f;
};
