// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "fpstrueEnemyBehaviorTree.generated.h"

class UBehaviorTree;
class UBlackboardData;

// 每个节点只执行一种行为；分支优先级由 Selector 和 Blackboard 条件决定。
UENUM(BlueprintType)
enum class EFPEnemyBehaviorAction : uint8
{
	Idle,
	SustainAttack,
	TryAttack,
	MaintainCombatPosition,
	Surround,
	Pursue
};

namespace FPEnemyBlackboard
{
	inline const FName TargetActor(TEXT("TargetActor"));
	inline const FName HasTarget(TEXT("HasTarget"));
	inline const FName Attacking(TEXT("Attacking"));
	inline const FName InAttackRange(TEXT("InAttackRange"));
	inline const FName InChaseRange(TEXT("InChaseRange"));
}

/** 一轮只采样一次目标与距离；后续装饰器仅读 Blackboard，不重复查询 Actor。 */
UCLASS()
class FPSTRUE_API UfpstrueBTTask_SampleDecision : public UBTTaskNode
{
	GENERATED_BODY()
public:
	UfpstrueBTTask_SampleDecision();
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
};

/** 执行叶子行为，Controller 仍统一掌管 MoveTo 去重、预算、朝向和攻击许可。 */
UCLASS()
class FPSTRUE_API UfpstrueBTTask_EnemyAction : public UBTTaskNode
{
	GENERATED_BODY()
public:
	UfpstrueBTTask_EnemyAction();
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
	UfpstrueBTTask_DecisionWait();
	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
protected:
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
};

/** 标准 Blackboard bool 条件的原生构建入口；编辑器中仍可直接编辑 Blackboard 条件。 */
UCLASS()
class FPSTRUE_API UfpstrueBTDecorator_DecisionFlag : public UBTDecorator_Blackboard
{
	GENERATED_BODY()
public:
	void Configure(FName KeyName, bool bExpectedValue);
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
};

// 世界内共享不可变树模板，每个 Controller 的 Blackboard 和节点内存由引擎单独分配。
UBehaviorTree* FPGetDefaultEnemyBehaviorTree(UWorld& World);
// 自定义树必须提供这些同类型键；避免拼写错误时静默运行错误的行为分支。
bool FPHasEnemyBlackboardSchema(const UBlackboardData* Blackboard);
