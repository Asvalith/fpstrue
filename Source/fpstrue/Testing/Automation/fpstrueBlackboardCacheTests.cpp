// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/AI/fpstrueEnemyBehaviorTree.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Game/fpstrueMultiplayer.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BrainComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "InputAction.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense_Sight.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFPEnemyBlackboardKeyCacheTest, "fpstrue.AI.BehaviorTree.BlackboardKeyCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 通过真实 Possess/RunBehaviorTree 和感知发布观察结果，不用固定 KeyID 或直接写黑板模拟采样。
bool FFPEnemyBlackboardKeyCacheTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	World->GetWorldSettings()->DefaultGameMode = AfpstruePvpGameMode::StaticClass();
	if (!TestNotNull(TEXT("Cache fixture has the real AI system"), World->CreateAISystem())) return false;
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	auto* Mode = World->GetAuthGameMode<AfpstruePvpGameMode>();
	auto* Player = World->SpawnActor<APlayerController>();
	auto* Target = World->SpawnActor<AfpstrueCharacter>(FVector(800.0f, 0.0f, 0.0f), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Unique gameplay mode exists"), Mode) || !TestNotNull(TEXT("Ordinary PlayerController exists"), Player) ||
		!TestNotNull(TEXT("Perceived player exists"), Target)) return false;
	// 原生玩家没有蓝图输入资产；真实 Possess 初始化仍要求完整输入动作。
	for (const FName Name : {TEXT("MoveAction"), TEXT("LookAction"), TEXT("JumpAction"), TEXT("FireAction"),
		TEXT("AimAction"), TEXT("SprintAction"), TEXT("ReloadAction")})
	{
		FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Target->GetClass(), Name);
		if (!TestNotNull(TEXT("Native player exposes its required input action"), Property)) return false;
		Property->SetObjectPropertyValue_InContainer(Target, NewObject<UInputAction>(Target));
	}
	Player->Possess(Target);
	Target->SetActorEnableCollision(false);
	Target->GetCharacterMovement()->SetComponentTickEnabled(false);
	TestTrue(TEXT("A normal possessed PlayerController remains a combat participant"), Mode->IsCombatParticipant(Target));
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
		AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Cache fixture enemy exists"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(FTransform::Identity);
	Enemy->SetActorEnableCollision(false);
	Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
	auto* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	if (!TestNotNull(TEXT("Cache fixture controller exists"), Controller)) return false;
	UBehaviorTree* FullTree = NewObject<UBehaviorTree>(World);
	UBlackboardData* FullData = NewObject<UBlackboardData>(FullTree);
	if (!TestTrue(TEXT("Full native tree can be created"), UfpstrueEnemyBehaviorTreeLibrary::PopulateDefaultTree(FullTree, FullData))) return false;
	// 同类型交换检测写错 Bool，跨类型交换检测沿用旧资产的偏移/编号。
	FullData->Keys.Swap(FullData->GetKeyID(FPEnemyBlackboard::HasTarget), FullData->GetKeyID(FPEnemyBlackboard::Attacking));
	FullData->Keys.Swap(FullData->GetKeyID(FPEnemyBlackboard::TargetActor), FullData->GetKeyID(FPEnemyBlackboard::TacticalGoal));
	Controller->BehaviorTreeAsset = FullTree;
	Controller->Possess(Enemy);
	if (!TestTrue(TEXT("Possess starts the real tree with reordered keys"), Controller->IsBehaviorReady())) return false;
	UBrainComponent* Brain = Controller->GetBrainComponent();
	Brain->SetComponentTickEnabled(false);
	Brain->PrimaryComponentTick.bCanEverTick = false;
	UBlackboardComponent* Blackboard = Controller->GetBlackboardComponent();
	if (!TestNotNull(TEXT("Possess initializes a real Blackboard"), Blackboard)) return false;
	const auto SeeAndSample = [this, Controller, Target](UBlackboardComponent& Data)
	{
		FAIStimulus Sight(*GetDefault<UAISense_Sight>(), 1.0f, Target->GetActorLocation(),
			Controller->GetPawn()->GetActorLocation(), FAIStimulus::SensingSucceeded);
		Sight.SetWantsNotifyOnlyOnValueChange(false);
		Controller->GetPerceptionComponent()->RegisterStimulus(Target, Sight);
		Controller->GetPerceptionComponent()->ProcessStimuli();
		Controller->RefreshBehaviorDecision(Data);
		TestTrue(TEXT("Sight selects the ordinary player's identity"), Controller->GetTargetCharacter() == Target);
		TestTrue(TEXT("Object value reaches its actual reordered key"), Data.GetValueAsObject(FPEnemyBlackboard::TargetActor) == Target);
		TestTrue(TEXT("HasTarget reaches its actual reordered Bool"), Data.GetValueAsBool(FPEnemyBlackboard::HasTarget));
		TestFalse(TEXT("Other Bool values are not overwritten by HasTarget"), Data.GetValueAsBool(FPEnemyBlackboard::Attacking));
	};
	SeeAndSample(*Blackboard);
	TestTrue(TEXT("Cache binds the actual full Blackboard asset"), Controller->CachedDecisionBlackboardAsset.Get() == FullData);
	TestEqual(TEXT("Cached Object ID respects authoring order"), Controller->DecisionKeys.TargetActor, FullData->GetKeyID(FPEnemyBlackboard::TargetActor));
	TestEqual(TEXT("Cached Bool ID respects authoring order"), Controller->DecisionKeys.HasTarget, FullData->GetKeyID(FPEnemyBlackboard::HasTarget));
	Controller->StopAI();
	TestFalse(TEXT("Stop clears the cached Bool destination"), Blackboard->GetValueAsBool(FPEnemyBlackboard::HasTarget));
	TestNull(TEXT("Stop clears the cached Object destination"), Blackboard->GetValueAsObject(FPEnemyBlackboard::TargetActor));
	// 显式测试启动入口，不能把注入上下文当作重启。UseBlackboard 在此复用相同实例/资产。
	if (!TestTrue(TEXT("An explicit start reuses the compatible full schema"), Controller->StartBehaviorLogic())) return false;
	TestTrue(TEXT("Same-asset restart reuses its Blackboard component"), Controller->GetBlackboardComponent() == Blackboard);
	TestTrue(TEXT("Same-asset restart keeps the cache bound to the actual asset"), Controller->CachedDecisionBlackboardAsset.Get() == FullData);
	SeeAndSample(*Blackboard);
	UBehaviorTree* CompactTree = NewObject<UBehaviorTree>(World);
	UBlackboardData* CompactData = NewObject<UBlackboardData>(CompactTree);
	if (!TestTrue(TEXT("Compatible tree can be created"), UfpstrueEnemyBehaviorTreeLibrary::PopulateDefaultTree(CompactTree, CompactData))) return false;
	for (const FName Key : {FPEnemyBlackboard::TacticalRole, FPEnemyBlackboard::TacticalGoal, FPEnemyBlackboard::HasInnerSlot})
		CompactData->Keys.RemoveAt(CompactData->GetKeyID(Key));
	TestTrue(TEXT("Schema without optional tactical keys remains supported"), FPHasEnemyBlackboardSchema(CompactData));
	Controller->StopAI();
	Controller->BehaviorTreeAsset = CompactTree;
	if (!TestTrue(TEXT("Explicit start switches the running Blackboard schema"), Controller->StartBehaviorLogic())) return false;
	Blackboard = Controller->GetBlackboardComponent();
	TestTrue(TEXT("Cache follows the actual replacement asset"), Controller->CachedDecisionBlackboardAsset.Get() == CompactData);
	TestEqual(TEXT("Replacement Object ID is resolved again"), Controller->DecisionKeys.TargetActor, CompactData->GetKeyID(FPEnemyBlackboard::TargetActor));
	TestEqual(TEXT("Optional tactical ID is invalid after schema change"), Controller->DecisionKeys.TacticalRole, FBlackboard::InvalidKey);
	TestEqual(TEXT("Optional Goal ID is not retained from the previous asset"), Controller->DecisionKeys.TacticalGoal, FBlackboard::InvalidKey);
	TestEqual(TEXT("Optional lease mirror ID is not retained from the previous asset"), Controller->DecisionKeys.HasInnerSlot, FBlackboard::InvalidKey);
	SeeAndSample(*Blackboard);
	AfpstrueSurroundManager* Manager = Controller->GetSurroundManager();
	Controller->StopAI();
	Controller->InitializeCombatContext(Target, Manager);
	TestFalse(TEXT("Context delivery cannot restart a stopped tree"), Controller->IsBehaviorReady());
	TestFalse(TEXT("Context delivery cannot reopen the command phase"), Controller->AcceptsCombatCommands());
	TestNull(TEXT("Context delivery cannot restore the stopped target"), Controller->TargetCharacter.Get());
	Controller->RefreshBehaviorDecision(*Blackboard);
	TestFalse(TEXT("A late sample cannot republish stopped HasTarget"), Blackboard->GetValueAsBool(FPEnemyBlackboard::HasTarget));
	TestNull(TEXT("A late sample cannot republish the stopped target"), Blackboard->GetValueAsObject(FPEnemyBlackboard::TargetActor));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
