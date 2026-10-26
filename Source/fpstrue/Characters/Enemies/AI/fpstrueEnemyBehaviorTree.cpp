// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/AI/fpstrueEnemyBehaviorTree.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"

// 初始化只采样一次的节点名称；每个敌人的数据保存在其 Controller 和黑板中。
UfpstrueBTTask_SampleDecision::UfpstrueBTTask_SampleDecision()
{
	NodeName = TEXT("Sample enemy decision once");
}

// 校验执行上下文后发布完整决策快照，成功则让后续装饰器消费同一轮数据。
EBTNodeResult::Type UfpstrueBTTask_SampleDecision::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AfpstrueEnemyAIController* Controller = Cast<AfpstrueEnemyAIController>(OwnerComp.GetAIOwner());
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (Controller == nullptr || Blackboard == nullptr)
	{
		return EBTNodeResult::Failed;
	}
	Controller->RefreshBehaviorDecision(*Blackboard);
	return EBTNodeResult::Succeeded;
}

// 初始化可配置叶子动作的名称，实际动作由 Action 字段决定。
UfpstrueBTTask_EnemyAction::UfpstrueBTTask_EnemyAction()
{
	NodeName = TEXT("Enemy action");
}

// 将高层选择交给行为树、执行细节交给 Controller；失败可触发 Selector 的后备分支。
EBTNodeResult::Type UfpstrueBTTask_EnemyAction::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AfpstrueEnemyAIController* Controller = Cast<AfpstrueEnemyAIController>(OwnerComp.GetAIOwner());
	return Controller != nullptr && Controller->ExecuteBehaviorAction(Action) ? EBTNodeResult::Succeeded : EBTNodeResult::Failed;
}

// 使用行为树自身的间隔更新，不增加另一套 Controller Timer 或逐帧决策循环。
UfpstrueBTTask_DecisionWait::UfpstrueBTTask_DecisionWait()
{
	NodeName = TEXT("Adaptive decision interval / initial stagger");
	bNotifyTick = true;
	bTickIntervals = true;
}

// 注册时间到达或消息唤醒两种出口；返回 InProgress 表示等待而不是当前帧反复执行。
EBTNodeResult::Type UfpstrueBTTask_DecisionWait::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	const AfpstrueEnemyAIController* Controller = Cast<AfpstrueEnemyAIController>(OwnerComp.GetAIOwner());
	// 每轮使用采样阶段根据最新距离和状态计算的间隔；首次执行则使用 Controller 的随机错峰延迟。
	// 无控制器时仍等待，避免错误树在同一帧无限失败重启。
	SetNextTickTime(NodeMemory, Controller != nullptr ? Controller->GetBehaviorDecisionDelay() : 1.0f);
	WaitForMessage(OwnerComp, FPEnemyDecisionWake);
	return EBTNodeResult::InProgress;
}

// 到期完成等待，下一节点再采样最新上下文；这些任务仍由 Game Thread 执行。
void UfpstrueBTTask_DecisionWait::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
}

// 验证目标对象类型、布尔键和独立实例语义；缺键或错误类型都不能运行项目的默认叶子逻辑。
bool FPHasEnemyBlackboardSchema(const UBlackboardData* Blackboard)
{
	if (Blackboard == nullptr)
	{
		return false;
	}
	const FBlackboardEntry* TargetEntry = Blackboard->GetKey(Blackboard->GetKeyID(FPEnemyBlackboard::TargetActor));
	const UBlackboardKeyType_Object* TargetType = TargetEntry != nullptr ? Cast<UBlackboardKeyType_Object>(TargetEntry->KeyType) : nullptr;
	if (TargetType == nullptr || TargetEntry->bInstanceSynced || TargetType->BaseClass == nullptr ||
		!AfpstrueCharacter::StaticClass()->IsChildOf(TargetType->BaseClass))
	{
		return false; // 类型必须能接受项目玩家；目标和决策条件不得跨敌人同步。
	}
	// 决策条件必须是各敌人独立的布尔键，防止一个敌人的采样覆盖其他敌人的行为。
	for (const FName Name :
		 {FPEnemyBlackboard::HasTarget, FPEnemyBlackboard::Attacking, FPEnemyBlackboard::InAttackRange, FPEnemyBlackboard::InChaseRange,
		  FPEnemyBlackboard::Retreating})
	{
		const FBlackboardEntry* Entry = Blackboard->GetKey(Blackboard->GetKeyID(Name));
		if (Entry == nullptr || Entry->bInstanceSynced || Entry->KeyType == nullptr ||
			Entry->KeyType->GetClass() != UBlackboardKeyType_Bool::StaticClass())
		{
			return false;
		}
	}
	// 正式树统一使用完整战术键，不再兼容缺失字段的旧黑板。
	const FName TacticalNames[] = {FPEnemyBlackboard::TacticalRole, FPEnemyBlackboard::TacticalGoal, FPEnemyBlackboard::HasInnerSlot};
	UClass* TacticalTypes[] = {UBlackboardKeyType_Int::StaticClass(), UBlackboardKeyType_Vector::StaticClass(), UBlackboardKeyType_Bool::StaticClass()};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(TacticalNames); ++Index)
	{
		const FBlackboardEntry* Entry = Blackboard->GetKey(Blackboard->GetKeyID(TacticalNames[Index]));
		if (Entry == nullptr || Entry->bInstanceSynced || Entry->KeyType == nullptr || Entry->KeyType->GetClass() != TacticalTypes[Index]) return false;
	}
	return true;
}
