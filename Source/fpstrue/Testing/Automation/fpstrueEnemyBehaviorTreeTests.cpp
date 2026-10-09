// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Characters/Enemies/AI/fpstrueEnemyBehaviorTree.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BrainComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "InputAction.h"
#include "Kismet/GameplayStatics.h"
#include "Tests/AutomationCommon.h"
#include "Navigation/PathFollowingComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AIPerceptionSystem.h"
#include "Perception/AISense_Sight.h"
#include "UObject/UnrealType.h"

#if WITH_EDITOR
#include "BehaviorTreeGraphNode.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#endif

namespace
{
bool ReportBehaviorSightStimulus(AfpstrueEnemyAIController* Controller, AActor* Target, bool bSensed,
	const FVector& StimulusLocation, FAutomationTestBase& Test)
{
	UAIPerceptionComponent* Perception = Controller->GetPerceptionComponent();
	if (!Test.TestNotNull(TEXT("Behavior fixture has the real sight listener"), Perception)) return false;
	FAIStimulus Stimulus(*GetDefault<UAISense_Sight>(), bSensed ? 1.0f : 0.0f, StimulusLocation,
		Controller->GetPawn()->GetActorLocation(), bSensed ? FAIStimulus::SensingSucceeded : FAIStimulus::SensingFailed);
	// 重复失败也经过组件回调，才能验证 Controller 不会延长已经开始的记忆期限。
	Stimulus.SetWantsNotifyOnlyOnValueChange(false);
	Perception->RegisterStimulus(Target, Stimulus);
	Perception->ProcessStimuli();
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFPEnemyBehaviorTreeStructureTest, "fpstrue.AI.BehaviorTree.DefaultStructure",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFPEnemyBehaviorTreeStructureTest::RunTest(const FString& Parameters)
{
	UBehaviorTree* Tree = NewObject<UBehaviorTree>();
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(Tree);
	const bool bHadEngineSelfKey = Blackboard->GetKeyID(FBlackboard::KeySelf) != FBlackboard::InvalidKey;
	TestTrue(TEXT("Populate an empty editable asset"), UfpstrueEnemyBehaviorTreeLibrary::PopulateDefaultTree(Tree, Blackboard));
	TestEqual(TEXT("Engine default SelfActor key is preserved"), Blackboard->GetKeyID(FBlackboard::KeySelf) != FBlackboard::InvalidKey,
			  bHadEngineSelfKey);
	TestTrue(TEXT("Required blackboard keys have correct types"), FPHasEnemyBlackboardSchema(Blackboard));
	UBTComposite_Sequence* Root = Cast<UBTComposite_Sequence>(Tree->RootNode);
	if (!TestNotNull(TEXT("Decision cycle is a real Sequence"), Root))
		return false;
	if (!TestEqual(TEXT("Wait, sample, selector"), Root->Children.Num(), 3))
		return false;
	TestNotNull(TEXT("First node preserves adaptive waiting"), Cast<UfpstrueBTTask_DecisionWait>(Root->Children[0].ChildTask));
	TestNotNull(TEXT("Second node samples once"), Cast<UfpstrueBTTask_SampleDecision>(Root->Children[1].ChildTask));
	UBTComposite_Selector* Selector = Cast<UBTComposite_Selector>(Root->Children[2].ChildComposite);
	if (!TestNotNull(TEXT("Behavior selection belongs to Selector"), Selector))
		return false;
	if (!TestEqual(TEXT("Invalid target, attack, outside range, combat, surround, pursue"), Selector->Children.Num(), 6))
		return false;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		TestEqual(TEXT("Conditional branches are guarded by Blackboard"), Selector->Children[Index].Decorators.Num(), 1);
	}
	UBTComposite_Selector* Combat = Cast<UBTComposite_Selector>(Selector->Children[3].ChildComposite);
	if (!TestNotNull(TEXT("Combat fallback is another Selector"), Combat))
		return false;
	if (!TestEqual(TEXT("Attack attempt then position maintenance"), Combat->Children.Num(), 2))
		return false;
	const UfpstrueBTTask_EnemyAction* Attempt = Cast<UfpstrueBTTask_EnemyAction>(Combat->Children[0].ChildTask);
	const UfpstrueBTTask_EnemyAction* Fallback = Cast<UfpstrueBTTask_EnemyAction>(Combat->Children[1].ChildTask);
	TestTrue(TEXT("TryAttack is first"), Attempt != nullptr && Attempt->Action == EFPEnemyBehaviorAction::TryAttack);
	TestTrue(TEXT("Maintenance is fallback"), Fallback != nullptr && Fallback->Action == EFPEnemyBehaviorAction::MaintainCombatPosition);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFPEnemyBehaviorTreeAssetSafetyTest, "fpstrue.AI.BehaviorTree.AssetSafety",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFPEnemyBehaviorTreeAssetSafetyTest::RunTest(const FString& Parameters)
{
	UBehaviorTree* Tree = NewObject<UBehaviorTree>();
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(Tree);
	TestFalse(TEXT("Empty blackboard must not pass validation"), FPHasEnemyBlackboardSchema(Blackboard));
	TestFalse(TEXT("Null input is rejected"), UfpstrueEnemyBehaviorTreeLibrary::PopulateDefaultTree(nullptr, Blackboard));
	TestTrue(TEXT("Initial population succeeds"), UfpstrueEnemyBehaviorTreeLibrary::PopulateDefaultTree(Tree, Blackboard));
	UBTCompositeNode* OriginalRoot = Tree->RootNode;
	TestFalse(TEXT("Existing designer tree cannot be overwritten"),
			  UfpstrueEnemyBehaviorTreeLibrary::PopulateDefaultTree(Tree, Blackboard));
	TestTrue(TEXT("Designer root is preserved"), Tree->RootNode == OriginalRoot);
	UBehaviorTree* AnotherTree = NewObject<UBehaviorTree>();
	const UBlackboardKeyType* OriginalTargetKey = Blackboard->Keys[Blackboard->GetKeyID(FPEnemyBlackboard::TargetActor)].KeyType;
	TestTrue(TEXT("Another empty tree can reuse an unchanged default Blackboard"),
			 UfpstrueEnemyBehaviorTreeLibrary::PopulateDefaultTree(AnotherTree, Blackboard));
	TestTrue(TEXT("Reusing a Blackboard preserves original key instances"),
			 Blackboard->Keys[Blackboard->GetKeyID(FPEnemyBlackboard::TargetActor)].KeyType == OriginalTargetKey);
	UBlackboardKeyType_Object* TargetType = CastChecked<UBlackboardKeyType_Object>(
		Blackboard->Keys[Blackboard->GetKeyID(FPEnemyBlackboard::TargetActor)].KeyType);
	UClass* OriginalBaseClass = TargetType->BaseClass;
	TargetType->BaseClass = AfpstrueEnemyAIController::StaticClass();
	TestFalse(TEXT("Object key must accept the actual player class"), FPHasEnemyBlackboardSchema(Blackboard));
	TargetType->BaseClass = OriginalBaseClass;
	FBlackboardEntry& AttackEntry = Blackboard->Keys[Blackboard->GetKeyID(FPEnemyBlackboard::Attacking)];
	AttackEntry.bInstanceSynced = true;
	TestFalse(TEXT("Per-enemy attack state cannot be instance synchronized"), FPHasEnemyBlackboardSchema(Blackboard));
	AttackEntry.bInstanceSynced = false;
	TestTrue(TEXT("Restored independent schema is valid"), FPHasEnemyBlackboardSchema(Blackboard));
	Blackboard->Keys.RemoveAt(Blackboard->Keys.Num() - 1);
	TestFalse(TEXT("Missing required key is rejected"), FPHasEnemyBlackboardSchema(Blackboard));

	UBehaviorTree* UnconfiguredTree = NewObject<UBehaviorTree>();
	UBlackboardData* DesignerBlackboard = NewObject<UBlackboardData>(UnconfiguredTree);
	FBlackboardEntry& DesignerKey = DesignerBlackboard->Keys.AddDefaulted_GetRef();
	DesignerKey.EntryName = TEXT("DesignerAlert");
	DesignerKey.KeyType = NewObject<UBlackboardKeyType_Bool>(DesignerBlackboard);
	const int32 OriginalKeyCount = DesignerBlackboard->Keys.Num();
	TestFalse(TEXT("Existing designer key cannot be overwritten"),
			  UfpstrueEnemyBehaviorTreeLibrary::PopulateDefaultTree(UnconfiguredTree, DesignerBlackboard));
	TestEqual(TEXT("Rejected Blackboard retains all keys"), DesignerBlackboard->Keys.Num(), OriginalKeyCount);
	TestNull(TEXT("Rejected tree has not been partially populated"), UnconfiguredTree->RootNode.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFPEnemyBehaviorTreeLifecycleTest, "fpstrue.AI.BehaviorTree.RuntimeLifecycle",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFPEnemyBehaviorTreeLifecycleTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game))
	{
		Fixture.ForwardErrorMessages(this);
		return false;
	}
	UWorld* World = Fixture.GetTestWorld();
	// FTestWorldWrapper 的 Game 临时世界默认不创建 AI 系统，真实 Blackboard 初始化需要它。
	if (!TestNotNull(TEXT("Test world has an AI system"), World->CreateAISystem()))
		return false;
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld())
	{
		Fixture.ForwardErrorMessages(this);
		return false;
	}
	const auto SpawnEnemy = [World]()
	{
		AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
			AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Enemy != nullptr)
		{
			Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
			Enemy->FinishSpawning(FTransform::Identity);
			Enemy->SetActorEnableCollision(false);
			Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
		}
		return Enemy;
	};
	AfpstrueEnemyCharacter* FirstEnemy = SpawnEnemy();
	AfpstrueEnemyCharacter* SecondEnemy = SpawnEnemy();
	AfpstrueEnemyAIController* First = World->SpawnActor<AfpstrueEnemyAIController>();
	AfpstrueEnemyAIController* Second = World->SpawnActor<AfpstrueEnemyAIController>();
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("First enemy"), FirstEnemy) || !TestNotNull(TEXT("Second enemy"), SecondEnemy) ||
		!TestNotNull(TEXT("First controller"), First) || !TestNotNull(TEXT("Second controller"), Second) ||
		!TestNotNull(TEXT("Target"), Target))
		return false;
	Target->SetActorEnableCollision(false);
	Target->SetActorLocation(FVector(100000.0f, 0.0f, 0.0f));
	Target->GetCharacterMovement()->SetComponentTickEnabled(false);
	First->Possess(FirstEnemy);
	Second->Possess(SecondEnemy);
	First->InitializeCombatContext(Target, nullptr);
	if (!TestNotNull(TEXT("Real behavior brain"), First->GetBrainComponent()) ||
		!TestNotNull(TEXT("First Blackboard"), First->GetBlackboardComponent()) ||
		!TestNotNull(TEXT("Second Blackboard"), Second->GetBlackboardComponent()))
		return false;
	TestTrue(TEXT("Possess starts the behavior brain"), First->GetBrainComponent()->IsRunning());
	TestNull(TEXT("Combat context injection alone does not discover a player"), First->GetTargetCharacter());
	const float OriginalIdleInterval = First->IdleDecisionInterval;
	First->IdleDecisionInterval = 10.0f;
	First->RefreshBehaviorDecision(*First->GetBlackboardComponent());
	TestFalse(TEXT("Unseen injected player is absent from the decision snapshot"),
		First->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::HasTarget));
	TestFalse(TEXT("Unseen player cannot start an attack"), First->ExecuteBehaviorAction(EFPEnemyBehaviorAction::TryAttack));
	TestTrue(TEXT("Player affiliation is hostile before sight queries are built"),
		First->GetTeamAttitudeTowards(*Target) == ETeamAttitude::Hostile);
	TestTrue(TEXT("Other enemies are excluded from hostile sight queries"),
		First->GetTeamAttitudeTowards(*SecondEnemy) == ETeamAttitude::Neutral);
	for (int32 Frame = 0; Frame < 40; ++Frame)
	{
		if (!Fixture.TickTestWorld(0.01f))
		{
			Fixture.ForwardErrorMessages(this);
			return false;
		}
	}
	TestFalse(TEXT("The running tree also keeps an undiscovered player absent"),
		First->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::HasTarget));
	if (!ReportBehaviorSightStimulus(First, Target, true, Target->GetActorLocation(), *this)) return false;
	if (!Fixture.TickTestWorld(0.02f)) { Fixture.ForwardErrorMessages(this); return false; }
	TestTrue(TEXT("Sight message wakes a ten-second idle wait for the next tree sample"),
		First->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::HasTarget));
	First->IdleDecisionInterval = OriginalIdleInterval;
	TestTrue(TEXT("Actual tree sample populated target"),
			 First->GetBlackboardComponent()->GetValueAsObject(FPEnemyBlackboard::TargetActor) == Target);
	TestFalse(TEXT("Another controller does not inherit target"),
			  Second->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::HasTarget));
	TestFalse(TEXT("Far target chooses Idle branch"), First->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::InChaseRange));
	TestTrue(TEXT("Idle leaf executed"), First->GetAIState() == EFPEnemyAIState::Idle);

	AfpstrueSurroundManager* SharedManager = World->SpawnActor<AfpstrueSurroundManager>();
	if (!TestNotNull(TEXT("Search fixture has a live shared target manager"), SharedManager)) return false;
	First->InitializeCombatContext(Target, SharedManager);
	First->TargetMemorySeconds = 0.25f;
	Target->SetActorLocation(FVector(1200.0f, 0.0f, 0.0f));
	if (!ReportBehaviorSightStimulus(First, Target, true, Target->GetActorLocation(), *this)) return false;
	First->RefreshBehaviorDecision(*First->GetBlackboardComponent());
	const FVector RememberedLocation = First->LastKnownTargetLocation;
	const FVector HiddenLocation(300000.0f, 50000.0f, 0.0f);
	Target->SetActorLocation(HiddenLocation);
	First->RefreshBehaviorDecision(*First->GetBlackboardComponent());
	TestTrue(TEXT("Sampling before a new sight result cannot copy an unseen target movement"),
		First->LastKnownTargetLocation.Equals(RememberedLocation));
	SharedManager->SetTargetCharacter(Target);
	FVector SharedLocation;
	TestTrue(TEXT("Manager independently knows the hidden player's new position"),
		SharedManager->GetSharedTargetSnapshot(SharedLocation) && SharedLocation.Equals(HiddenLocation));
	if (!ReportBehaviorSightStimulus(First, Target, false, HiddenLocation, *this)) return false;
	const double OriginalForgetTime = First->TargetForgetTime;
	TestFalse(TEXT("Sight loss immediately revokes live target visibility"), First->bTargetVisible);
	TestNull(TEXT("Gameplay consumers cannot obtain the hidden player"), First->GetTargetCharacter());
	TestTrue(TEXT("Failure stimulus does not replace the last successful location"),
		First->LastKnownTargetLocation.Equals(RememberedLocation));
	First->RefreshBehaviorDecision(*First->GetBlackboardComponent());
	TestTrue(TEXT("Sight loss retains a bounded search decision"),
		First->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::HasTarget));
	TestNull(TEXT("Search snapshot does not expose the hidden player's Actor"),
		First->GetBlackboardComponent()->GetValueAsObject(FPEnemyBlackboard::TargetActor));
	TestFalse(TEXT("Search snapshot cannot reuse a previous attack-range decision"),
		First->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::InAttackRange));
	TestTrue(TEXT("Search distance is measured against the frozen location"),
		FMath::IsNearlyEqual(First->DecisionContext.DistanceSquared,
			static_cast<float>(FVector::DistSquared2D(FirstEnemy->GetActorLocation(), RememberedLocation))));
	for (const EFPEnemyBehaviorAction Action : {EFPEnemyBehaviorAction::TryAttack, EFPEnemyBehaviorAction::SustainAttack,
		EFPEnemyBehaviorAction::MaintainCombatPosition, EFPEnemyBehaviorAction::Surround})
	{
		TestFalse(TEXT("Hidden player cannot drive attacks or shared surround actions"), First->ExecuteBehaviorAction(Action));
	}
	First->SetControlRotation(FRotator(0.0f, 37.0f, 0.0f));
	const FRotator RotationBeforeSearch = First->GetControlRotation();
	TestFalse(TEXT("Hidden player cannot update the live facing direction"), First->UpdateFacingTarget());
	TestFalse(TEXT("Arriving at remembered position cannot face the hidden player"), First->FaceTargetAtRest());
	TestTrue(TEXT("Search preserves control rotation instead of looking at the live target"),
		First->GetControlRotation().Equals(RotationBeforeSearch));
	TestTrue(TEXT("Search can request pursuit of its remembered position"), First->ExecuteBehaviorAction(EFPEnemyBehaviorAction::Pursue));
	// 临时世界没有 NavMesh；真实寻路失败记录携带提交目标，可以区分记忆位置与 Manager 的实时位置。
	TestTrue(TEXT("Search navigation uses remembered location instead of Manager snapshot"),
		First->MoveFailures[0].Goal.Location.Equals(RememberedLocation));
	if (!Fixture.TickTestWorld(0.1f)) { Fixture.ForwardErrorMessages(this); return false; }
	if (!ReportBehaviorSightStimulus(First, Target, false, HiddenLocation + FVector(10000.0f, 0.0f, 0.0f), *this)) return false;
	TestEqual(TEXT("Repeated failure cannot renew the search deadline"), First->TargetForgetTime, OriginalForgetTime);
	TestTrue(TEXT("Repeated failure cannot move the search location"), First->LastKnownTargetLocation.Equals(RememberedLocation));
	if (!Fixture.TickTestWorld(0.2f)) { Fixture.ForwardErrorMessages(this); return false; }
	First->RefreshBehaviorDecision(*First->GetBlackboardComponent());
	TestFalse(TEXT("Expired memory removes the search decision"),
		First->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::HasTarget));
	TestFalse(TEXT("Expired memory cannot submit another pursuit"), First->ExecuteBehaviorAction(EFPEnemyBehaviorAction::Pursue));
	Target->SetActorLocation(FVector(1300.0f, 0.0f, 0.0f));
	if (!ReportBehaviorSightStimulus(First, Target, true, Target->GetActorLocation(), *this)) return false;
	First->RefreshBehaviorDecision(*First->GetBlackboardComponent());
	TestTrue(TEXT("Rediscovery restores the real target and its newly seen position"),
		First->GetTargetCharacter() == Target && First->LastKnownTargetLocation.Equals(Target->GetActorLocation()));
	TestEqual(TEXT("Rediscovery cancels the old memory deadline"), First->TargetForgetTime, 0.0);
	First->InitializeCombatContext(Target, nullptr);
	if (!ReportBehaviorSightStimulus(First, Target, true, Target->GetActorLocation(), *this)) return false;

	// 单轮采样由一个入口构建；目标失效时不能留下上轮的近战条件。
	Target->SetActorLocation(FVector(100.0f, 0.0f, 0.0f));
	if (!ReportBehaviorSightStimulus(First, Target, true, Target->GetActorLocation(), *this)) return false;
	First->RefreshBehaviorDecision(*First->GetBlackboardComponent());
	TestTrue(TEXT("Near target enters attack range"), First->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::InAttackRange));
	APlayerController* LocalPlayer = World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("A local player remains present while combat target is revoked"), LocalPlayer)) return false;
	for (const FName ActionName : {TEXT("MoveAction"), TEXT("LookAction"), TEXT("JumpAction"), TEXT("FireAction"),
		TEXT("AimAction"), TEXT("SprintAction"), TEXT("ReloadAction")})
	{
		FObjectPropertyBase* ActionProperty = FindFProperty<FObjectPropertyBase>(Target->GetClass(), ActionName);
		if (!TestNotNull(TEXT("The native local player has its input action property"), ActionProperty)) return false;
		ActionProperty->SetObjectPropertyValue_InContainer(Target, NewObject<UInputAction>(Target));
	}
	LocalPlayer->Possess(Target);
	First->AIState = EFPEnemyAIState::Chase;
	First->InitializeCombatContext(nullptr, nullptr);
	TestTrue(TEXT("Revoking a target immediately returns the enemy to Idle"), First->GetAIState() == EFPEnemyAIState::Idle);
	UBlackboardComponent* DecisionBlackboard = First->GetBlackboardComponent();
	const FBlackboard::FKey TargetKey = DecisionBlackboard->GetKeyID(FPEnemyBlackboard::TargetActor);
	const float ExpectedIdleDecisionDelay = FMath::Max(0.01f, First->IdleDecisionInterval * First->SignificanceDecisionMultiplier);
	int32 InvalidSnapshotNotifications = 0;
	const FDelegateHandle SnapshotObserver = DecisionBlackboard->RegisterObserver(TargetKey, First,
		FOnBlackboardChangeNotification::CreateLambda([&](const UBlackboardComponent& Blackboard, FBlackboard::FKey ChangedKey)
		{
			++InvalidSnapshotNotifications;
			TestEqual(TEXT("Target observer receives the changed target key"), ChangedKey, TargetKey);
			TestNull(TEXT("Observer sees the cleared target"), Blackboard.GetValueAsObject(FPEnemyBlackboard::TargetActor));
			for (const FName Key :
				 {FPEnemyBlackboard::HasTarget, FPEnemyBlackboard::Attacking, FPEnemyBlackboard::InAttackRange, FPEnemyBlackboard::InChaseRange})
			{
				TestFalse(TEXT("Observer sees every decision flag already cleared"), Blackboard.GetValueAsBool(Key));
			}
			TestEqual(TEXT("Idle interval is ready before observer notification"), First->GetBehaviorDecisionDelay(), ExpectedIdleDecisionDelay);
			return EBlackboardNotificationResult::ContinueObserving;
		}));
	First->RefreshBehaviorDecision(*First->GetBlackboardComponent());
	DecisionBlackboard->UnregisterObserver(TargetKey, SnapshotObserver);
	TestEqual(TEXT("Invalid refresh publishes one complete target snapshot"), InvalidSnapshotNotifications, 1);
	First->RefreshBehaviorDecision(*DecisionBlackboard);
	TestNull(TEXT("A later decision does not silently reacquire the local player"), First->GetTargetCharacter());
	TestFalse(TEXT("A local player alone cannot restore a revoked combat target"), DecisionBlackboard->GetValueAsBool(FPEnemyBlackboard::HasTarget));
	First->InitializeCombatContext(Target, nullptr);
	First->RefreshBehaviorDecision(*First->GetBlackboardComponent());
	TestFalse(TEXT("Reinjecting the same player starts an undiscovered context"),
		First->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::HasTarget));
	if (!ReportBehaviorSightStimulus(First, Target, true, Target->GetActorLocation(), *this)) return false;
	First->RefreshBehaviorDecision(*First->GetBlackboardComponent());
	TestTrue(TEXT("A later valid sample restores target"), First->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::HasTarget));
	UGameplayStatics::ApplyDamage(Target, 10000.0f, nullptr, nullptr, nullptr);
	if (!TestTrue(TEXT("The local target actually died"), Target->IsDead())) return false;
	First->RefreshBehaviorDecision(*DecisionBlackboard);
	TestNull(TEXT("A dead local player is removed from the combat context"), First->GetTargetCharacter());
	TestFalse(TEXT("Death clears the Blackboard target condition"), DecisionBlackboard->GetValueAsBool(FPEnemyBlackboard::HasTarget));
	TestTrue(TEXT("Target loss idles the enemy without permanently stopping its tree"),
		First->GetAIState() == EFPEnemyAIState::Idle && First->GetBrainComponent()->IsRunning());
	AfpstrueCharacter* Replacement = World->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("An explicitly injected replacement target exists"), Replacement)) return false;
	Replacement->SetActorEnableCollision(false);
	Replacement->SetActorLocation(FVector(100000.0f, 0.0f, 0.0f));
	Replacement->GetCharacterMovement()->SetComponentTickEnabled(false);
	First->InitializeCombatContext(Replacement, nullptr);
	if (!ReportBehaviorSightStimulus(First, Replacement, true, Replacement->GetActorLocation(), *this)) return false;
	First->RefreshBehaviorDecision(*DecisionBlackboard);
	TestTrue(TEXT("Sight discovery restores the explicitly injected replacement target"),
		DecisionBlackboard->GetValueAsObject(FPEnemyBlackboard::TargetActor) == Replacement);
	if (!TestTrue(TEXT("The replacement target is actually destroyed"), Replacement->Destroy())) return false;
	First->RefreshBehaviorDecision(*DecisionBlackboard);
	TestNull(TEXT("A destroyed target is removed from the combat context"), First->GetTargetCharacter());
	for (const FName Key :
		{FPEnemyBlackboard::HasTarget, FPEnemyBlackboard::Attacking, FPEnemyBlackboard::InAttackRange, FPEnemyBlackboard::InChaseRange})
	{
		TestFalse(TEXT("A destroyed target leaves no decision flags"), DecisionBlackboard->GetValueAsBool(Key));
	}
	AfpstrueEnemyAIController* UnpossessedController = World->SpawnActor<AfpstrueEnemyAIController>();
	if (!TestNotNull(TEXT("Unpossessed controller"), UnpossessedController)) return false;
	TestTrue(TEXT("Missing controlled pawn is an invalid move context"),
		UnpossessedController->MoveToGoal(FVector::ZeroVector, 15.0f, true) ==
		AfpstrueEnemyAIController::EMoveGoalResult::InvalidContext);
	AfpstrueCharacter* StoppedProbe = World->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("A queued sight event has a live target when AI stops"), StoppedProbe)) return false;
	StoppedProbe->SetActorEnableCollision(false);
	StoppedProbe->GetCharacterMovement()->SetComponentTickEnabled(false);
	First->InitializeCombatContext(StoppedProbe, nullptr);
	UAIPerceptionComponent* FirstPerception = First->GetPerceptionComponent();
	FAIStimulus QueuedSight(*GetDefault<UAISense_Sight>(), 1.0f, StoppedProbe->GetActorLocation(),
		FirstEnemy->GetActorLocation(), FAIStimulus::SensingSucceeded);
	FirstPerception->RegisterStimulus(StoppedProbe, QueuedSight);
	First->StopAI();
	// ForgetAll 不清空已排队刺激；真实组件仍派发它，停止状态必须拒绝恢复旧命令入口。
	FirstPerception->ProcessStimuli();
	TestFalse(TEXT("A late queued sight event cannot restore stopped visibility"), First->bTargetVisible);
	TestEqual(TEXT("A late queued sight event cannot create a stopped memory deadline"), First->TargetForgetTime, 0.0);
	TestTrue(TEXT("A late queued sight event cannot restore a stopped search location"), First->LastKnownTargetLocation.IsZero());
	TestFalse(TEXT("StopAI stops behavior brain"), First->GetBrainComponent()->IsRunning());
	TestNull(TEXT("StopAI clears blackboard target"), First->GetBlackboardComponent()->GetValueAsObject(FPEnemyBlackboard::TargetActor));
	for (const FName Key :
		 {FPEnemyBlackboard::HasTarget, FPEnemyBlackboard::Attacking, FPEnemyBlackboard::InAttackRange, FPEnemyBlackboard::InChaseRange})
	{
		TestFalse(TEXT("StopAI clears every decision flag"), First->GetBlackboardComponent()->GetValueAsBool(Key));
	}
	First->UnPossess();
	First->Possess(FirstEnemy);
	TestTrue(TEXT("A new Possess restarts behavior brain"), First->GetBrainComponent()->IsRunning());
	TestNull(TEXT("A new Possess does not retain old combat target"), First->GetTargetCharacter());
	TestNull(TEXT("Reused Blackboard does not retain old target"),
			 First->GetBlackboardComponent()->GetValueAsObject(FPEnemyBlackboard::TargetActor));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFPEnemySightOcclusionTest, "fpstrue.AI.Perception.SightOcclusionAndFacing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFPEnemySightOcclusionTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	if (!TestNotNull(TEXT("Sight test world has an AI system"), World->CreateAISystem())) return false;
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
		AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Real sight observer Pawn"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(FTransform::Identity);
	Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
	AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	AActor* Wall = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Real sight controller"), Controller) || !TestNotNull(TEXT("Real sight target"), Target)
		|| !TestNotNull(TEXT("Sight blocking actor"), Wall)) return false;
	Target->GetCharacterMovement()->SetComponentTickEnabled(false);
	Target->SetActorLocation(FVector(1000.0f, 0.0f, 0.0f));
	UBoxComponent* WallBox = NewObject<UBoxComponent>(Wall);
	Wall->AddInstanceComponent(WallBox);
	Wall->SetRootComponent(WallBox);
	WallBox->SetBoxExtent(FVector(40.0f, 300.0f, 300.0f));
	WallBox->SetCollisionResponseToAllChannels(ECR_Block);
	WallBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	WallBox->RegisterComponent();
	Wall->SetActorLocation(FVector(500.0f, 0.0f, 100.0f));
	Controller->Possess(Enemy);
	Controller->InitializeCombatContext(Target, nullptr);
	Controller->SetControlRotation(FRotator(0.0f, 180.0f, 0.0f));
	UAIPerceptionSystem* PerceptionSystem = UAIPerceptionSystem::GetCurrent(World);
	if (!TestNotNull(TEXT("Real sight system"), PerceptionSystem)) return false;
	PerceptionSystem->RegisterSourceForSenseClass(UAISense_Sight::StaticClass(), *Target);
	const auto TickUntilVisible = [&](bool bExpectedVisible)
	{
		for (int32 Frame = 0; Frame < 100; ++Frame)
		{
			if (!Fixture.TickTestWorld(0.02f)) { Fixture.ForwardErrorMessages(this); return false; }
			if ((Controller->GetTargetCharacter() == Target) == bExpectedVisible) return true;
		}
		return false;
	};
	// 不注入 Stimulus：真实 Sight 必须根据 Pawn 当前朝向、Visibility 线段和组件事件建立可见状态。
	if (!TestTrue(TEXT("Open sight uses the actual Pawn facing, not the controller's desired yaw"), TickUntilVisible(true))) return false;
	WallBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	if (!TestTrue(TEXT("A real blocking wall removes live target visibility"), TickUntilVisible(false))) return false;
	TestNull(TEXT("Wall-hidden Actor is unavailable to gameplay consumers"), Controller->GetTargetCharacter());
	WallBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	if (!TestTrue(TEXT("Removing the wall rediscovers the target"), TickUntilVisible(true))) return false;
	Enemy->SetActorRotation(FRotator(0.0f, 180.0f, 0.0f));
	if (!TestTrue(TEXT("Turning the actual Pawn away loses a target outside its sight cone"), TickUntilVisible(false))) return false;
	Enemy->SetActorRotation(FRotator::ZeroRotator);
	TestTrue(TEXT("Turning back rediscovers a target through the real sight system"), TickUntilVisible(true));
	Controller->StopAI();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFPEnemyMoveRequestOwnershipTest,
	"fpstrue.AI.BehaviorTree.MoveRequestOwnershipAndBackoff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFPEnemyMoveRequestOwnershipTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	World->CreateAISystem();
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	const FTransform SpawnTransform(FVector(1000.0, 0.0, 0.0));
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(),
		SpawnTransform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Enemy"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(SpawnTransform);
	AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	if (!TestNotNull(TEXT("Controller"), Controller)) return false;
	Controller->Possess(Enemy);
	Controller->GetBrainComponent()->StopLogic(TEXT("Move request fixture"));
	UPathFollowingComponent* Following = Controller->GetPathFollowingComponent();
	if (!TestNotNull(TEXT("Engine path-following component"), Following)) return false;
	int32 SubmittedCompletions = 0;
	FAIRequestID FirstFailedId = FAIRequestID::InvalidRequest;
	bool bStopDuringCompletion = false;
	const FDelegateHandle Observer = Following->OnRequestFinished.AddLambda([&](FAIRequestID Id, const FPathFollowingResult& Result)
	{
		++SubmittedCompletions;
		if (!FirstFailedId.IsValid() && Result.IsFailure()) FirstFailedId = Id;
		if (bStopDuringCompletion) Controller->StopAI();
	});
	using EMoveResult = AfpstrueEnemyAIController::EMoveGoalResult;
	const FVector Unreachable(5000.0, 0.0, 0.0);
	// 无 NavMesh 临时世界，真实 AAIController::MoveTo 会同步返回 Invalid 并分配真实请求 ID。
	// 不注入失败标记，也不借全局预算伪装退避：两个策略各可提交一次，之后各自等待。
	TestTrue(TEXT("Surround request fails in the real engine navigation entrypoint"),
		Controller->MoveToGoal(Unreachable, 15.0f, true) == EMoveResult::Unreachable);
	TestTrue(TEXT("Fallback pursue also receives a real navigation failure"),
		Controller->MoveToGoal(Unreachable, 75.0f, false) == EMoveResult::Unreachable);
	TestEqual(TEXT("Two strategies initially submit two real engine requests"), SubmittedCompletions, 2);
	for (int32 Decision = 0; Decision < 6; ++Decision)
	{
		Controller->MoveToGoal(Unreachable, 15.0f, true);
		Controller->MoveToGoal(Unreachable, 75.0f, false);
	}
	TestEqual(TEXT("Alternating fallback cannot overwrite the other strategy's backoff"), SubmittedCompletions, 2);
	const FVector OtherFloor = Unreachable + FVector(0.0, 0.0, 500.0);
	Controller->MoveToGoal(OtherFloor, 15.0f, true);
	TestEqual(TEXT("A different floor is a different navigation request"), SubmittedCompletions, 3);
	Controller->MoveToGoal(OtherFloor, 30.0f, true);
	TestEqual(TEXT("Changed acceptance radius is not suppressed by old failure"), SubmittedCompletions, 4);
	TestTrue(TEXT("Already-at-goal succeeds through the engine immediate-completion route"),
		Controller->MoveToGoal(Controller->GetNavAgentLocation(), 15.0f, true) == EMoveResult::Arrived);
	TestTrue(TEXT("Immediate success publishes an arrival cache after callbacks"), Controller->bHasMoveGoal);
	const auto ArrivalGoal = Controller->LastMoveGoal;
	Controller->OnMoveCompleted(FirstFailedId, FPathFollowingResult(EPathFollowingResult::Invalid));
	TestTrue(TEXT("Delayed old failure cannot invalidate newer arrival cache"), Controller->bHasMoveGoal);
	TestTrue(TEXT("Delayed completion preserves new goal semantics"), Controller->LastMoveGoal.Matches(ArrivalGoal, 1.0f));
	bStopDuringCompletion = true;
	Controller->MoveToGoal(Unreachable + FVector(1000.0, 0.0, 0.0), 15.0f, true);
	TestFalse(TEXT("StopAI inside synchronous completion closes command gate"), Controller->AcceptsCombatCommands());
	TestFalse(TEXT("Outer MoveTo return cannot restore stopped cache"), Controller->bHasMoveGoal);
	TestFalse(TEXT("Outer MoveTo return cannot restore stopped request ownership"), Controller->OwnedMoveRequestId.IsValid());
	Following->OnRequestFinished.Remove(Observer);
	return true;
}

#if WITH_EDITOR

namespace FPEnemyBehaviorAssetTests
{
const TCHAR* const Packages[] = {TEXT("/Game/FirstPerson/AI/BT_FPEnemy"), TEXT("/Game/FirstPerson/AI/BB_FPEnemy"), TEXT("/Game/FirstPerson/AI/BP_FPEnemyAIController"),
								 TEXT("/Game/FirstPerson/Blueprints/enemy/enemy_BP")};

UObject* ReadObjectProperty(const UObject* Object, FName Name)
{
	const FObjectPropertyBase* Property = Object != nullptr ? FindFProperty<FObjectPropertyBase>(Object->GetClass(), Name) : nullptr;
	return Property != nullptr ? Property->GetObjectPropertyValue_InContainer(Object) : nullptr;
}
} // namespace FPEnemyBehaviorAssetTests

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FFPEnemyBehaviorTreeAssetGraphTest, "fpstrue.AI.BehaviorTree.AssetGraphAndBindings",
								  EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FFPEnemyBehaviorTreeAssetGraphTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	// UE simple-test 返回 true 会被算作通过。缺少可选二进制资产时不枚举子测试，并明确记录 SKIP。
	// 一旦资产存在并枚举了测试，任何加载失败或图缺失都会真正失败，不能走 native fallback 蒙混验收。
	for (const TCHAR* Package : FPEnemyBehaviorAssetTests::Packages)
	{
		if (!FPackageName::DoesPackageExist(Package))
		{
			UE_LOG(
				LogTemp, Warning,
				TEXT("SKIP fpstrue.AI.BehaviorTree.AssetGraphAndBindings: asset package %s is absent; asset wiring/graph NOT validated."),
				Package);
			return;
		}
	}
	OutBeautifiedNames.Add(TEXT("DefaultProjectAssets"));
	OutTestCommands.Add(TEXT("DefaultProjectAssets"));
}

bool FFPEnemyBehaviorTreeAssetGraphTest::RunTest(const FString& Parameters)
{
	UBehaviorTree* Tree = LoadObject<UBehaviorTree>(nullptr, TEXT("/Game/FirstPerson/AI/BT_FPEnemy.BT_FPEnemy"));
	UBlackboardData* Data = LoadObject<UBlackboardData>(nullptr, TEXT("/Game/FirstPerson/AI/BB_FPEnemy.BB_FPEnemy"));
	UBlueprint* ControllerBlueprint = LoadObject<UBlueprint>(nullptr, TEXT("/Game/FirstPerson/AI/BP_FPEnemyAIController.BP_FPEnemyAIController"));
	UBlueprint* EnemyBlueprint = LoadObject<UBlueprint>(nullptr, TEXT("/Game/FirstPerson/Blueprints/enemy/enemy_BP.enemy_BP"));
	if (!TestNotNull(TEXT("Serialized BehaviorTree loads"), Tree) || !TestNotNull(TEXT("Serialized Blackboard loads"), Data) ||
		!TestNotNull(TEXT("Controller Blueprint loads"), ControllerBlueprint) ||
		!TestNotNull(TEXT("Enemy Blueprint loads"), EnemyBlueprint))
		return false;
	if (!TestNotNull(TEXT("Controller Blueprint is compiled"), ControllerBlueprint->GeneratedClass.Get()) ||
		!TestNotNull(TEXT("Enemy Blueprint is compiled"), EnemyBlueprint->GeneratedClass.Get()))
		return false;
	TestTrue(TEXT("Tree uses the saved Blackboard, not a transient fallback"), Tree->BlackboardAsset == Data);
	TestTrue(TEXT("Saved Blackboard schema is valid"), FPHasEnemyBlackboardSchema(Data));
	UObject* ControllerDefaults = ControllerBlueprint->GeneratedClass->GetDefaultObject();
	TestTrue(TEXT("Blueprint Controller defaults reference the saved BT"),
			 FPEnemyBehaviorAssetTests::ReadObjectProperty(ControllerDefaults, TEXT("BehaviorTreeAsset")) == Tree);
	const APawn* EnemyDefaults = Cast<APawn>(EnemyBlueprint->GeneratedClass->GetDefaultObject());
	if (!TestNotNull(TEXT("Enemy Blueprint derives from Pawn"), EnemyDefaults))
		return false;
	TestTrue(TEXT("Enemy defaults use the Blueprint Controller"),
			 EnemyDefaults->AIControllerClass.Get() == ControllerBlueprint->GeneratedClass.Get());
	if (!TestNotNull(TEXT("Saved runtime tree root exists"), Tree->RootNode.Get()))
		return false;
	UEdGraph* Graph = Tree->BTGraph;
	if (!TestNotNull(TEXT("Saved editable graph exists; open and save BT editor before running this test"), Graph))
		return false;
	if (!TestTrue(TEXT("Editable graph contains more than an empty Root"), Graph->Nodes.Num() > 1))
		return false;
	TestTrue(TEXT("Behavior graph is editable"), static_cast<bool>(Graph->bEditable));

	// 从真实运行树收集节点，然后与图中的 NodeInstance、引脚连线逐一对应。
	TSet<const UBTNode*> RuntimeNodes;
	TArray<const UBTNode*> PendingRuntime;
	PendingRuntime.Add(Tree->RootNode);
	while (!PendingRuntime.IsEmpty())
	{
		const UBTNode* Node = PendingRuntime.Pop(EAllowShrinking::No);
		if (Node == nullptr || RuntimeNodes.Contains(Node))
			continue;
		RuntimeNodes.Add(Node);
		TestFalse(TEXT("Saved runtime nodes are not transient"), Node->HasAnyFlags(RF_Transient));
		if (const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node))
		{
			for (const FBTCompositeChild& Child : Composite->Children)
			{
				PendingRuntime.Add(Child.ChildComposite != nullptr ? static_cast<const UBTNode*>(Child.ChildComposite.Get())
																   : static_cast<const UBTNode*>(Child.ChildTask.Get()));
			}
		}
	}
	TMap<const UBTNode*, const UEdGraphNode*> RuntimeToGraph;
	const UEdGraphNode* EditorRoot = nullptr;
	for (const UEdGraphNode* GraphNode : Graph->Nodes)
	{
		if (GraphNode == nullptr)
			continue;
		if (GraphNode->GetClass()->GetFName() == TEXT("BehaviorTreeGraphNode_Root"))
			EditorRoot = GraphNode;
		const UBTNode* RuntimeNode = Cast<UBTNode>(FPEnemyBehaviorAssetTests::ReadObjectProperty(GraphNode, TEXT("NodeInstance")));
		if (RuntimeNode != nullptr && RuntimeNodes.Contains(RuntimeNode))
			RuntimeToGraph.Add(RuntimeNode, GraphNode);
		for (const UEdGraphPin* Pin : GraphNode->Pins)
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Output)
				continue;
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (!TestNotNull(TEXT("Output link has an input endpoint"), Linked))
					return false;
				TestTrue(TEXT("Graph links connect to input pins"), Linked->Direction == EGPD_Input);
				TestTrue(TEXT("Graph links stay within this graph"), Linked->GetOwningNode()->GetGraph() == Graph);
				TestTrue(TEXT("Graph links are bidirectional"), Linked->LinkedTo.Contains(Pin));
			}
		}
	}
	if (!TestNotNull(TEXT("Editor Root node was restored"), EditorRoot))
		return false;
	TestEqual(TEXT("Every runtime node has a matching editable graph node"), RuntimeToGraph.Num(), RuntimeNodes.Num());
	const auto HasGraphEdge = [](const UEdGraphNode* Parent, const UEdGraphNode* Child)
	{
		if (Parent == nullptr || Child == nullptr)
			return false;
		for (const UEdGraphPin* Pin : Parent->Pins)
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Output)
				continue;
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked != nullptr && Linked->GetOwningNode() == Child)
					return true;
			}
		}
		return false;
	};
	TestTrue(TEXT("Editor Root is connected to the runtime root"), HasGraphEdge(EditorRoot, RuntimeToGraph.FindRef(Tree->RootNode)));
	for (const UBTNode* Node : RuntimeNodes)
	{
		if (const UBTCompositeNode* Composite = Cast<UBTCompositeNode>(Node))
		{
			for (const FBTCompositeChild& Child : Composite->Children)
			{
				const UBTNode* ChildNode = Child.ChildComposite != nullptr ? static_cast<const UBTNode*>(Child.ChildComposite.Get())
																		   : static_cast<const UBTNode*>(Child.ChildTask.Get());
				const UEdGraphNode* ParentGraph = RuntimeToGraph.FindRef(Node);
				const UEdGraphNode* ChildGraph = RuntimeToGraph.FindRef(ChildNode);
				const UBehaviorTreeGraphNode* BTChildGraph = Cast<UBehaviorTreeGraphNode>(ChildGraph);
				for (const UBTDecorator* Decorator : Child.Decorators)
				{
					TestTrue(TEXT("Saved decorator and its Blackboard condition share the editable SubNode instance"),
							 Decorator != nullptr && !Decorator->HasAnyFlags(RF_Transient) && BTChildGraph != nullptr &&
								 BTChildGraph->SubNodes.ContainsByPredicate(
									 [Decorator](const UAIGraphNode* SubNode)
									 { return SubNode != nullptr && SubNode->NodeInstance == Decorator; }));
				}
				const bool bConnected = HasGraphEdge(ParentGraph, ChildGraph);
				TestTrue(FString::Printf(TEXT("Runtime edge %s -> %s has an editable graph connection"), *GetNameSafe(Node),
										 *GetNameSafe(ChildNode)),
						 bConnected);
				if (!bConnected && ParentGraph != nullptr)
				{
					for (const UEdGraphPin* Pin : ParentGraph->Pins)
					{
						if (Pin == nullptr || Pin->Direction != EGPD_Output)
							continue;
						for (const UEdGraphPin* Linked : Pin->LinkedTo)
						{
							if (Linked == nullptr)
								continue;
							const UObject* LinkedInstance =
								FPEnemyBehaviorAssetTests::ReadObjectProperty(Linked->GetOwningNode(), TEXT("NodeInstance"));
							AddInfo(FString::Printf(TEXT("Graph output %s -> %s (node %s)"), *GetNameSafe(Node),
													*GetNameSafe(LinkedInstance), *GetNameSafe(Linked->GetOwningNode())));
						}
					}
				}
			}
		}
	}
	return true;
}

#endif // WITH_EDITOR

#endif
