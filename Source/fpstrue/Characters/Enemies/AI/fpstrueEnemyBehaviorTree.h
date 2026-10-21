// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "fpstrueEnemyBehaviorTree.generated.h"

class UBehaviorTree;
class UBlackboardData;

/**
 * 【设计取舍】共用树模板和无状态节点省重复配置；每个敌人的黑板、节点内存和事务仍独立。
 * 间隔采样与事件唤醒减少轮询，代价是存在决策等待；攻击提交由 Controller/Combat 再实时复核。
 * 树只选动作，Manager 授群体资格，Combat 结算伤害；黑板不是许可所有者，客户端不运行权威树。
 */
// 每个节点只执行一种行为；分支优先级由 Selector 和 Blackboard 条件决定。
UENUM(BlueprintType)
enum class EFPEnemyBehaviorAction : uint8
{
	Idle,
	SustainAttack,
	TryAttack,
	MaintainCombatPosition,
	Surround,
	Pursue,
	Retreat
};

namespace FPEnemyBlackboard
{
	inline const FName TargetActor(TEXT("TargetActor"));
	inline const FName HasTarget(TEXT("HasTarget"));
	inline const FName Attacking(TEXT("Attacking"));
	inline const FName InAttackRange(TEXT("InAttackRange"));
	inline const FName InChaseRange(TEXT("InChaseRange"));
	inline const FName Retreating(TEXT("Retreating"));
	inline const FName TacticalRole(TEXT("TacticalRole"));
	inline const FName TacticalGoal(TEXT("TacticalGoal"));
	inline const FName HasInnerSlot(TEXT("HasInnerSlot"));
}

inline const FName FPEnemyDecisionWake(TEXT("FPEnemyDecisionWake"));

/** 一轮只采样一次目标与距离；后续装饰器仅读 Blackboard，不重复查询 Actor。 */
UCLASS()
class FPSTRUE_API UfpstrueBTTask_SampleDecision : public UBTTaskNode
{
	GENERATED_BODY()
public:
	// 为编辑器中的单次采样节点设置可读名称。
	UfpstrueBTTask_SampleDecision();
	// 将 Controller 的单轮快照写入黑板；缺少 Controller 或黑板时返回失败。
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
};

/** 执行叶子行为，Controller 仍统一掌管 MoveTo 去重、预算、朝向和攻击许可。 */
UCLASS()
class FPSTRUE_API UfpstrueBTTask_EnemyAction : public UBTTaskNode
{
	GENERATED_BODY()
public:
	// 初始化叶子动作节点的显示名称，不保存某个敌人的运行状态。
	UfpstrueBTTask_EnemyAction();
	// 调用当前 Controller 执行已配置动作，并把执行结果交给行为树选择后备分支。
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;

	UPROPERTY(EditAnywhere, Category = "Enemy")
	EFPEnemyBehaviorAction Action = EFPEnemyBehaviorAction::Idle;
};

/** 行为树自身的间隔唤醒任务；没有额外 Controller Timer，也不逐帧重算决策。 */
UCLASS()
class FPSTRUE_API UfpstrueBTTask_DecisionWait : public UBTTaskNode
{
	GENERATED_BODY()
public:
	// 启用行为树的间隔 Tick，等待期间不重复采样距离和目标。
	UfpstrueBTTask_DecisionWait();
	// 按本敌人的决策间隔进入等待，也接受外部事件发来的提前唤醒消息。
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
protected:
	// 等待时间到达后完成潜伏任务，使行为树继续进入采样节点。
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
};

/** 标准 Blackboard bool 条件的原生构建入口；编辑器中仍可直接编辑 Blackboard 条件。 */
UCLASS()
class FPSTRUE_API UfpstrueBTDecorator_DecisionFlag : public UBTDecorator_Blackboard
{
	GENERATED_BODY()
public:
	// 设置黑板布尔条件及期望值，禁用等待期间基于旧快照的分支中止。
	void Configure(FName KeyName, bool bExpectedValue);
	// 默认树迁移前比较条件及可编辑属性；用户改过任一规则即拒绝自动覆盖。
	bool HasSameCondition(const UfpstrueBTDecorator_DecisionFlag& Other) const;
};

/** 初始化未配置树；创建默认 Blackboard 或复用完全匹配的已有键，供编辑器生成可编辑资产。 */
UCLASS()
class FPSTRUE_API UfpstrueEnemyBehaviorTreeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	// 拒绝覆盖已有树/图，不修改既有 Blackboard；日后策划编辑不会被重建抹掉。
	UFUNCTION(BlueprintCallable, Category = "Enemy|Behavior Tree")
	static bool PopulateDefaultTree(UBehaviorTree* Tree, UBlackboardData* Blackboard);
	// 仅升级未编辑的旧默认结构；新默认结构重复调用不改变节点或图。
	UFUNCTION(BlueprintCallable, Category = "Enemy|Behavior Tree")
	static bool UpgradeDefaultTreeForRetreat(UBehaviorTree* Tree, UBlackboardData* Blackboard);
};

// 世界内共享不可变树模板，每个 Controller 的 Blackboard 和节点内存由引擎单独分配。
UBehaviorTree* FPGetDefaultEnemyBehaviorTree(UWorld& World);
// 自定义树必须提供这些同类型键；避免拼写错误时静默运行错误的行为分支。
bool FPHasEnemyBlackboardSchema(const UBlackboardData* Blackboard);
