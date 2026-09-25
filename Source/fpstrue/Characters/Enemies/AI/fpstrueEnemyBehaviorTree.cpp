// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/AI/fpstrueEnemyBehaviorTree.h"
#include "Characters/Enemies/fpstrueEnemyAIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "Engine/World.h"

#if WITH_EDITOR
#include "BehaviorTreeGraph.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#endif

UfpstrueBTTask_SampleDecision::UfpstrueBTTask_SampleDecision()
{
	NodeName = TEXT("Sample enemy decision once");
}

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

UfpstrueBTTask_EnemyAction::UfpstrueBTTask_EnemyAction()
{
	NodeName = TEXT("Enemy action");
}

EBTNodeResult::Type UfpstrueBTTask_EnemyAction::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AfpstrueEnemyAIController* Controller = Cast<AfpstrueEnemyAIController>(OwnerComp.GetAIOwner());
	return Controller != nullptr && Controller->ExecuteBehaviorAction(Action) ? EBTNodeResult::Succeeded : EBTNodeResult::Failed;
}

UfpstrueBTTask_DecisionWait::UfpstrueBTTask_DecisionWait()
{
	NodeName = TEXT("Adaptive decision interval / initial stagger");
	bNotifyTick = true;
	bTickIntervals = true;
}

EBTNodeResult::Type UfpstrueBTTask_DecisionWait::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	const AfpstrueEnemyAIController* Controller = Cast<AfpstrueEnemyAIController>(OwnerComp.GetAIOwner());
	// 每轮使用采样阶段根据最新距离和状态计算的间隔；首次执行则使用 Controller 的随机错峰延迟。
	// 无控制器时仍等待，避免错误树在同一帧无限失败重启。
	SetNextTickTime(NodeMemory, Controller != nullptr ? Controller->GetBehaviorDecisionDelay() : 1.0f);
	return EBTNodeResult::InProgress;
}

void UfpstrueBTTask_DecisionWait::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
}

void UfpstrueBTDecorator_DecisionFlag::Configure(FName KeyName, bool bExpectedValue)
{
	BlackboardKey.SelectedKeyName = KeyName;
	OperationType = bExpectedValue ? EBasicKeyOperation::Set : EBasicKeyOperation::NotSet;
#if WITH_EDITORONLY_DATA
	BasicOperation = bExpectedValue ? EBasicKeyOperation::Set : EBasicKeyOperation::NotSet;
#endif
	// 快照只在本轮采样时改变；不在等待期间对旧快照触发 Observer Abort。
	FlowAbortMode = EBTFlowAbortMode::None;
}

namespace
{
template <typename TKey> void AddKey(UBlackboardData& Blackboard, FName Name)
{
	FBlackboardEntry& Entry = Blackboard.Keys.AddDefaulted_GetRef();
	Entry.EntryName = Name;
	Entry.KeyType = NewObject<TKey>(&Blackboard);
}

FBTCompositeChild& AddChild(UBTCompositeNode& Parent, UBTNode& Node)
{
	FBTCompositeChild& Child = Parent.Children.AddDefaulted_GetRef();
	Child.ChildComposite = Cast<UBTCompositeNode>(&Node);
	Child.ChildTask = Cast<UBTTaskNode>(&Node);
	return Child;
}

void AddCondition(UBehaviorTree& Tree, FBTCompositeChild& Child, FName Key, bool bExpected)
{
	UfpstrueBTDecorator_DecisionFlag* Condition = NewObject<UfpstrueBTDecorator_DecisionFlag>(&Tree);
	Condition->Configure(Key, bExpected);
	Child.Decorators.Add(Condition);
}

FBTCompositeChild& AddAction(UBehaviorTree& Tree, UBTCompositeNode& Parent, EFPEnemyBehaviorAction Action, const TCHAR* Name)
{
	UfpstrueBTTask_EnemyAction* Task = NewObject<UfpstrueBTTask_EnemyAction>(&Tree, FName(Name));
	Task->Action = Action;
	Task->NodeName = Name;
	return AddChild(Parent, *Task);
}
} // namespace

UBehaviorTree* FPGetDefaultEnemyBehaviorTree(UWorld& World)
{
	static const FName TreeName(TEXT("FPEnemyDefaultBehaviorTree"));
	if (UBehaviorTree* Existing = FindObject<UBehaviorTree>(&World, *TreeName.ToString()))
	{
		return Existing;
	}

	UBehaviorTree* Tree = NewObject<UBehaviorTree>(&World, TreeName, RF_Transient);
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(Tree);
	return UfpstrueEnemyBehaviorTreeLibrary::PopulateDefaultTree(Tree, Blackboard) ? Tree : nullptr;
}

bool UfpstrueEnemyBehaviorTreeLibrary::PopulateDefaultTree(UBehaviorTree* Tree, UBlackboardData* Blackboard)
{
	if (Tree == nullptr || Blackboard == nullptr || Tree->RootNode != nullptr || Blackboard->Parent != nullptr)
	{
		return false;
	}
	const bool bReuseBlackboard = Blackboard->Keys.Num() == 6 && Blackboard->GetKeyID(FBlackboard::KeySelf) != FBlackboard::InvalidKey &&
								  FPHasEnemyBlackboardSchema(Blackboard);
	if (!bReuseBlackboard && Blackboard->Keys.Num() > 1)
	{
		return false;
	}
#if WITH_EDITOR
	if (Tree->BTGraph != nullptr || (Tree->IsAsset() && !Blackboard->IsAsset()))
	{
		return false;
	}
#endif
	// UE 的 PostInitProperties 会给新 Blackboard 加入 SelfActor；它不是用户配置。
	// 已有 schema 可以复用，但不重建或修改任何键；其余自定义 schema 拒绝初始化。
	for (const FBlackboardEntry& Entry : Blackboard->Keys)
	{
		if (bReuseBlackboard && Entry.EntryName != FBlackboard::KeySelf)
		{
			if (Entry.bInstanceSynced)
			{
				return false;
			}
			continue;
		}
		const UBlackboardKeyType_Object* ObjectKey = Cast<UBlackboardKeyType_Object>(Entry.KeyType);
		if (Entry.EntryName != FBlackboard::KeySelf || ObjectKey == nullptr || ObjectKey->BaseClass != AActor::StaticClass() ||
			Entry.bInstanceSynced)
		{
			return false;
		}
	}
	if (!bReuseBlackboard)
	{
		AddKey<UBlackboardKeyType_Object>(*Blackboard, FPEnemyBlackboard::TargetActor);
		AddKey<UBlackboardKeyType_Bool>(*Blackboard, FPEnemyBlackboard::HasTarget);
		AddKey<UBlackboardKeyType_Bool>(*Blackboard, FPEnemyBlackboard::Attacking);
		AddKey<UBlackboardKeyType_Bool>(*Blackboard, FPEnemyBlackboard::InAttackRange);
		AddKey<UBlackboardKeyType_Bool>(*Blackboard, FPEnemyBlackboard::InChaseRange);
	}
	Tree->BlackboardAsset = Blackboard;

	// 循环：等待（首次错峰）→ 一次采样 → 优先 Selector。
	// 叶子节点返回结果决定后备分支，Controller 不再维护第二套 UpdateAI 决策链。
	UBTComposite_Sequence* Root = NewObject<UBTComposite_Sequence>(Tree, TEXT("DecisionCycle"));
	Root->NodeName = TEXT("Decision cycle");
	Tree->RootNode = Root;
	AddChild(*Root, *NewObject<UfpstrueBTTask_DecisionWait>(Tree));
	AddChild(*Root, *NewObject<UfpstrueBTTask_SampleDecision>(Tree));
	UBTComposite_Selector* Choice = NewObject<UBTComposite_Selector>(Tree, TEXT("EnemyBehaviorPriority"));
	Choice->NodeName = TEXT("Enemy behavior priority");
	AddChild(*Root, *Choice);

	AddCondition(*Tree, AddAction(*Tree, *Choice, EFPEnemyBehaviorAction::Idle, TEXT("NoTargetIdle")), FPEnemyBlackboard::HasTarget, false);
	AddCondition(*Tree, AddAction(*Tree, *Choice, EFPEnemyBehaviorAction::SustainAttack, TEXT("SustainAttack")),
				 FPEnemyBlackboard::Attacking, true);
	AddCondition(*Tree, AddAction(*Tree, *Choice, EFPEnemyBehaviorAction::Idle, TEXT("OutsideChaseRange")), FPEnemyBlackboard::InChaseRange,
				 false);

	UBTComposite_Selector* Combat = NewObject<UBTComposite_Selector>(Tree, TEXT("AttackOrMaintainPosition"));
	Combat->NodeName = TEXT("Attack or maintain position");
	AddCondition(*Tree, AddChild(*Choice, *Combat), FPEnemyBlackboard::InAttackRange, true);
	AddAction(*Tree, *Combat, EFPEnemyBehaviorAction::TryAttack, TEXT("TurnAndTryAttack"));
	AddAction(*Tree, *Combat, EFPEnemyBehaviorAction::MaintainCombatPosition, TEXT("MaintainCombatPosition"));
	AddAction(*Tree, *Choice, EFPEnemyBehaviorAction::Surround, TEXT("MoveToSurroundSlot"));
	AddAction(*Tree, *Choice, EFPEnemyBehaviorAction::Pursue, TEXT("PursueSharedTarget"));
#if WITH_EDITOR
	if (Tree->IsAsset() && !Tree->HasAnyFlags(RF_Transient))
	{
		UBehaviorTreeGraph* Graph = CastChecked<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(
			Tree, TEXT("Behavior Tree"), UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass()));
		Tree->BTGraph = Graph;
		// AddSubNode 会调用 UpdateAsset；根尚未连通时重入会把后续节点作为孤儿移走。
		// 整张图连通后再解锁，由引擎一次重建运行树及执行索引，而非事后修改 Transient 标记。
		Graph->LockUpdates();
		Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
		Graph->OnCreated();
		Graph->Initialize();
		Graph->UpdateClassData();
		Graph->UnlockUpdates();
		Tree->MarkPackageDirty();
	}
#endif
	return true;
}

bool FPHasEnemyBlackboardSchema(const UBlackboardData* Blackboard)
{
	if (Blackboard == nullptr ||
		Blackboard->GetKeyType(Blackboard->GetKeyID(FPEnemyBlackboard::TargetActor)) != UBlackboardKeyType_Object::StaticClass())
	{
		return false;
	}
	for (const FName Name :
		 {FPEnemyBlackboard::HasTarget, FPEnemyBlackboard::Attacking, FPEnemyBlackboard::InAttackRange, FPEnemyBlackboard::InChaseRange})
	{
		if (Blackboard->GetKeyType(Blackboard->GetKeyID(Name)) != UBlackboardKeyType_Bool::StaticClass())
		{
			return false;
		}
	}
	return true;
}
