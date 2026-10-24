// Copyright Epic Games, Inc. All Rights Reserved.

#include "Characters/Enemies/AI/fpstrueEnemyBehaviorTree.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "Engine/World.h"
#include "UObject/UnrealType.h"

#if WITH_EDITOR
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#endif

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

// 为原生分支配置 Bool 或 Object 的 Set/NotSet 条件，保持编辑器与运行字段一致。
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
// 输出阻止自动迁移的具体属性并返回 false，保留策划已有配置。
bool RejectEnemyTreeUpgradeProperty(const UBTNode& Node, const UBTNode& Default, const FProperty& Property)
{
	FString ActualValue, DefaultValue;
	Property.ExportText_InContainer(0, ActualValue, &Node, nullptr, const_cast<UBTNode*>(&Node), PPF_None);
	Property.ExportText_InContainer(0, DefaultValue, &Default, nullptr, const_cast<UBTNode*>(&Default), PPF_None);
	UE_LOG(LogTemp, Warning, TEXT("FPEnemyBTUpgrade rejected editable property: node=%s name=%s expectedName=%s property=%s actual=%s expected=%s"),
		*Node.GetPathName(), *Node.GetNodeName(), *Default.GetNodeName(), *Property.GetName(), *ActualValue, *DefaultValue);
	return false;
}
}

bool UfpstrueBTDecorator_DecisionFlag::HasSameCondition(const UfpstrueBTDecorator_DecisionFlag& Other) const
{
	for (TFieldIterator<FProperty> Property(GetClass()); Property; ++Property)
		if (Property->HasAnyPropertyFlags(CPF_Edit) && Property->GetFName() != TEXT("BlackboardKey") && !Property->Identical_InContainer(this, &Other)) return RejectEnemyTreeUpgradeProperty(*this, Other, **Property);
	const bool bMatches = GetSelectedBlackboardKey() == Other.GetSelectedBlackboardKey() && OperationType == Other.OperationType &&
		GetFlowAbortMode() == Other.GetFlowAbortMode() && IsInversed() == Other.IsInversed();
	if (!bMatches) UE_LOG(LogTemp, Warning, TEXT("FPEnemyBTUpgrade rejected condition: node=%s key=%s/%s operation=%u/%u abort=%d/%d inverse=%d/%d"),
		*GetPathName(), *GetSelectedBlackboardKey().ToString(), *Other.GetSelectedBlackboardKey().ToString(), OperationType, Other.OperationType,
		static_cast<int32>(GetFlowAbortMode()), static_cast<int32>(Other.GetFlowAbortMode()), IsInversed(), Other.IsInversed());
	return bMatches;
}

namespace
{
// 为默认黑板新增指定类型的独立键，不开启跨敌人的实例同步。
template <typename TKey> void AddKey(UBlackboardData& Blackboard, FName Name)
{
	FBlackboardEntry& Entry = Blackboard.Keys.AddDefaulted_GetRef();
	Entry.EntryName = Name;
	Entry.KeyType = NewObject<TKey>(&Blackboard);
}

// 将角色、目标点和内圈资格加入默认黑板；这里只建字段，不拥有实际战术分配。
void AddTacticalKeys(UBlackboardData& Blackboard)
{
	AddKey<UBlackboardKeyType_Int>(Blackboard, FPEnemyBlackboard::TacticalRole);
	AddKey<UBlackboardKeyType_Vector>(Blackboard, FPEnemyBlackboard::TacticalGoal);
	AddKey<UBlackboardKeyType_Bool>(Blackboard, FPEnemyBlackboard::HasInnerSlot);
}

// 将组合节点或任务挂到父节点，并返回可继续添加装饰器的子节点记录。
FBTCompositeChild& AddChild(UBTCompositeNode& Parent, UBTNode& Node)
{
	FBTCompositeChild& Child = Parent.Children.AddDefaulted_GetRef();
	Child.ChildComposite = Cast<UBTCompositeNode>(&Node);
	Child.ChildTask = Cast<UBTTaskNode>(&Node);
	return Child;
}

// 给一个子分支增加黑板 Set/NotSet 条件，装饰器对象归属于当前行为树。
void AddCondition(UBehaviorTree& Tree, FBTCompositeChild& Child, FName Key, bool bExpected)
{
	UfpstrueBTDecorator_DecisionFlag* Condition = NewObject<UfpstrueBTDecorator_DecisionFlag>(&Tree);
	Condition->Configure(Key, bExpected);
	Child.Decorators.Add(Condition);
}

// 创建指定动作的叶子节点并连接到父节点，返回分支用于追加条件。
FBTCompositeChild& AddAction(UBehaviorTree& Tree, UBTCompositeNode& Parent, EFPEnemyBehaviorAction Action, const TCHAR* Name)
{
	UfpstrueBTTask_EnemyAction* Task = NewObject<UfpstrueBTTask_EnemyAction>(&Tree, FName(Name));
	Task->Action = Action;
	Task->NodeName = Name;
	return AddChild(Parent, *Task);
}

// 只调整默认树的业务层次；原有叶子和条件实例继续由同一个 Controller 消费。
void GroupDefaultBranches(UBehaviorTree& Tree, UBTCompositeNode& Choice)
{
	const TArray<FBTCompositeChild> Flat = Choice.Children;
	check(Flat.Num() == 7);
	Choice.Children.Reset();
	const auto AddGroup = [&Tree, &Choice](UClass* Class, const TCHAR* Name, const TCHAR* Label)
	{
		UBTCompositeNode* Group = NewObject<UBTCompositeNode>(&Tree, Class, FName(Name));
		Group->NodeName = Label;
		AddChild(Choice, *Group);
		return Group;
	};
	const auto MoveGuardToGroup = [](const FBTCompositeChild& Branch, FBTCompositeChild& Group, UBTCompositeNode& Composite)
	{
		Group.Decorators = Branch.Decorators;
		Group.DecoratorOps = Branch.DecoratorOps;
		FBTCompositeChild Leaf = Branch;
		Leaf.Decorators.Reset();
		Leaf.DecoratorOps.Reset();
		Composite.Children.Add(MoveTemp(Leaf));
	};
	UBTCompositeNode* Retreat = AddGroup(UBTComposite_Sequence::StaticClass(), TEXT("RetreatBehavior"), TEXT("Retreat / cover"));
	MoveGuardToGroup(Flat[1], Choice.Children.Last(), *Retreat);
	UBTCompositeNode* Combat = AddGroup(UBTComposite_Selector::StaticClass(), TEXT("VisualCombat"), TEXT("Combat / visible target"));
	AddCondition(Tree, Choice.Children.Last(), FPEnemyBlackboard::TargetActor, true);
	for (int32 Index = 2; Index <= 5; ++Index) Combat->Children.Add(Flat[Index]);
	UBTCompositeNode* Investigation = AddGroup(UBTComposite_Selector::StaticClass(), TEXT("InvestigateLastKnownPosition"), TEXT("Investigation / last known position"));
	AddCondition(Tree, Choice.Children.Last(), FPEnemyBlackboard::HasTarget, true);
	AddCondition(Tree, Choice.Children.Last(), FPEnemyBlackboard::TargetActor, false);
	AddCondition(Tree, AddAction(Tree, *Investigation, EFPEnemyBehaviorAction::Idle, TEXT("OutsideInvestigationRange")),
		FPEnemyBlackboard::InChaseRange, false);
	Investigation->Children.Add(Flat[6]);
	UBTCompositeNode* Idle = AddGroup(UBTComposite_Sequence::StaticClass(), TEXT("IdleBehavior"), TEXT("Idle / search"));
	MoveGuardToGroup(Flat[0], Choice.Children.Last(), *Idle);
}

// 仅用于建立旧资产的严格比较模板，不向策划资产提供“任意树拍平”操作。
void FlattenDefaultBranches(UBTCompositeNode& Choice)
{
	const TArray<FBTCompositeChild> Groups = Choice.Children;
	check(Groups.Num() == 4);
	const UBTCompositeNode* Combat = Groups[1].ChildComposite;
	const auto Ungroup = [](const FBTCompositeChild& Group)
	{
		FBTCompositeChild Leaf = Group.ChildComposite->Children[0];
		Leaf.Decorators = Group.Decorators;
		Leaf.DecoratorOps = Group.DecoratorOps;
		return Leaf;
	};
	Choice.Children = {Ungroup(Groups[3]), Ungroup(Groups[0]), Combat->Children[0], Combat->Children[1],
		Combat->Children[2], Combat->Children[3], Groups[2].ChildComposite->Children[1]};
}

// 接受引擎的隐式 AND 或相同顺序的显式 AND；OR/NOT、重排或重复 Test 均不是默认规则。
bool HasDefaultDecoratorLogic(const FBTCompositeChild& Child)
{
	if (Child.DecoratorOps.IsEmpty()) return true;
	const int32 Count = Child.Decorators.Num();
	const int32 FirstTest = Count > 1 ? 1 : 0;
	if (Count == 0 || Child.DecoratorOps.Num() != Count + FirstTest) return false;
	if (FirstTest != 0 && (Child.DecoratorOps[0].Operation != EBTDecoratorLogic::And || Child.DecoratorOps[0].Number != Count)) return false;
	for (int32 Index = 0; Index < Count; ++Index)
		if (Child.DecoratorOps[FirstTest + Index].Operation != EBTDecoratorLogic::Test || Child.DecoratorOps[FirstTest + Index].Number != Index) return false;
	return true;
}

#if WITH_EDITOR
// 只有严格模板验证通过才重接图。保留运行节点实例，并按叶子宽度给业务组留出空间。
void RebuildDefaultGraph(UBehaviorTree& Tree, UBehaviorTreeGraph& Graph)
{
	Graph.LockUpdates();
	const TArray<TObjectPtr<UEdGraphNode>> OldNodes = Graph.Nodes;
	for (UEdGraphNode* Node : OldNodes)
	{
		for (UEdGraphPin* Pin : Node->Pins) if (Pin != nullptr) Pin->BreakAllPinLinks();
		if (!Node->IsA<UBehaviorTreeGraphNode_Root>()) Graph.RemoveNode(Node);
	}
	Graph.OnCreated();
	Graph.Initialize();
	TMap<const UBTNode*, UBehaviorTreeGraphNode*> GraphNodes;
	UBehaviorTreeGraphNode* EditorRoot = nullptr;
	for (UEdGraphNode* Raw : Graph.Nodes)
	{
		UBehaviorTreeGraphNode* Node = CastChecked<UBehaviorTreeGraphNode>(Raw);
		if (Node->IsA<UBehaviorTreeGraphNode_Root>()) EditorRoot = Node;
		else GraphNodes.Add(CastChecked<UBTNode>(Node->NodeInstance), Node);
	}
	int32 NextLeafX = 0;
	TFunction<int32(const UBTNode*, int32)> Layout = [&](const UBTNode* Runtime, int32 Depth)
	{
		UBehaviorTreeGraphNode* Node = GraphNodes.FindChecked(Runtime);
		const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Runtime);
		int32 FirstX = NextLeafX, LastX = NextLeafX;
		if (Composite != nullptr && !Composite->Children.IsEmpty())
		{
			for (int32 Index = 0; Index < Composite->Children.Num(); ++Index)
			{
				const int32 ChildX = Layout(Composite->GetChildNode(Index), Depth + 1);
				if (Index == 0) FirstX = ChildX;
				LastX = ChildX;
			}
		}
		else NextLeafX += 400;
		Node->NodePosX = (FirstX + LastX) / 2;
		Node->NodePosY = 180 + Depth * 240;
		return Node->NodePosX;
	};
	const int32 RootX = Layout(Tree.RootNode, 0);
	if (EditorRoot != nullptr) { EditorRoot->NodePosX = RootX; EditorRoot->NodePosY = 0; }
	Graph.UpdateClassData();
	Graph.UnlockUpdates();
}
#endif
} // namespace

// 复用本 World 的不可变默认树模板；运行黑板和节点内存仍由各 Controller 独立拥有。
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

// 仅给未配置的树生成默认决策链；已有树、图或不兼容黑板返回失败，避免覆盖策划编辑。
bool UfpstrueEnemyBehaviorTreeLibrary::PopulateDefaultTree(UBehaviorTree* Tree, UBlackboardData* Blackboard)
{
	if (Tree == nullptr || Blackboard == nullptr || Tree->RootNode != nullptr || Blackboard->Parent != nullptr)
	{
		return false;
	}
	// 区分可复用的完整默认 schema 与尚未配置的黑板，不把任意已有内容当作空白资产。
	const bool bReuseBlackboard = (Blackboard->Keys.Num() == 7 || Blackboard->Keys.Num() == 10) && Blackboard->GetKeyID(FBlackboard::KeySelf) != FBlackboard::InvalidKey &&
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
			continue;
		}
		const UBlackboardKeyType_Object* ObjectKey = Cast<UBlackboardKeyType_Object>(Entry.KeyType);
		if (Entry.EntryName != FBlackboard::KeySelf || ObjectKey == nullptr || ObjectKey->BaseClass != AActor::StaticClass() ||
			Entry.bInstanceSynced)
		{
			return false;
		}
	}
	// 只有未配置黑板才创建目标和决策条件；已匹配的黑板保持原键不变。
	if (!bReuseBlackboard)
	{
		AddKey<UBlackboardKeyType_Object>(*Blackboard, FPEnemyBlackboard::TargetActor);
		AddKey<UBlackboardKeyType_Bool>(*Blackboard, FPEnemyBlackboard::HasTarget);
		AddKey<UBlackboardKeyType_Bool>(*Blackboard, FPEnemyBlackboard::Attacking);
		AddKey<UBlackboardKeyType_Bool>(*Blackboard, FPEnemyBlackboard::InAttackRange);
		AddKey<UBlackboardKeyType_Bool>(*Blackboard, FPEnemyBlackboard::InChaseRange);
		AddKey<UBlackboardKeyType_Bool>(*Blackboard, FPEnemyBlackboard::Retreating);
		AddTacticalKeys(*Blackboard);
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

	// 先建立叶子和局部优先级，再将其归入撤退、可见战斗、位置调查、空闲四组。
	AddCondition(*Tree, AddAction(*Tree, *Choice, EFPEnemyBehaviorAction::Idle, TEXT("NoTargetIdle")), FPEnemyBlackboard::HasTarget, false);
	// 低血撤退先中断已有攻击，避免 SustainAttack 把伤害窗口一直维持到动画自然结束。
	AddCondition(*Tree, AddAction(*Tree, *Choice, EFPEnemyBehaviorAction::Retreat, TEXT("RetreatToCover")),
				 FPEnemyBlackboard::Retreating, true);
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
	GroupDefaultBranches(*Tree, *Choice);
#if WITH_EDITOR
	// 真实编辑器资产补建可编辑图；临时运行模板不创建编辑器图对象。
	if (Tree->IsAsset() && !Tree->HasAnyFlags(RF_Transient))
	{
		UBehaviorTreeGraph* Graph = CastChecked<UBehaviorTreeGraph>(FBlueprintEditorUtils::CreateNewGraph(
			Tree, TEXT("Behavior Tree"), UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass()));
		Tree->BTGraph = Graph;
		// AddSubNode 会调用 UpdateAsset；根尚未连通时重入会把后续节点作为孤儿移走。
		// 整张图连通后再解锁，由引擎一次重建运行树及执行索引，而非事后修改 Transient 标记。
		Graph->LockUpdates();
		Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
		RebuildDefaultGraph(*Tree, *Graph);
		Tree->MarkPackageDirty();
	}
#endif
	return true;
}

// 先严格确认已知默认模板，再升级业务分组及旧字段；任一自定义规则都会阻止迁移。
bool UfpstrueEnemyBehaviorTreeLibrary::UpgradeDefaultTreeForRetreat(UBehaviorTree* Tree, UBlackboardData* Blackboard)
{
	// 1. 验证上下文与黑板 schema，并生成只用于比较的默认参考树。
	const auto Reject = [Tree](const TCHAR* Stage, const FString& Detail = FString())
	{
		UE_LOG(LogTemp, Warning, TEXT("FPEnemyBTUpgrade rejected: tree=%s stage=%s %s"), *GetPathNameSafe(Tree), Stage, *Detail);
		return false;
	};
	if (Tree == nullptr || Blackboard == nullptr || Tree->BlackboardAsset != Blackboard || Blackboard->Parent != nullptr ||
		!Tree->RootDecorators.IsEmpty() || !Tree->RootDecoratorOps.IsEmpty() ||
		(Blackboard->Keys.Num() != 6 && Blackboard->Keys.Num() != 7 && Blackboard->Keys.Num() != 9 && Blackboard->Keys.Num() != 10))
		return Reject(TEXT("Context"), FString::Printf(TEXT("blackboard=%s treeBlackboard=%s parent=%s keys=%d rootDecorators=%d rootOps=%d"),
			*GetPathNameSafe(Blackboard), *GetPathNameSafe(Tree == nullptr ? nullptr : Tree->BlackboardAsset.Get()), *GetPathNameSafe(Blackboard == nullptr ? nullptr : Blackboard->Parent.Get()),
			Blackboard == nullptr ? -1 : Blackboard->Keys.Num(), Tree == nullptr ? -1 : Tree->RootDecorators.Num(), Tree == nullptr ? -1 : Tree->RootDecoratorOps.Num()));
	const bool bCurrent = Blackboard->GetKeyID(FPEnemyBlackboard::Retreating) != FBlackboard::InvalidKey;
	UBTCompositeNode* Root = Tree->RootNode;
	const auto HasClass = [](const UBTNode* Node, UClass* Class) { return Node != nullptr && Node->GetClass() == Class; };
	const bool bHistoricalRootOrder = Root != nullptr && Root->Children.Num() == 3 &&
		HasClass(Root->GetChildNode(0), UfpstrueBTTask_DecisionWait::StaticClass()) &&
		HasClass(Root->GetChildNode(1), UBTComposite_Selector::StaticClass()) &&
		HasClass(Root->GetChildNode(2), UfpstrueBTTask_SampleDecision::StaticClass());
	const UBTCompositeNode* ActualChoice = Root != nullptr && Root->Children.Num() == 3 ?
		Root->Children[bHistoricalRootOrder ? 1 : 2].ChildComposite.Get() : nullptr;
	const bool bGrouped = ActualChoice != nullptr && ActualChoice->Children.Num() == 4;
	// 只识别这一种已知坏签名；其余属性、节点、图映射仍须逐项与默认模板一致。
	const bool bMisplacedRetreat = !bGrouped && bCurrent && ActualChoice != nullptr && ActualChoice->Children.Num() == 7 &&
		ActualChoice->Children[1].Decorators.IsEmpty() && ActualChoice->Children[2].Decorators.Num() == 2;
	UBehaviorTree* Expected = NewObject<UBehaviorTree>(GetTransientPackage(), NAME_None, RF_Transient);
	UBlackboardData* ExpectedBlackboard = NewObject<UBlackboardData>(Expected);
	if (!PopulateDefaultTree(Expected, ExpectedBlackboard)) return Reject(TEXT("ReferenceBuild"));
	UBTCompositeNode* ExpectedChoice = Expected->RootNode->Children[2].ChildComposite;
	if (!bGrouped) FlattenDefaultBranches(*ExpectedChoice);
	if (!bCurrent) ExpectedChoice->Children.RemoveAt(1);
	if (bMisplacedRetreat)
	{
		ExpectedChoice->Children[2].Decorators.Insert(ExpectedChoice->Children[1].Decorators[0], 0);
		ExpectedChoice->Children[1].Decorators.Reset();
	}
	if (bHistoricalRootOrder) Expected->RootNode->Children.Swap(1, 2);
	TSet<FName> SeenKeys;
	for (const FBlackboardEntry& Entry : Blackboard->Keys)
	{
		const FBlackboardEntry* Default = ExpectedBlackboard->Keys.FindByPredicate([&Entry](const FBlackboardEntry& Key) { return Key.EntryName == Entry.EntryName; });
		if (Default == nullptr || SeenKeys.Contains(Entry.EntryName) || (!bCurrent && Entry.EntryName == FPEnemyBlackboard::Retreating) ||
			Entry.bInstanceSynced || Entry.KeyType == nullptr || Entry.KeyType->GetClass() != Default->KeyType->GetClass())
			return Reject(TEXT("BlackboardKey"), FString::Printf(TEXT("key=%s actualClass=%s expectedClass=%s synced=%d duplicate=%d"),
				*Entry.EntryName.ToString(), *GetNameSafe(Entry.KeyType == nullptr ? nullptr : Entry.KeyType->GetClass()),
				*GetNameSafe(Default == nullptr ? nullptr : Default->KeyType->GetClass()), Entry.bInstanceSynced, SeenKeys.Contains(Entry.EntryName)));
		SeenKeys.Add(Entry.EntryName);
		if (const UBlackboardKeyType_Object* Object = Cast<UBlackboardKeyType_Object>(Entry.KeyType))
			if (Object->BaseClass != CastChecked<UBlackboardKeyType_Object>(Default->KeyType)->BaseClass)
				return Reject(TEXT("BlackboardObjectBaseClass"), FString::Printf(TEXT("key=%s actual=%s expected=%s"), *Entry.EntryName.ToString(),
					*GetNameSafe(Object->BaseClass), *GetNameSafe(CastChecked<UBlackboardKeyType_Object>(Default->KeyType)->BaseClass)));
	}
	const bool bHasTacticalKeys = SeenKeys.Contains(FPEnemyBlackboard::TacticalRole);
	if (SeenKeys.Num() != (bCurrent ? 7 : 6) + (bHasTacticalKeys ? 3 : 0)) return Reject(TEXT("BlackboardKeySet"));
	for (const FBlackboardEntry& Entry : ExpectedBlackboard->Keys)
	{
		const bool bTactical = Entry.EntryName == FPEnemyBlackboard::TacticalRole || Entry.EntryName == FPEnemyBlackboard::TacticalGoal || Entry.EntryName == FPEnemyBlackboard::HasInnerSlot;
		if ((!bCurrent && Entry.EntryName == FPEnemyBlackboard::Retreating) || (!bHasTacticalKeys && bTactical)) continue;
		if (!SeenKeys.Contains(Entry.EntryName)) return Reject(TEXT("BlackboardMissingDefaultKey"), Entry.EntryName.ToString());
	}
	// 2. 校验运行节点、分支和可编辑属性，不在检查途中修改输入资产。
	TSet<const UBTNode*> Nodes;
	TMap<const UBTNode*, const FBTCompositeChild*> Branches;
	TFunction<bool(const UBTNode*, const UBTNode*)> Matches = [&](const UBTNode* Node, const UBTNode* Default)
	{
		if (Node == nullptr || Default == nullptr || Node->GetClass() != Default->GetClass() || Nodes.Contains(Node))
			return Reject(TEXT("RuntimeNodeClassOrDuplicate"), FString::Printf(TEXT("node=%s actualClass=%s expectedClass=%s duplicate=%d"),
				*GetPathNameSafe(Node), *GetNameSafe(Node == nullptr ? nullptr : Node->GetClass()), *GetNameSafe(Default == nullptr ? nullptr : Default->GetClass()), Nodes.Contains(Node)));
		for (TFieldIterator<FProperty> Property(Node->GetClass()); Property; ++Property)
			if (Property->HasAnyPropertyFlags(CPF_Edit) && Property->GetFName() != TEXT("BlackboardKey") && !Property->Identical_InContainer(Node, Default)) return RejectEnemyTreeUpgradeProperty(*Node, *Default, **Property);
		if (const UBTTaskNode* Task = Cast<UBTTaskNode>(Node); Task != nullptr && !Task->Services.IsEmpty()) return Reject(TEXT("RuntimeTaskServices"), Node->GetNodeName());
		Nodes.Add(Node);
		const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node);
		if (Composite == nullptr) return true;
		const UBTCompositeNode* DefaultComposite = CastChecked<UBTCompositeNode>(Default);
		if (!Composite->Services.IsEmpty() || Composite->Children.Num() != DefaultComposite->Children.Num())
			return Reject(TEXT("RuntimeCompositeStructure"), FString::Printf(TEXT("node=%s services=%d children=%d expectedChildren=%d"), *Node->GetNodeName(), Composite->Services.Num(), Composite->Children.Num(), DefaultComposite->Children.Num()));
		for (int32 Index = 0; Index < Composite->Children.Num(); ++Index)
		{
			const FBTCompositeChild& Child = Composite->Children[Index];
			const FBTCompositeChild& DefaultChild = DefaultComposite->Children[Index];
			if ((Child.ChildComposite != nullptr && Child.ChildTask != nullptr) || Child.Decorators.Num() != DefaultChild.Decorators.Num() ||
				!HasDefaultDecoratorLogic(Child))
				return Reject(TEXT("RuntimeBranchDecorators"), FString::Printf(TEXT("parent=%s index=%d decorators=%d expected=%d ops=%d"), *Node->GetNodeName(), Index, Child.Decorators.Num(), DefaultChild.Decorators.Num(), Child.DecoratorOps.Num()));
			for (int32 DecoratorIndex = 0; DecoratorIndex < Child.Decorators.Num(); ++DecoratorIndex)
			{
				const UfpstrueBTDecorator_DecisionFlag* Flag = Cast<UfpstrueBTDecorator_DecisionFlag>(Child.Decorators[DecoratorIndex]);
				if (Flag == nullptr || Flag->GetClass() != UfpstrueBTDecorator_DecisionFlag::StaticClass() || !Flag->HasSameCondition(*CastChecked<UfpstrueBTDecorator_DecisionFlag>(DefaultChild.Decorators[DecoratorIndex])))
					return Reject(TEXT("RuntimeCondition"), FString::Printf(TEXT("parent=%s branch=%d decorator=%d class=%s"), *Node->GetNodeName(), Index, DecoratorIndex, *GetNameSafe(Child.Decorators[DecoratorIndex] == nullptr ? nullptr : Child.Decorators[DecoratorIndex]->GetClass())));
			}
			const UBTNode* ChildNode = Composite->GetChildNode(Index);
			Branches.Add(ChildNode, &Child);
			if (!Matches(ChildNode, DefaultComposite->GetChildNode(Index))) return false;
		}
		return true;
	};
	if (!Matches(Tree->RootNode, Expected->RootNode)) return Reject(TEXT("RuntimeDefaultTemplate"));
#if WITH_EDITOR
	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Tree->BTGraph);
	// 3. 有编辑图时同时验证图与运行树的一一对应，避免只修运行结构而覆盖策划布局。
	TMap<const UBTNode*, UBehaviorTreeGraphNode*> GraphNodes;
	UBehaviorTreeGraphNode_Root* GraphRoot = nullptr;
	if ((Tree->IsAsset() && Graph == nullptr) || (Tree->BTGraph != nullptr && (Graph == nullptr || Graph->GetClass() != UBehaviorTreeGraph::StaticClass())))
		return Reject(TEXT("GraphClass"), GetNameSafe(Tree->BTGraph == nullptr ? nullptr : Tree->BTGraph->GetClass()));
	if (Graph != nullptr)
	{
		if (Graph->Nodes.Num() != Nodes.Num() + 1) return Reject(TEXT("GraphNodeCount"), FString::Printf(TEXT("actual=%d expected=%d"), Graph->Nodes.Num(), Nodes.Num() + 1));
		for (UEdGraphNode* Raw : Graph->Nodes)
		{
			UBehaviorTreeGraphNode* Node = Cast<UBehaviorTreeGraphNode>(Raw);
			if (Node == nullptr || !Node->Services.IsEmpty() || !Node->NodeComment.IsEmpty())
				return Reject(TEXT("GraphCustomNode"), FString::Printf(TEXT("node=%s class=%s services=%d comment=%s"), *GetPathNameSafe(Raw), *GetNameSafe(Raw == nullptr ? nullptr : Raw->GetClass()), Node == nullptr ? -1 : Node->Services.Num(), Node == nullptr ? TEXT("") : *Node->NodeComment));
			if (Node->GetClass() == UBehaviorTreeGraphNode_Root::StaticClass())
			{
				if (GraphRoot != nullptr || CastChecked<UBehaviorTreeGraphNode_Root>(Node)->BlackboardAsset != Blackboard || !Node->SubNodes.IsEmpty())
					return Reject(TEXT("GraphRoot"), FString::Printf(TEXT("duplicate=%d blackboard=%s subnodes=%d"), GraphRoot != nullptr, *GetPathNameSafe(CastChecked<UBehaviorTreeGraphNode_Root>(Node)->BlackboardAsset), Node->SubNodes.Num()));
				GraphRoot = CastChecked<UBehaviorTreeGraphNode_Root>(Node);
				continue;
			}
			const UBTNode* Runtime = Cast<UBTNode>(Node->NodeInstance);
			const FBTCompositeChild* Branch = Branches.FindRef(Runtime);
			const int32 DecoratorCount = Branch == nullptr ? 0 : Branch->Decorators.Num();
			if (!Nodes.Contains(Runtime) || GraphNodes.Contains(Runtime) || Node->Decorators.Num() != DecoratorCount || Node->SubNodes.Num() != DecoratorCount ||
				Node->GetClass() != (Cast<UBTCompositeNode>(Runtime) != nullptr ? UBehaviorTreeGraphNode_Composite::StaticClass() : UBehaviorTreeGraphNode_Task::StaticClass()))
				return Reject(TEXT("GraphRuntimeMapping"), FString::Printf(TEXT("graph=%s class=%s runtime=%s known=%d duplicate=%d decorators=%d expected=%d subnodes=%d"), *Node->GetPathName(), *Node->GetClass()->GetName(), *GetPathNameSafe(Runtime), Nodes.Contains(Runtime), GraphNodes.Contains(Runtime), Node->Decorators.Num(), DecoratorCount, Node->SubNodes.Num()));
			for (int32 Index = 0; Index < DecoratorCount; ++Index)
				if (Node->Decorators[Index] == nullptr || Node->Decorators[Index]->GetClass() != UBehaviorTreeGraphNode_Decorator::StaticClass() || Node->Decorators[Index]->NodeInstance != Branch->Decorators[Index] ||
					Node->SubNodes[Index] != Node->Decorators[Index] || !Node->Decorators[Index]->SubNodes.IsEmpty() || !Node->Decorators[Index]->Services.IsEmpty() || !Node->Decorators[Index]->Decorators.IsEmpty())
					return Reject(TEXT("GraphDecoratorMapping"), FString::Printf(TEXT("graph=%s index=%d actual=%s expected=%s"), *Node->GetPathName(), Index, *GetPathNameSafe(Node->Decorators[Index] == nullptr ? nullptr : Node->Decorators[Index]->NodeInstance.Get()), *GetPathNameSafe(Branch->Decorators[Index])));
			GraphNodes.Add(Runtime, Node);
		}
		if (GraphRoot == nullptr) return Reject(TEXT("GraphMissingRoot"));
		GraphNodes.Add(nullptr, GraphRoot);
		for (const auto& Pair : GraphNodes)
		{
			const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Pair.Key);
			TSet<const UBTNode*> Linked;
			for (const UEdGraphPin* Pin : Pair.Value->Pins)
				if (Pin != nullptr && Pin->Direction == EGPD_Output) for (const UEdGraphPin* Link : Pin->LinkedTo)
				{
					const UBehaviorTreeGraphNode* Child = Link == nullptr ? nullptr : Cast<UBehaviorTreeGraphNode>(Link->GetOwningNode());
					const UBTNode* ChildRuntime = Child == nullptr ? nullptr : Cast<UBTNode>(Child->NodeInstance);
					if (ChildRuntime == nullptr || Link->Direction != EGPD_Input || !Link->LinkedTo.Contains(Pin) || GraphNodes.FindRef(ChildRuntime) != Child || Linked.Contains(ChildRuntime))
						return Reject(TEXT("GraphPinMapping"), FString::Printf(TEXT("parent=%s pin=%s child=%s runtime=%s direction=%d reciprocal=%d duplicate=%d"), *Pair.Value->GetPathName(), *Pin->PinName.ToString(), *GetPathNameSafe(Child), *GetPathNameSafe(ChildRuntime), Link == nullptr ? -1 : static_cast<int32>(Link->Direction), Link != nullptr && Link->LinkedTo.Contains(Pin), Linked.Contains(ChildRuntime)));
					Linked.Add(ChildRuntime);
				}
			const int32 ExpectedLinks = Pair.Key == nullptr ? 1 : Composite == nullptr ? 0 : Composite->Children.Num();
			if (Linked.Num() != ExpectedLinks) return Reject(TEXT("GraphPinCount"), FString::Printf(TEXT("node=%s actual=%d expected=%d"), *Pair.Value->GetPathName(), Linked.Num(), ExpectedLinks));
			if (Pair.Key == nullptr && !Linked.Contains(Tree->RootNode)) return Reject(TEXT("GraphRootLink"));
			if (Composite != nullptr) for (int32 Index = 0; Index < Composite->Children.Num(); ++Index)
				if (!Linked.Contains(Composite->GetChildNode(Index))) return Reject(TEXT("GraphMissingBranchLink"), FString::Printf(TEXT("parent=%s child=%s index=%d"), *Composite->GetNodeName(), *GetPathNameSafe(Composite->GetChildNode(Index)), Index));
		}
		if (bHistoricalRootOrder && static_cast<int64>(GraphNodes.FindChecked(Root->GetChildNode(1))->NodePosX) - GraphNodes.FindChecked(Root->GetChildNode(0))->NodePosX < 2)
			return Reject(TEXT("HistoricalRootLayout"), TEXT("Wait and Choice must leave space for Sample between them."));
	}
#endif
	// 4. 所有校验通过后才提交修正；新分层树重复调用不替换节点或重建图。
	if (bHistoricalRootOrder)
	{
		Root->Children.Swap(1, 2);
#if WITH_EDITOR
		if (Graph != nullptr) GraphNodes.FindChecked(Root->GetChildNode(1))->NodePosX = static_cast<int32>(
			(static_cast<int64>(GraphNodes.FindChecked(Root->GetChildNode(0))->NodePosX) + GraphNodes.FindChecked(Root->GetChildNode(2))->NodePosX) / 2);
#endif
		Tree->MarkPackageDirty();
	}
	if (Blackboard->GetKeyID(FPEnemyBlackboard::TacticalRole) == FBlackboard::InvalidKey)
	{
		AddTacticalKeys(*Blackboard);
		Blackboard->MarkPackageDirty();
	}
	if (bGrouped)
	{
#if WITH_EDITOR
		if (bHistoricalRootOrder && Graph != nullptr) Graph->UpdateAsset();
#endif
		UE_LOG(LogTemp, Display, TEXT("FPEnemyBTUpgrade verified Grouped default tree: %s"), *Tree->GetPathName());
		return true;
	}
	UBTCompositeNode* Choice = Root->Children[2].ChildComposite;
	if (!bCurrent)
	{
		AddKey<UBlackboardKeyType_Bool>(*Blackboard, FPEnemyBlackboard::Retreating);
		FBTCompositeChild Retreat = AddAction(*Tree, *Choice, EFPEnemyBehaviorAction::Retreat, TEXT("RetreatToCover"));
		Choice->Children.Pop();
		Choice->Children.Insert(Retreat, 1);
		AddCondition(*Tree, Choice->Children[1], FPEnemyBlackboard::Retreating, true);
	}
	else if (bMisplacedRetreat)
	{
		Choice->Children[1].Decorators.Add(Choice->Children[2].Decorators[0]);
		Choice->Children[2].Decorators.RemoveAt(0);
		Choice->Children[1].DecoratorOps.Reset();
		Choice->Children[2].DecoratorOps.Reset();
		UE_LOG(LogTemp, Display, TEXT("FPEnemyBTUpgrade repaired the verified misplaced Retreating condition on %s"), *Tree->GetPathName());
	}
	GroupDefaultBranches(*Tree, *Choice);
#if WITH_EDITOR
	if (Graph != nullptr) RebuildDefaultGraph(*Tree, *Graph);
#endif
	UE_LOG(LogTemp, Display, TEXT("FPEnemyBTUpgrade upgraded %s default tree to Grouped: %s"),
		bMisplacedRetreat ? TEXT("Misplaced") : TEXT("Flat"), *Tree->GetPathName());
	Tree->MarkPackageDirty();
	Blackboard->MarkPackageDirty();
	return true;
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
	// 旧自定义树的七键契约仍兼容；战术镜像要么没有，要么完整且各实例独立。
	const FName TacticalNames[] = {FPEnemyBlackboard::TacticalRole, FPEnemyBlackboard::TacticalGoal, FPEnemyBlackboard::HasInnerSlot};
	UClass* TacticalTypes[] = {UBlackboardKeyType_Int::StaticClass(), UBlackboardKeyType_Vector::StaticClass(), UBlackboardKeyType_Bool::StaticClass()};
	int32 TacticalKeyCount = 0;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(TacticalNames); ++Index)
	{
		const FBlackboardEntry* Entry = Blackboard->GetKey(Blackboard->GetKeyID(TacticalNames[Index]));
		if (Entry == nullptr) continue;
		if (Entry->bInstanceSynced || Entry->KeyType == nullptr || Entry->KeyType->GetClass() != TacticalTypes[Index]) return false;
		++TacticalKeyCount;
	}
	if (TacticalKeyCount != 0 && TacticalKeyCount != UE_ARRAY_COUNT(TacticalNames)) return false;
	return true;
}
