// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Characters/Enemies/AI/fpstrueEnemyBehaviorTree.h"
#include "Characters/Enemies/AI/fpstrueEnemyAIController.h"
#include "Characters/Enemies/AI/fpstrueSurroundManager.h"
#include "Characters/Enemies/AI/fpstrueEnemyPositionQuery.h"
#include "Characters/Enemies/fpstrueEnemyCharacter.h"
#include "Characters/Player/fpstrueCharacter.h"
#include "Game/fpstrueMultiplayer.h"
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
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "InputAction.h"
#include "Kismet/GameplayStatics.h"
#include "Tests/AutomationCommon.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationData.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AIPerceptionSystem.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISense_Hearing.h"
#include "Perception/AISense_Sight.h"
#include "UObject/UnrealType.h"
#include "UObject/StrongObjectPtr.h"
#include "Characters/Enemies/Performance/fpstrueEnemyPerformance.h"
#include "Characters/Enemies/fpstrueEnemyCombatComponent.h"
#include "Animation/AnimNotifyQueue.h"
#include "Animation/ActiveMontageInstanceScope.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "AnimationSharingManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "TimerManager.h"
#include "Misc/ScopeExit.h"
#include <limits>

#if WITH_EDITOR
#include "BehaviorTreeGraphNode.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#endif

// 行为决策、感知与寻路：先验证默认结构，再验证实际运行与资产接线。
namespace
{
// 保持真实树已启动且未暂停，只隔离后台采样。单独关 Enabled 会被 UE ScheduleNextTick 重新打开。
void DisableAutomaticBehaviorTick(UBrainComponent& Brain)
{
	Brain.SetComponentTickEnabled(false);
	Brain.PrimaryComponentTick.bCanEverTick = false;
}

// 经真实感知组件派发成功/失败刺激，保留回调链；只固定输入，不替代 Controller 的处理逻辑。
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

// 边界注入仍经过生产感知组件；与下方实际 ReportNoiseEvent 分发测试分开，不冒充真实听觉范围验收。
void ReportBehaviorHearingStimulus(AfpstrueEnemyAIController& Controller, AActor& Source, const FVector& Location,
	bool bSensed = true, FName Tag = TEXT("Weapon.Gunshot"))
{
	FAIStimulus Stimulus(*GetDefault<UAISense_Hearing>(), bSensed ? 1.0f : 0.0f, Location,
		Controller.GetPawn()->GetActorLocation(), bSensed ? FAIStimulus::SensingSucceeded : FAIStimulus::SensingFailed, Tag);
	Stimulus.SetWantsNotifyOnlyOnValueChange(false);
	Controller.GetPerceptionComponent()->RegisterStimulus(&Source, Stimulus);
	Controller.GetPerceptionComponent()->ProcessStimuli();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFPEnemyHearingInvestigationTest,
	"fpstrue.AI.Perception.AuthoritativeGunshotInvestigation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 真实原生 Hearing 分发验证墙后/距离，组件刺激验证合并、视觉优先、硬期限与暂停/死亡/Stop 资格。
bool FFPEnemyHearingInvestigationTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	if (!TestNotNull(TEXT("Hearing fixture creates the native AI system"), World->CreateAISystem())) return false;
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(),
		FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Hearing listener pawn"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(FTransform::Identity);
	Enemy->SetActorEnableCollision(false);
	Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
	AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	AActor* Wall = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Hearing controller"), Controller) || !TestNotNull(TEXT("Noise source"), Target) ||
		!TestNotNull(TEXT("Real occluder"), Wall)) return false;
	Target->SetActorEnableCollision(false);
	Target->GetCharacterMovement()->SetComponentTickEnabled(false);
	Target->SetActorLocation(FVector(1000.0f, 0.0f, 0.0f));
	UBoxComponent* WallBox = NewObject<UBoxComponent>(Wall);
	Wall->AddInstanceComponent(WallBox);
	Wall->SetRootComponent(WallBox);
	WallBox->SetBoxExtent(FVector(50.0f, 2000.0f, 2000.0f));
	WallBox->SetCollisionResponseToAllChannels(ECR_Block);
	WallBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	WallBox->RegisterComponent();
	Wall->SetActorLocation(FVector(300.0f, 0.0f, 0.0f));
	Controller->Possess(Enemy);
	Controller->InitializeCombatContext(Target, nullptr);
	if (!TestNotNull(TEXT("Real hearing brain"), Controller->GetBrainComponent())) return false;
	// ★【测试隔离边界】只关闭TickEnabled并不可靠：BT收到唤醒消息时ScheduleNextTick会自行重新启用。
	// 取消Brain TickFunction注册（不注销组件/不Pause/不Stop），保留真实感知和生产命令资格，
	// 防止无NavMesh临时世界的真实叶子提前结束调查；下方再显式执行该叶子验证寻路失败收口。
	Controller->GetBrainComponent()->PrimaryComponentTick.UnRegisterTickFunction();
	TestFalse(TEXT("Isolated lifetime test has no scheduler-registered Brain tick"),
		Controller->GetBrainComponent()->PrimaryComponentTick.IsTickFunctionRegistered());
	Controller->NoiseInvestigationSeconds = 0.35f;
	Controller->NoiseMergeInterval = 0.1f;
	Controller->NoiseInvestigationCooldown = 0.1f;
	UAIPerceptionSystem* Perception = UAIPerceptionSystem::GetCurrent(World);
	if (!TestNotNull(TEXT("Real perception event router"), Perception)) return false;
	Perception->RegisterSourceForSenseClass(UAISense_Sight::StaticClass(), *Target);
	const auto TickFor = [&](float Seconds)
	{
		for (int32 Frame = 0; Frame < FMath::CeilToInt(Seconds / 0.01f); ++Frame)
			if (!Fixture.TickTestWorld(0.01f)) { Fixture.ForwardErrorMessages(this); return false; }
		return true;
	};
	if (!TickFor(0.05f)) return false;
	const UAISenseConfig_Hearing* HearingConfig = Cast<UAISenseConfig_Hearing>(Controller->GetPerceptionComponent()->GetSenseConfig(
		UAISense::GetSenseID<UAISense_Hearing>()));
	TestTrue(TEXT("The real listener has bounded native Hearing configured"), HearingConfig != nullptr && HearingConfig->HearingRange == 4500.0f);
	TestNull(TEXT("Real wall keeps the source outside combat visibility"), Controller->GetTargetCharacter());
	TestFalse(TEXT("Null source cannot submit gunshot semantics"), AfpstrueEnemyAIController::ReportAuthoritativeGunshot(nullptr, FVector::ZeroVector));
	TestFalse(TEXT("Invalid location cannot submit gunshot semantics"), AfpstrueEnemyAIController::ReportAuthoritativeGunshot(Target, FAISystem::InvalidLocation));
	TestFalse(TEXT("Silent or malformed input cannot submit gunshot semantics"), AfpstrueEnemyAIController::ReportAuthoritativeGunshot(Target, FVector::ZeroVector, 0.0f));
	Target->SetRole(ROLE_SimulatedProxy);
	TestFalse(TEXT("A non-authoritative source cannot report predicted gunfire"), AfpstrueEnemyAIController::ReportAuthoritativeGunshot(Target, Target->GetActorLocation()));
	Target->SetRole(ROLE_Authority);
	TestTrue(TEXT("An authoritative out-of-range noise is accepted by the router"),
		AfpstrueEnemyAIController::ReportAuthoritativeGunshot(Target, FVector(7000.0f, 0.0f, 0.0f)));
	if (!TickFor(0.05f)) return false;
	TestFalse(TEXT("The actual Hearing range excludes an out-of-range event"), Controller->bInvestigatingNoise);
	const FVector HeardLocation(650.0f, 0.0f, 0.0f);
	TestTrue(TEXT("One authoritative event enters the real Hearing system"),
		AfpstrueEnemyAIController::ReportAuthoritativeGunshot(Target, HeardLocation, 1.0f, 1000.0f));
	if (!TickFor(0.05f)) return false;
	if (!TestTrue(TEXT("Native Hearing can begin a bounded investigation through a real wall"), Controller->bInvestigatingNoise)) return false;
	TestFalse(TEXT("Hearing does not grant sight"), Controller->bTargetVisible);
	TestTrue(TEXT("Memory stores the noise position, not source transform"), Controller->LastKnownTargetLocation.Equals(HeardLocation));
	TestNull(TEXT("Hearing cannot expose a combat target"), Controller->GetTargetCharacter());
	const double OriginalDeadline = Controller->TargetForgetTime;
	ReportBehaviorHearingStimulus(*Controller, *Target, HeardLocation + FVector(20.0f, 0.0f, 0.0f));
	TestTrue(TEXT("Nearby repeated gunfire coalesces without moving the goal"), Controller->LastKnownTargetLocation.Equals(HeardLocation));
	TestEqual(TEXT("Repeated gunfire cannot renew the hard deadline"), Controller->TargetForgetTime, OriginalDeadline);
	Target->SetActorLocation(FVector(12000.0f, 9000.0f, 0.0f));
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	TestTrue(TEXT("An unheard source movement cannot update the frozen investigation"), Controller->LastKnownTargetLocation.Equals(HeardLocation));
	TestTrue(TEXT("Investigation reuses the bounded search branch"), Controller->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::HasTarget));
	TestNull(TEXT("The search Blackboard does not leak the hidden actor"), Controller->GetBlackboardComponent()->GetValueAsObject(FPEnemyBlackboard::TargetActor));
	for (const EFPEnemyBehaviorAction Action : {EFPEnemyBehaviorAction::TryAttack, EFPEnemyBehaviorAction::SustainAttack,
		EFPEnemyBehaviorAction::MaintainCombatPosition, EFPEnemyBehaviorAction::Surround})
		TestFalse(TEXT("Hearing alone cannot attack or request hidden surround slots"), Controller->ExecuteBehaviorAction(Action));
	if (!TickFor(0.11f)) return false;
	const FVector MovedNoise(1000.0f, 0.0f, 0.0f);
	ReportBehaviorHearingStimulus(*Controller, *Target, MovedNoise);
	TestTrue(TEXT("A later spatially distinct event can refresh the fixed noise goal"), Controller->LastKnownTargetLocation.Equals(MovedNoise));
	TestEqual(TEXT("Goal refresh still cannot extend investigation lifetime"), Controller->TargetForgetTime, OriginalDeadline);
	ReportBehaviorHearingStimulus(*Controller, *Target, FVector(2000.0f, 0.0f, 0.0f), false);
	TestTrue(TEXT("A failed hearing stimulus cannot move or erase valid memory"), Controller->LastKnownTargetLocation.Equals(MovedNoise));
	if (!TickFor(0.3f)) return false;
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	TestFalse(TEXT("The existing decision sample expires the investigation without a new timer"), Controller->bInvestigatingNoise);
	TestFalse(TEXT("Expired sound leaves no valid search decision"), Controller->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::HasTarget));
	const double CompletionCooldown = Controller->NextNoiseInvestigationTime;
	AddInfo(FString::Printf(TEXT("Hearing completion boundary: now=%.6f originalDeadline=%.6f nextAllowed=%.6f brainTickRegistered=%d"),
		World->GetTimeSeconds(), OriginalDeadline, CompletionCooldown,
		Controller->GetBrainComponent()->PrimaryComponentTick.IsTickFunctionRegistered() ? 1 : 0));
	TestTrue(TEXT("Completing the transaction publishes a future cooldown before callbacks"), CompletionCooldown > World->GetTimeSeconds());
	ReportBehaviorHearingStimulus(*Controller, *Target, HeardLocation);
	TestFalse(TEXT("Continuous gunfire cannot bypass the completion cooldown"), Controller->bInvestigatingNoise);
	TestEqual(TEXT("A denied stimulus preserves the completion deadline"), Controller->NextNoiseInvestigationTime, CompletionCooldown);
	if (!TickFor(0.12f)) return false;
	ReportBehaviorHearingStimulus(*Controller, *Target, HeardLocation);
	TestTrue(TEXT("A genuinely new investigation may start after cooldown"), Controller->bInvestigatingNoise);
	// 无 NavMesh 的真实 MoveTo 失败必须收口，而不是每声枪响重新提交同一失败路径。
	TestTrue(TEXT("The search leaf executes using existing MoveTo ownership"), Controller->ExecuteBehaviorAction(EFPEnemyBehaviorAction::Pursue));
	TestFalse(TEXT("Actual navigation failure completes the noise transaction"), Controller->bInvestigatingNoise);
	if (!TickFor(0.12f)) return false;
	ReportBehaviorHearingStimulus(*Controller, *Target, HeardLocation);
	if (!ReportBehaviorSightStimulus(Controller, Target, true, FVector(900.0f, 100.0f, 0.0f), *this)) return false;
	TestFalse(TEXT("Successful sight replaces noise-only investigation"), Controller->bInvestigatingNoise);
	const FVector VisualLocation = Controller->LastKnownTargetLocation;
	ReportBehaviorHearingStimulus(*Controller, *Target, FVector(2000.0f, 0.0f, 0.0f));
	TestTrue(TEXT("Hearing cannot overwrite a current visual location"), Controller->LastKnownTargetLocation.Equals(VisualLocation));
	Controller->NotifyCombatTargetUnavailable(Target);
	Controller->InitializeCombatContext(Target, nullptr);
	Controller->GetBrainComponent()->PauseLogic(TEXT("Hearing pause boundary"));
	ReportBehaviorHearingStimulus(*Controller, *Target, HeardLocation);
	TestFalse(TEXT("A paused brain does not start a new noise investigation"), Controller->bInvestigatingNoise);
	Controller->GetBrainComponent()->ResumeLogic(TEXT("Hearing resume boundary"));
	ReportBehaviorHearingStimulus(*Controller, *Target, HeardLocation);
	TestTrue(TEXT("Resuming permits a later hearing event"), Controller->bInvestigatingNoise);
	Controller->SetRole(ROLE_SimulatedProxy);
	ReportBehaviorHearingStimulus(*Controller, *Target, MovedNoise);
	TestTrue(TEXT("A non-authoritative controller cannot change its stored sound"), Controller->LastKnownTargetLocation.Equals(HeardLocation));
	Controller->SetRole(ROLE_Authority);
	Controller->StopAI();
	ReportBehaviorHearingStimulus(*Controller, *Target, MovedNoise);
	TestFalse(TEXT("StopAI clears investigation and rejects stale stimuli"), Controller->bInvestigatingNoise);
	TestTrue(TEXT("StopAI clears all last-known location data"), Controller->LastKnownTargetLocation.IsZero());
	Controller->UnPossess();
	Controller->Possess(Enemy);
	Controller->GetBrainComponent()->PrimaryComponentTick.UnRegisterTickFunction();
	Controller->InitializeCombatContext(Target, nullptr);
	ReportBehaviorHearingStimulus(*Controller, *Target, HeardLocation);
	TestTrue(TEXT("New possession has a fresh hearing lifetime"), Controller->bInvestigatingNoise);
	UGameplayStatics::ApplyDamage(Enemy, 10000.0f, nullptr, Target, nullptr);
	TestTrue(TEXT("Death follows the real damage path"), Enemy->IsDead());
	ReportBehaviorHearingStimulus(*Controller, *Target, MovedNoise);
	TestFalse(TEXT("Death clears memory and rejects all later hearing events"), Controller->bInvestigatingNoise);
	UGameplayStatics::ApplyDamage(Target, 10000.0f, nullptr, nullptr, nullptr);
	TestFalse(TEXT("A dead player cannot report new gunfire"), AfpstrueEnemyAIController::ReportAuthoritativeGunshot(Target, HeardLocation));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFPEnemyBehaviorTreeStructureTest, "fpstrue.AI.BehaviorTree.DefaultStructure",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 检查默认树的黑板类型、等待/采样顺序和攻击失败后的站位分支，不把生成树等同于运行验收。
bool FFPEnemyBehaviorTreeStructureTest::RunTest(const FString& Parameters)
{
	UBehaviorTree* Tree = NewObject<UBehaviorTree>();
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(Tree);
	const bool bHadEngineSelfKey = Blackboard->GetKeyID(FBlackboard::KeySelf) != FBlackboard::InvalidKey;
	TestTrue(TEXT("Populate an empty editable asset"), UfpstrueEnemyBehaviorTreeLibrary::PopulateDefaultTree(Tree, Blackboard));
	TestEqual(TEXT("Engine default SelfActor key is preserved"), Blackboard->GetKeyID(FBlackboard::KeySelf) != FBlackboard::InvalidKey,
			  bHadEngineSelfKey);
	TestTrue(TEXT("Required blackboard keys have correct types"), FPHasEnemyBlackboardSchema(Blackboard));
	TestEqual(TEXT("Default schema includes independent retreat and tactical state"), Blackboard->Keys.Num(), 10);
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
	if (!TestEqual(TEXT("Invalid target, retreat, attack, outside range, combat, surround, pursue"), Selector->Children.Num(), 7))
		return false;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		TestEqual(TEXT("Conditional branches are guarded by Blackboard"), Selector->Children[Index].Decorators.Num(), 1);
	}
	const UfpstrueBTTask_EnemyAction* Retreat = Cast<UfpstrueBTTask_EnemyAction>(Selector->Children[1].ChildTask);
	const UfpstrueBTTask_EnemyAction* Sustain = Cast<UfpstrueBTTask_EnemyAction>(Selector->Children[2].ChildTask);
	TestTrue(TEXT("Retreat takes priority over sustaining an attack"), Retreat != nullptr && Retreat->Action == EFPEnemyBehaviorAction::Retreat);
	TestTrue(TEXT("SustainAttack remains after retreat"), Sustain != nullptr && Sustain->Action == EFPEnemyBehaviorAction::SustainAttack);
	// 攻击尝试失败必须有站位维护兜底，避免敌人因许可不足而停止整个决策循环。
	UBTComposite_Selector* Combat = Cast<UBTComposite_Selector>(Selector->Children[4].ChildComposite);
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

// 验证建树工具只填充兼容的空资产，保留设计师内容，并拒绝错误类型或跨敌人共享的状态键。
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
	// 分别破坏目标类型、独立实例状态和必需键，验证黑板校验覆盖这些实际接线错误。
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
	FBlackboardEntry& RetreatEntry = Blackboard->Keys[Blackboard->GetKeyID(FPEnemyBlackboard::Retreating)];
	RetreatEntry.bInstanceSynced = true;
	TestFalse(TEXT("Per-enemy retreat state cannot be instance synchronized"), FPHasEnemyBlackboardSchema(Blackboard));
	RetreatEntry.bInstanceSynced = false;
	UBlackboardKeyType* OriginalRetreatType = RetreatEntry.KeyType;
	RetreatEntry.KeyType = NewObject<UBlackboardKeyType_Object>(Blackboard);
	TestFalse(TEXT("Retreat state must remain a Bool key"), FPHasEnemyBlackboardSchema(Blackboard));
	RetreatEntry.KeyType = OriginalRetreatType;
	TestTrue(TEXT("Restored independent schema is valid"), FPHasEnemyBlackboardSchema(Blackboard));
	TestTrue(TEXT("Upgrading a current default tree is idempotent"),
		UfpstrueEnemyBehaviorTreeLibrary::UpgradeDefaultTreeForRetreat(Tree, Blackboard));
	TestTrue(TEXT("Current default upgrade preserves the root instance"), Tree->RootNode == OriginalRoot);
	Blackboard->Keys.RemoveAt(Blackboard->GetKeyID(FPEnemyBlackboard::Retreating));
	TestFalse(TEXT("Missing required key is rejected"), FPHasEnemyBlackboardSchema(Blackboard));
	UBehaviorTree* LegacyTree = NewObject<UBehaviorTree>();
	UBlackboardData* LegacyBlackboard = NewObject<UBlackboardData>(LegacyTree);
	if (!TestTrue(TEXT("Legacy fixture starts from the native default template"),
		UfpstrueEnemyBehaviorTreeLibrary::PopulateDefaultTree(LegacyTree, LegacyBlackboard))) return false;
	UBTComposite_Sequence* LegacyRoot = CastChecked<UBTComposite_Sequence>(LegacyTree->RootNode);
	UBTComposite_Selector* LegacyChoice = CastChecked<UBTComposite_Selector>(LegacyRoot->Children[2].ChildComposite);
	UBTTaskNode* OriginalSustain = LegacyChoice->Children[2].ChildTask;
	LegacyChoice->Children.RemoveAt(1);
	LegacyBlackboard->Keys.RemoveAt(LegacyBlackboard->GetKeyID(FPEnemyBlackboard::Retreating));
	for (const FName Key : {FPEnemyBlackboard::TacticalRole, FPEnemyBlackboard::TacticalGoal, FPEnemyBlackboard::HasInnerSlot})
		LegacyBlackboard->Keys.RemoveAt(LegacyBlackboard->GetKeyID(Key));
	TestEqual(TEXT("Legacy fixture retains the original six-key schema"), LegacyBlackboard->Keys.Num(), 6);
	TestFalse(TEXT("Legacy schema cannot run the new retreat contract before upgrade"), FPHasEnemyBlackboardSchema(LegacyBlackboard));
	TestTrue(TEXT("A verified legacy native default tree upgrades in place"),
		UfpstrueEnemyBehaviorTreeLibrary::UpgradeDefaultTreeForRetreat(LegacyTree, LegacyBlackboard));
	TestTrue(TEXT("Legacy upgrade preserves the original root"), LegacyTree->RootNode == LegacyRoot);
	TestTrue(TEXT("Legacy upgrade preserves the original attack task instance"), LegacyChoice->Children[2].ChildTask == OriginalSustain);
	TestEqual(TEXT("Legacy upgrade adds independent retreat and tactical keys"), LegacyBlackboard->Keys.Num(), 10);
	TestTrue(TEXT("The upgraded legacy schema satisfies the full contract"), FPHasEnemyBlackboardSchema(LegacyBlackboard));
	const UfpstrueBTTask_EnemyAction* LegacyRetreat = Cast<UfpstrueBTTask_EnemyAction>(LegacyChoice->Children[1].ChildTask);
	TestTrue(TEXT("Legacy upgrade inserts retreat before the existing attack branch"),
		LegacyRetreat != nullptr && LegacyRetreat->Action == EFPEnemyBehaviorAction::Retreat);
	UBehaviorTree* CustomizedTree = NewObject<UBehaviorTree>();
	UBlackboardData* CustomizedBlackboard = NewObject<UBlackboardData>(CustomizedTree);
	if (!TestTrue(TEXT("Custom fixture starts from a valid native template"),
		UfpstrueEnemyBehaviorTreeLibrary::PopulateDefaultTree(CustomizedTree, CustomizedBlackboard))) return false;
	UBTComposite_Sequence* CustomizedRoot = CastChecked<UBTComposite_Sequence>(CustomizedTree->RootNode);
	UBTComposite_Selector* CustomizedChoice = CastChecked<UBTComposite_Selector>(CustomizedRoot->Children[2].ChildComposite);
	UfpstrueBTTask_EnemyAction* CustomizedAction = CastChecked<UfpstrueBTTask_EnemyAction>(CustomizedChoice->Children[1].ChildTask);
	CustomizedAction->Action = EFPEnemyBehaviorAction::Idle;
	TestFalse(TEXT("Upgrade refuses a designer-modified behavior branch"),
		UfpstrueEnemyBehaviorTreeLibrary::UpgradeDefaultTreeForRetreat(CustomizedTree, CustomizedBlackboard));
	TestTrue(TEXT("Rejected upgrade preserves the designer root"), CustomizedTree->RootNode == CustomizedRoot);
	TestTrue(TEXT("Rejected upgrade preserves the designer action"), CustomizedAction->Action == EFPEnemyBehaviorAction::Idle);
	TestEqual(TEXT("Rejected upgrade preserves all Blackboard entries"), CustomizedBlackboard->Keys.Num(), 10);

	// 已有自定义键的资产不得被默认模板覆盖，拒绝后也不能留下半成品树。
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

// 运行真实行为树，检查个体黑板隔离、感知唤醒、失去目标的记忆和 StopAI/重新 Possess 边界。
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
	// 只关闭无关移动/碰撞，保留真实 Pawn、Controller、Brain 和 Blackboard 初始化链。
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
	// 注入玩家身份不是发现玩家位置：感知成功之前树和攻击入口都应保持无目标。
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
	// 感知消息应立即唤醒长 Idle 等待，且只更新当前敌人的黑板。
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

	// 管理器知道隐藏玩家的实时位置，但失去视线的个体只能追最后一次看见的位置。
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
	// 丢失视线立即撤销目标 Actor、攻击和实时朝向；保留有限记忆但不泄露隐藏位置。
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
	// 重复失败刺激不能续期；记忆过期停止搜索，重新看见才恢复实时目标。
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
	// 隐藏目标/共享缓存场景已经结束；独立近战场景不能从场景兜底查找重新拿到旧 Manager。
	SharedManager->Destroy();
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
	// 从黑板键通知内部读整份状态，验证先提交所有决策字段、再统一发布通知。
	const FDelegateHandle SnapshotObserver = DecisionBlackboard->RegisterObserver(TargetKey, First,
		FOnBlackboardChangeNotification::CreateLambda([&](const UBlackboardComponent& Blackboard, FBlackboard::FKey ChangedKey)
		{
			++InvalidSnapshotNotifications;
			TestEqual(TEXT("Target observer receives the changed target key"), ChangedKey, TargetKey);
			TestNull(TEXT("Observer sees the cleared target"), Blackboard.GetValueAsObject(FPEnemyBlackboard::TargetActor));
			for (const FName Key :
				 {FPEnemyBlackboard::HasTarget, FPEnemyBlackboard::Attacking, FPEnemyBlackboard::InAttackRange, FPEnemyBlackboard::InChaseRange,
				  FPEnemyBlackboard::Retreating})
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
	// 目标死亡和销毁应撤销决策，但不永久停止树；显式注入新目标后仍能重新发现。
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
		{FPEnemyBlackboard::HasTarget, FPEnemyBlackboard::Attacking, FPEnemyBlackboard::InAttackRange, FPEnemyBlackboard::InChaseRange,
		 FPEnemyBlackboard::Retreating})
	{
		TestFalse(TEXT("A destroyed target leaves no decision flags"), DecisionBlackboard->GetValueAsBool(Key));
	}
	AfpstrueEnemyAIController* UnpossessedController = World->SpawnActor<AfpstrueEnemyAIController>();
	if (!TestNotNull(TEXT("Unpossessed controller"), UnpossessedController)) return false;
	TestTrue(TEXT("Missing controlled pawn is an invalid move context"),
		UnpossessedController->MoveToGoal(FVector::ZeroVector, 15.0f, true) ==
		AfpstrueEnemyAIController::EMoveGoalResult::InvalidContext);
	// 先将成功刺激排队再 StopAI，确保迟到的真实回调不能重新打开命令入口。
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
		 {FPEnemyBlackboard::HasTarget, FPEnemyBlackboard::Attacking, FPEnemyBlackboard::InAttackRange, FPEnemyBlackboard::InChaseRange,
		  FPEnemyBlackboard::Retreating})
	{
		TestFalse(TEXT("StopAI clears every decision flag"), First->GetBlackboardComponent()->GetValueAsBool(Key));
	}
	// 重用 Controller/Blackboard 时必须启动新上下文，而不是继承上一轮目标。
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

// 不注入刺激：用真实 Sight、墙体碰撞与 Pawn 朝向验证看见、遮挡、转身和重新发现。
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
	// 给真实感知系统有限更新窗口，等待目标可见性达到预期；超出窗口视为失败。
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFPEnemyIdleSearchSightTest,
	"fpstrue.AI.Perception.MultiplayerIdleSearchAndStop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 不注入刺激、不指定敌人目标：真实 BT 巡视、Movement 平滑转身和 Sight 共同发现背后的已入场玩家。
bool FFPEnemyIdleSearchSightTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	if (!TestNotNull(TEXT("Idle search world has the real AI system"), World->CreateAISystem())) return false;
	World->GetWorldSettings()->DefaultGameMode = AfpstruePvpGameMode::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	AActor* Floor = World->SpawnActor<AActor>();
	AActor* Wall = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Idle search floor"), Floor) || !TestNotNull(TEXT("Idle search wall"), Wall)) return false;
	const auto AddBox = [](AActor* Owner, const FVector& Extent, const FVector& Location)
	{
		UBoxComponent* Box = NewObject<UBoxComponent>(Owner);
		Owner->AddInstanceComponent(Box);
		Owner->SetRootComponent(Box);
		Box->SetBoxExtent(Extent);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->RegisterComponent();
		Owner->SetActorLocation(Location);
		return Box;
	};
	AddBox(Floor, FVector(10000.0f, 10000.0f, 20.0f), FVector(0.0f, 0.0f, -116.0f));
	UBoxComponent* WallBox = AddBox(Wall, FVector(40.0f, 300.0f, 300.0f), FVector(-500.0f, 0.0f, 100.0f));
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
		AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Idle search observer"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(FTransform::Identity);
	AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>(FVector(-1000.0f, 0.0f, 0.0f), FRotator::ZeroRotator);
	APlayerController* Player = World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Idle search controller"), Controller) || !TestNotNull(TEXT("Idle search target"), Target) ||
		!TestNotNull(TEXT("An admitted player controller"), Player)) return false;
	Target->GetCharacterMovement()->SetComponentTickEnabled(false);
	// 只补齐原生 Pawn 的本地输入资产前置，不发送输入；玩家控制器 Possess 仍经过真实入场链。
	for (const FName ActionName : {TEXT("MoveAction"), TEXT("LookAction"), TEXT("JumpAction"), TEXT("FireAction"),
		TEXT("AimAction"), TEXT("SprintAction"), TEXT("ReloadAction")})
	{
		FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Target->GetClass(), ActionName);
		if (!TestNotNull(TEXT("Idle search player retains its declared input action contract"), Property)) return false;
		Property->SetObjectPropertyValue_InContainer(Target, NewObject<UInputAction>(Target));
	}
	Player->Possess(Target);
	Controller->IdleDecisionInterval = 0.1f;
	Controller->IdleSearchYawSpeed = 90.0f;
	Controller->Possess(Enemy);
	// 临时世界没有网络驱动，只固定多人选敌模式；目标入场资格、BT、旋转、遮挡和视距仍走生产实现。
	Controller->bUsesMultiplayerTargets = true;
	Controller->InitializeCombatContext(nullptr, nullptr);
	UAIPerceptionSystem* PerceptionSystem = UAIPerceptionSystem::GetCurrent(World);
	if (!TestNotNull(TEXT("Real idle search sight system"), PerceptionSystem) ||
		!TestNotNull(TEXT("Real idle search brain"), Controller->GetBrainComponent())) return false;
	PerceptionSystem->RegisterSourceForSenseClass(UAISense_Sight::StaticClass(), *Target);
	const auto TickFor = [&](float Seconds)
	{
		for (int32 Frame = 0; Frame < FMath::CeilToInt(Seconds / 0.02f); ++Frame)
			if (!Fixture.TickTestWorld(0.02f)) { Fixture.ForwardErrorMessages(this); return false; }
		return true;
	};
	const double OriginalYawRate = Enemy->GetCharacterMovement()->RotationRate.Yaw;
	// 首轮 BT 有真实随机错峰和 latent 调度；先等巡视分支启动，再测完整的旋转时间窗，不能把等待时间当作 Movement 执行时间。
	float SearchStartWait = 0.0f;
	while ((!Controller->bIdleSearchActive || !Enemy->GetCharacterMovement()->IsMovingOnGround()) && SearchStartWait < 1.0f)
	{
		if (!TickFor(0.02f)) return false;
		SearchStartWait += 0.02f;
	}
	if (!TestTrue(TEXT("The real idle branch starts and the observer is grounded within its startup budget"),
		Controller->bIdleSearchActive && Enemy->GetCharacterMovement()->IsMovingOnGround())) return false;
	const float SearchStartYaw = Enemy->GetActorRotation().Yaw;
	if (!TickFor(0.3f)) return false;
	const float InitialYaw = Enemy->GetActorRotation().Yaw;
	const float SearchYawDelta = FMath::FindDeltaAngleDegrees(SearchStartYaw, InitialYaw);
	AddInfo(FString::Printf(TEXT("Idle search rotation sample: startupWait=%.3f startYaw=%.3f endYaw=%.3f delta=%.3f requestedWindow=0.300 yawRate=%.3f"),
		SearchStartWait, SearchStartYaw, InitialYaw, SearchYawDelta, Enemy->GetCharacterMovement()->RotationRate.Yaw));
	TestTrue(TEXT("Idle search is active without assigning an unseen target"), Controller->bIdleSearchActive && Controller->GetTargetCharacter() == nullptr);
	TestTrue(TEXT("Movement turns gradually rather than snapping to the next ninety-degree heading"), SearchYawDelta > 0.0f && SearchYawDelta <= 29.0f);
	TestFalse(TEXT("Idle search does not enable a new controller Actor Tick"), Controller->PrimaryActorTick.bCanEverTick);
	Controller->GetBrainComponent()->PauseLogic(TEXT("Idle search pause boundary"));
	const FRotator PausedRotation = Enemy->GetActorRotation();
	if (!TickFor(0.3f)) return false;
	TestTrue(TEXT("Pausing the brain immediately freezes an already published search turn"), Enemy->GetActorRotation().Equals(PausedRotation, 0.01f));
	Controller->GetBrainComponent()->ResumeLogic(TEXT("Idle search resume boundary"));
	if (!TickFor(4.5f)) return false;
	TestNull(TEXT("A complete search sweep does not discover a player through a real wall"), Controller->GetTargetCharacter());
	TestNull(TEXT("The wall-hidden player has never been injected as a cached target"), Controller->TargetCharacter.Get());
	WallBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Enemy->SetActorRotation(FRotator::ZeroRotator);
	Controller->SetControlRotation(FRotator::ZeroRotator);
	bool bDiscovered = false;
	for (int32 Frame = 0; Frame < 200; ++Frame)
	{
		if (!TickFor(0.02f)) return false;
		if (Controller->GetTargetCharacter() == Target) { bDiscovered = true; break; }
	}
	if (!TestTrue(TEXT("Without manual facing or fake stimuli, the search sweep discovers the player behind it"), bDiscovered)) return false;
	if (!TickFor(0.2f)) return false;
	TestFalse(TEXT("Acquiring a real target exits idle search"), Controller->bIdleSearchActive);
	TestEqual(TEXT("Acquiring a target restores the authored movement yaw rate"), Enemy->GetCharacterMovement()->RotationRate.Yaw, OriginalYawRate);
	Target->SetActorLocation(FVector(6000.0f, 0.0f, 0.0f));
	Controller->NotifyCombatTargetUnavailable(Target);
	Controller->GetPerceptionComponent()->RequestStimuliListenerUpdate();
	if (!TickFor(5.0f)) return false;
	TestNull(TEXT("Search does not acquire an admitted player outside the existing forty-five-meter sight range"), Controller->GetTargetCharacter());
	TestNull(TEXT("Out-of-range search does not retain a live actor target"), Controller->TargetCharacter.Get());
	TestTrue(TEXT("An unseen player leaves the real brain searching rather than permanently frozen"), Controller->bIdleSearchActive);
	Controller->StopAI();
	const FRotator StoppedRotation = Enemy->GetActorRotation();
	if (!TickFor(0.3f)) return false;
	TestTrue(TEXT("StopAI immediately stops the in-progress search turn"), Enemy->GetActorRotation().Equals(StoppedRotation, 0.01f));
	TestFalse(TEXT("StopAI clears search ownership"), Controller->bIdleSearchActive);
	TestEqual(TEXT("StopAI restores the authored movement yaw rate"), Enemy->GetCharacterMovement()->RotationRate.Yaw, OriginalYawRate);
	TestFalse(TEXT("A stale idle request cannot restart a stopped controller"), Controller->ExecuteBehaviorAction(EFPEnemyBehaviorAction::Idle));
	Controller->UnPossess();
	Controller->Possess(Enemy);
	Controller->bUsesMultiplayerTargets = true;
	Controller->InitializeCombatContext(nullptr, nullptr);
	if (!TickFor(0.3f)) return false;
	UGameplayStatics::ApplyDamage(Enemy, 1000.0f, Player, Target, nullptr);
	TestTrue(TEXT("Death follows the real damage and AI stop path"), Enemy->IsDead());
	TestFalse(TEXT("Death cancels search ownership"), Controller->bIdleSearchActive);
	const FRotator DeadRotation = Enemy->GetActorRotation();
	if (!TickFor(0.3f)) return false;
	TestTrue(TEXT("A dead enemy cannot keep turning toward an old search heading"), Enemy->GetActorRotation().Equals(DeadRotation, 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFPEnemyMoveRequestOwnershipTest,
	"fpstrue.AI.BehaviorTree.MoveRequestOwnershipAndBackoff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 检查真实 MoveTo 失败的独立退避、即时到达缓存和旧请求 ID 过滤，含完成回调内 StopAI 重入。
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
	DisableAutomaticBehaviorTick(*Controller->GetBrainComponent());
	UPathFollowingComponent* Following = Controller->GetPathFollowingComponent();
	if (!TestNotNull(TEXT("Engine path-following component"), Following)) return false;
	int32 SubmittedCompletions = 0;
	FAIRequestID FirstFailedId = FAIRequestID::InvalidRequest;
	bool bStopDuringCompletion = false;
	// 记录引擎分配的完成请求，并可在同步完成广播中关闭 AI，检验外层返回后的身份复核。
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
	AfpstrueEnemyCharacter* OtherEnemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(),
		SpawnTransform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Another enemy can consume the same candidate point"), OtherEnemy)) return false;
	OtherEnemy->AutoPossessAI = EAutoPossessAI::Disabled;
	OtherEnemy->FinishSpawning(SpawnTransform);
	AfpstrueEnemyAIController* OtherController = World->SpawnActor<AfpstrueEnemyAIController>();
	if (!TestNotNull(TEXT("Another controller owns its own navigation"), OtherController)) return false;
	OtherController->Possess(OtherEnemy);
	DisableAutomaticBehaviorTick(*OtherController->GetBrainComponent());
	UPathFollowingComponent* OtherFollowing = OtherController->GetPathFollowingComponent();
	if (!TestNotNull(TEXT("Another enemy retains its own path-following component"), OtherFollowing)) return false;
	TestTrue(TEXT("Shared candidate locations do not share path-following state"), Following != OtherFollowing);
	int32 OtherCompletions = 0;
	FAIRequestID OtherFailedId = FAIRequestID::InvalidRequest;
	const FDelegateHandle OtherObserver = OtherFollowing->OnRequestFinished.AddLambda([&](FAIRequestID Id, const FPathFollowingResult& Result)
	{
		++OtherCompletions;
		if (Result.IsFailure()) OtherFailedId = Id;
	});
	OtherController->MoveToGoal(Unreachable, 15.0f, true);
	TestEqual(TEXT("One enemy's failure backoff does not suppress another enemy's request"), OtherCompletions, 1);
	TestEqual(TEXT("Another enemy's completion cannot modify the first path-following component"), SubmittedCompletions, 2);
	OtherController->MoveToGoal(Unreachable, 15.0f, true);
	TestEqual(TEXT("The second enemy has its own failure backoff"), OtherCompletions, 1);
	for (int32 Decision = 0; Decision < 6; ++Decision)
	{
		Controller->MoveToGoal(Unreachable, 15.0f, true);
		Controller->MoveToGoal(Unreachable, 75.0f, false);
	}
	TestEqual(TEXT("Alternating fallback cannot overwrite the other strategy's backoff"), SubmittedCompletions, 2);
	// 不同楼层和接受半径是不同请求；不能被旧失败的退避错误压制。
	const FVector OtherFloor = Unreachable + FVector(0.0, 0.0, 500.0);
	Controller->MoveToGoal(OtherFloor, 15.0f, true);
	TestEqual(TEXT("A different floor is a different navigation request"), SubmittedCompletions, 3);
	Controller->MoveToGoal(OtherFloor, 30.0f, true);
	TestEqual(TEXT("Changed acceptance radius is not suppressed by old failure"), SubmittedCompletions, 4);
	TestTrue(TEXT("Already-at-goal succeeds through the engine immediate-completion route"),
		Controller->MoveToGoal(Controller->GetNavAgentLocation(), 15.0f, true) == EMoveResult::Arrived);
	TestTrue(TEXT("Immediate success publishes an arrival cache after callbacks"), Controller->bHasMoveGoal);
	const auto ArrivalGoal = Controller->LastMoveGoal;
	// 模拟迟到的旧失败，只允许其对应请求失效，不能清掉更新的到达缓存。
	Controller->OnMoveCompleted(FirstFailedId, FPathFollowingResult(EPathFollowingResult::Invalid));
	TestTrue(TEXT("Delayed old failure cannot invalidate newer arrival cache"), Controller->bHasMoveGoal);
	TestTrue(TEXT("Delayed completion preserves new goal semantics"), Controller->LastMoveGoal.Matches(ArrivalGoal, 1.0f));
	Controller->OnMoveCompleted(OtherFailedId, FPathFollowingResult(EPathFollowingResult::Invalid));
	TestTrue(TEXT("Another controller's old completion cannot clear the first arrival cache"), Controller->bHasMoveGoal);
	bStopDuringCompletion = true;
	Controller->MoveToGoal(Unreachable + FVector(1000.0, 0.0, 0.0), 15.0f, true);
	TestFalse(TEXT("StopAI inside synchronous completion closes command gate"), Controller->AcceptsCombatCommands());
	TestFalse(TEXT("Outer MoveTo return cannot restore stopped cache"), Controller->bHasMoveGoal);
	TestFalse(TEXT("Outer MoveTo return cannot restore stopped request ownership"), Controller->OwnedMoveRequestId.IsValid());
	// 解除本用例的完成监听，后续世界清理不再访问局部测试状态。
	Following->OnRequestFinished.Remove(Observer);
	OtherFollowing->OnRequestFinished.Remove(OtherObserver);
	return true;
}

#if WITH_EDITOR

namespace FPEnemyBehaviorAssetTests
{
const TCHAR* const Packages[] = {TEXT("/Game/FirstPerson/AI/BT_FPEnemy"), TEXT("/Game/FirstPerson/AI/BB_FPEnemy"), TEXT("/Game/FirstPerson/AI/BP_FPEnemyAIController"),
								 TEXT("/Game/FirstPerson/Blueprints/enemy/enemy_BP")};

// 只读取已保存的反射引用，检查蓝图默认对象和编辑器节点实际指向哪些资产。
UObject* ReadObjectProperty(const UObject* Object, FName Name)
{
	const FObjectPropertyBase* Property = Object != nullptr ? FindFProperty<FObjectPropertyBase>(Object->GetClass(), Name) : nullptr;
	return Property != nullptr ? Property->GetObjectPropertyValue_InContainer(Object) : nullptr;
}
} // namespace FPEnemyBehaviorAssetTests

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FFPEnemyBehaviorTreeAssetGraphTest, "fpstrue.AI.BehaviorTree.AssetGraphAndBindings",
								  EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 仅当所有可选项目资产存在时枚举资产用例；缺资产明确跳过，不能借原生回退伪装验收。
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

// 校验真实保存的 BT/BB/蓝图引用，以及运行树与可编辑图的节点、装饰器和连线一致性。
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
	// 在实际编辑器输出引脚上查找父子连线，不用运行树结构代替图接线检查。
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
	// 逐条映射运行时父子关系，并确认装饰器与图的 SubNode 共用保存的实例。
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

// 攻击事务、站位缓存与共享动画退出：与上面的 AI 决策保持同一组回归入口。
// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
// 建立不含项目玩法的测试世界，按需创建 AI 系统，隔离攻击事务与导航/生成流程。
bool BeginCombatTestWorld(FTestWorldWrapper& Fixture, FAutomationTestBase& Test, bool bCreateAI = false)
{
	if (!Fixture.CreateTestWorld(EWorldType::Game))
	{
		Fixture.ForwardErrorMessages(&Test);
		return false;
	}
	UWorld* World = Fixture.GetTestWorld();
	if (bCreateAI && !Test.TestNotNull(TEXT("The native BT has an AI system"), World->CreateAISystem())) return false;
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (Fixture.BeginPlayInTestWorld()) return true;
	Fixture.ForwardErrorMessages(&Test);
	return false;
}

// 经真实 Sight 组件广播已看见目标，避免直接写可见标志绕过战斗上下文约束。
bool ReportVisibleCombatTarget(AfpstrueEnemyAIController* Controller, AfpstrueCharacter* Target, FAutomationTestBase& Test)
{
	UAIPerceptionComponent* Perception = Controller->GetPerceptionComponent();
	if (!Test.TestNotNull(TEXT("Combat fixture has the real sight listener"), Perception)) return false;
	FAIStimulus Stimulus(*GetDefault<UAISense_Sight>(), 1.0f, Target->GetActorLocation(),
		Controller->GetPawn()->GetActorLocation(), FAIStimulus::SensingSucceeded);
	// 同一玩家的新会话也通过真实感知组件发送通知，不直接写 Controller 的可见状态。
	Stimulus.SetWantsNotifyOnlyOnValueChange(false);
	Perception->RegisterStimulus(Target, Stimulus);
	Perception->ProcessStimuli();
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyCombatLifecycleTest,
	"fpstrue.Gameplay.Combat.AttackProtectionAndCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 检查攻击保护、单次伤害、许可归还及停止/替换目标/组件退出的统一清理；不验收最终刀刃画面。
bool FFpstrueEnemyCombatLifecycleTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginCombatTestWorld(Fixture, *this, true)) return false;
	UWorld* World = Fixture.GetTestWorld();

	// 保留真实敌人生命周期，但关闭无关碰撞，使用测试 Controller 显式建立目标关系。
	const auto SpawnEnemy = [World]()
	{
		AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
			AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Enemy != nullptr)
		{
			Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
			Enemy->FinishSpawning(FTransform::Identity);
			Enemy->SetActorEnableCollision(false);
		}
		return Enemy;
	};
	AfpstrueEnemyCharacter* Enemy = SpawnEnemy();
	AfpstrueEnemyCharacter* Probe = SpawnEnemy();
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	AfpstrueSurroundManager* Surround = World->SpawnActor<AfpstrueSurroundManager>();
	if (!TestNotNull(TEXT("Enemy"), Enemy) || !TestNotNull(TEXT("Budget probe"), Probe)
		|| !TestNotNull(TEXT("Player"), Target) || !TestNotNull(TEXT("Controller"), Controller)
		|| !TestNotNull(TEXT("Surround manager"), Surround)) return false;
	Target->SetActorEnableCollision(false);
	Target->SetActorLocation(FVector(200.0f, 0.0f, 0.0f));
	Controller->Possess(Enemy);
	Controller->InitializeCombatContext(Target, Surround);
	if (!ReportVisibleCombatTarget(Controller, Target, *this)) return false;
	// 仅固定共享导航输出，资格仍经过生产队列、席位版本和 Controller 决策入口。
	const auto SeedCombatSlots = [&](AfpstrueCharacter* Player)
	{
		Surround->SetTargetCharacter(Player);
		Surround->PublishedTargetLocation = Surround->CachedTargetLocation;
		for (int32 Index = 0; Index < 2; ++Index)
		{
			FfpstrueSurroundSlot& Slot = Surround->SurroundSlots[Index];
			Slot.bHasProjectedSlotLocation = Slot.bHasProjectedApproachLocation = true;
			Slot.ProjectedSlotLocation = Player->GetActorLocation() + FVector(250.0f, Index * 250.0f, 0.0f);
			Slot.ProjectedApproachLocation = Enemy->GetNavAgentLocation() + FVector(0.0f, Index * 200.0f, 0.0f);
		}
		FFPEnemyTacticalAssignment Assignment;
		const bool bGranted = Surround->GetOrRequestTacticalAssignment(Enemy, Assignment)
			&& Assignment.Role == EFPEnemyTacticalRole::Inner
			&& Surround->GetOrRequestTacticalAssignment(Probe, Assignment) && Assignment.Role == EFPEnemyTacticalRole::Inner;
		Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
		return bGranted;
	};
	if (!TestTrue(TEXT("Both permit probes first acquire genuine inner leases"), SeedCombatSlots(Target))) return false;
	// 直接驱动攻击入口，避免 BT 下一轮重新申请名额掩盖清理测试。
	if (!TestNotNull(TEXT("Behavior brain"), Controller->GetBrainComponent())) return false;
	DisableAutomaticBehaviorTick(*Controller->GetBrainComponent());
	FIntProperty* BudgetProperty = FindFProperty<FIntProperty>(Surround->GetClass(), TEXT("MaxActiveAttackers"));
	if (!TestNotNull(TEXT("Editable attack budget"), BudgetProperty)) return false;
	BudgetProperty->SetPropertyValue_InContainer(Surround, 1);
	UfpstrueEnemyCombatComponent* Combat = Enemy->FindComponentByClass<UfpstrueEnemyCombatComponent>();
	if (!TestNotNull(TEXT("Native combat component"), Combat)) return false;
	// 合并源文件不能改变已保存动画资产使用的反射类路径；同一 Notify 对象不保存某个敌人的事务。
	TestEqual(TEXT("Saved attack-window class path remains valid"),
		LoadClass<UAnimNotifyState>(nullptr, TEXT("/Script/fpstrue.fpstrueAnimNotifyState_AttackWindow")),
		UfpstrueAnimNotifyState_AttackWindow::StaticClass());
	UfpstrueAnimNotifyState_AttackWindow* AttackWindow = NewObject<UfpstrueAnimNotifyState_AttackWindow>(World);
	const FAnimNotifyEventReference NotifyContext;
	AttackWindow->NotifyBegin(nullptr, nullptr, 1.0f, NotifyContext);
	AttackWindow->NotifyBegin(Target->GetMesh(), nullptr, 1.0f, NotifyContext);
	TestTrue(TEXT("A new combat transaction is Idle"), Combat->AttackPhase == EFPEnemyAttackPhase::Idle);

	// 与共享注销回归一致：只构造最小插件登记，不加载项目动画资产；注销走真实插件实现。
	if (!TestTrue(TEXT("Animation Sharing is enabled"), UAnimationSharingManager::AnimationSharingEnabled())) return false;
	UfpstrueEnemyAnimationSharingCoordinator* Sharing = NewObject<UfpstrueEnemyAnimationSharingCoordinator>(Surround);
	Surround->AddInstanceComponent(Sharing);
	Sharing->RegisterComponent();
	UAnimationSharingManager* Manager = NewObject<UAnimationSharingManager>(World);
	Sharing->SharingManager = Manager;
	const FArrayProperty* SkeletonDataProperty = FindFProperty<FArrayProperty>(Manager->GetClass(), TEXT("PerSkeletonData"));
	const FObjectPropertyBase* EntryProperty = SkeletonDataProperty != nullptr
		? CastField<FObjectPropertyBase>(SkeletonDataProperty->Inner) : nullptr;
	if (!TestNotNull(TEXT("Plugin skeleton data schema"), EntryProperty)) return false;
	UAnimSharingInstance* Data = static_cast<UAnimSharingInstance*>(NewObject<UObject>(Manager, EntryProperty->PropertyClass));
	SkeletonDataProperty->ContainerPtrToValuePtr<TArray<TObjectPtr<UAnimSharingInstance>>>(Manager)->Add(Data);
	USkeletalMeshComponent* Leader = NewObject<USkeletalMeshComponent>(Surround);
	Surround->AddInstanceComponent(Leader);
	Leader->RegisterComponent();
	USkeletalMeshComponent* Mesh = Enemy->GetMesh();
	Mesh->SetLeaderPoseComponent(Leader, true);
	Mesh->SetComponentTickEnabled(false);
	Mesh->PrimaryComponentTick.bCanEverTick = false;
	Mesh->bIgnoreLeaderPoseComponentLOD = true;
	Mesh->SetComponentTickInterval(0.5f);
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered;
	Enemy->GetCharacterMovement()->SetComponentTickInterval(0.5f);
	Data->RegisteredActors.Add(Enemy);
	Data->PerActorData.AddZeroed_GetRef().ComponentIndices.Add(0);
	FPerComponentData& ComponentData = Data->PerComponentData.AddZeroed_GetRef();
	ComponentData.ActorIndex = 0;
	ComponentData.Component = Mesh;
	Sharing->RegisteredActorHandles.Add(TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemy), 0);
	Enemy->SetAnimationSharingCoordinator(Sharing);

	// 从真实共享登记状态启动攻击，必须先退出共享并恢复高频动画/移动，再进入 Windup。
	TestTrue(TEXT("Attacker takes the single permit"), Surround->TryAcquireAttackPermission(Enemy));
	TestFalse(TEXT("Another enemy cannot take the occupied permit"), Surround->TryAcquireAttackPermission(Probe));
	if (!TestTrue(TEXT("A real attack starts"), Combat->TryAttackTarget())) return false;
	TestTrue(TEXT("An active attack owns its failsafe timer"), World->GetTimerManager().IsTimerActive(Combat->AttackFinishTimerHandle));
	TestTrue(TEXT("Starting an attack enters Windup"), Combat->AttackPhase == EFPEnemyAttackPhase::Windup);
	TestEqual(TEXT("Attack restores full-rate movement before animation"), Enemy->GetCharacterMovement()->GetComponentTickInterval(), 0.0f);
	TestEqual(TEXT("Attack restores full-rate animation"), Mesh->GetComponentTickInterval(), 0.0f);
	TestTrue(TEXT("Attack always refreshes pose and bones"),
		Mesh->VisibilityBasedAnimTickOption == EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones);
	TestFalse(TEXT("Attack detaches from the animation leader"), Mesh->LeaderPoseComponent.IsValid());
	TestFalse(TEXT("Attack restores independent LOD control"), Mesh->bIgnoreLeaderPoseComponentLOD);
	TestTrue(TEXT("Attack restores animation tick eligibility"), Mesh->PrimaryComponentTick.bCanEverTick);
	TestEqual(TEXT("Attack removes the real plugin registration"), Data->RegisteredActors.Num(), 0);
	TestEqual(TEXT("Attack removes the coordinator handle"), Sharing->GetRegisteredEnemyCount(), 0);

	// 两根临时骨骼提供真实 Socket 查询，无需加载 Montage 或项目模型。
	USkeletalMesh* BladeMesh = NewObject<USkeletalMesh>(Enemy, NAME_None, RF_Transient);
	{
		FReferenceSkeletonModifier Modifier(BladeMesh->GetRefSkeleton(), nullptr);
		Modifier.Add(FMeshBoneInfo(TEXT("weapontop"), TEXT("weapontop"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("weaponend"), TEXT("weaponend"), 0), FTransform(FVector(50.0f, 0.0f, 0.0f)));
	}
	BladeMesh->CalculateInvRefMatrices();
	Mesh->bEnableAnimation = false;
	Mesh->SetSkeletalMesh(BladeMesh);
	AttackWindow->NotifyBegin(Mesh, nullptr, 1.0f, NotifyContext);
	TestTrue(TEXT("Anonymous Notify cannot open an unbound attack"), Combat->AttackPhase == EFPEnemyAttackPhase::Windup);
	// 本测试关注窗口/伤害规则；真实播放身份与停止重入由 ExplicitPlaybackAndStopReentry 覆盖。
	Combat->BeginAttackWindow();
	TestTrue(TEXT("A valid window enters Active"), Combat->AttackPhase == EFPEnemyAttackPhase::Active);
	// 席位撤销必须立即封住最终结算，不等待下一轮 BT；同一个敌人重新获授也不能复活旧窗口。
	UfpstrueHealthComponent* TargetHealth = Target->FindComponentByClass<UfpstrueHealthComponent>();
	if (!TestNotNull(TEXT("Damage revocation probe uses the real player health"), TargetHealth)) return false;
	const float BeforeRevokedHit = TargetHealth->GetHealth();
	const uint64 OriginalLeaseRevision = Controller->TacticalAssignment.Revision;
	Surround->ReleaseSurroundSlot(Enemy);
	TestTrue(TEXT("Revocation probe still has an old Active window before BT cancellation"), Combat->AttackPhase == EFPEnemyAttackPhase::Active);
	TestFalse(TEXT("Revoking an inner lease immediately rejects old-window damage"), Combat->TryApplyAttackDamage(Target));
	TestEqual(TEXT("Revoked window cannot change player health"), TargetHealth->GetHealth(), BeforeRevokedHit);
	FFPEnemyTacticalAssignment Regranted;
	if (!TestTrue(TEXT("The same enemy can legitimately acquire a new inner lease"), Surround->GetOrRequestTacticalAssignment(Enemy, Regranted))) return false;
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	TestTrue(TEXT("Regrant has a different lease identity"), Regranted.Revision != OriginalLeaseRevision);
	TestTrue(TEXT("The new inner lease is valid"), Surround->HasInnerSlot(Enemy, Regranted.Revision));
	TestFalse(TEXT("Regranting the same actor cannot ABA-revive its old attack window"), Combat->TryApplyAttackDamage(Target));
	TestEqual(TEXT("Regranted old window still cannot change player health"), TargetHealth->GetHealth(), BeforeRevokedHit);
	Combat->ResetCombat();
	if (!TestTrue(TEXT("A fresh transaction can capture the new lease"), Combat->TryAttackTarget())) return false;
	Combat->BeginAttackWindow();
	const FVector HistoricalSample(11.0f, 22.0f, 33.0f);
	Combat->PreviousWeaponBase = HistoricalSample;
	Combat->BeginAttackWindow();
	TestTrue(TEXT("Duplicate Begin preserves the previous frame sample"), Combat->PreviousWeaponBase == HistoricalSample);
	Combat->EndAttackWindow();
	TestTrue(TEXT("Closing a valid window enters Recovery"), Combat->AttackPhase == EFPEnemyAttackPhase::Recovery);
	Combat->BeginAttackWindow();
	TestTrue(TEXT("Another window before a hit may become Active"), Combat->AttackPhase == EFPEnemyAttackPhase::Active);
	// 环境规则不依赖 NavMesh：近战不可穿越阻挡 Visibility 的墙，也不能攻击明显不同楼层。
	AActor* Wall = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Melee blocker owner"), Wall)) return false;
	UBoxComponent* WallBox = NewObject<UBoxComponent>(Wall);
	Wall->AddInstanceComponent(WallBox);
	Wall->SetRootComponent(WallBox);
	WallBox->SetBoxExtent(FVector(10.0f, 100.0f, 150.0f));
	WallBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	WallBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	WallBox->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	WallBox->RegisterComponent();
	Wall->SetActorLocation(FVector(100.0f, 0.0f, 0.0f));
	TestFalse(TEXT("A wall added after attack start blocks damage"), Combat->TryApplyAttackDamage(Target));
	float SampledDistanceSquared, SampledAttackRange;
	TestFalse(TEXT("BT reach snapshot includes the same wall rule"), Combat->SampleAttackReach(SampledDistanceSquared, SampledAttackRange));
	TestEqual(TEXT("Blocked snapshot still provides distance for pursuit"), SampledDistanceSquared, 40000.0f);
	TestTrue(TEXT("Blocked snapshot still provides range for decision frequency"), SampledAttackRange >= Combat->GetConfiguredAttackRange());
	Wall->Destroy();
	Target->SetActorLocation(FVector(200.0f, 0.0f, 1000.0f));
	TestFalse(TEXT("Moving to another floor after attack start blocks damage"), Combat->TryApplyAttackDamage(Target));
	TestFalse(TEXT("BT reach snapshot also rejects another floor"), Combat->SampleAttackReach(SampledDistanceSquared, SampledAttackRange));
	Target->SetActorLocation(FVector(200.0f, 0.0f, 0.0f));
	TestTrue(TEXT("A new reach sample observes the removed blocker"), Combat->SampleAttackReach(SampledDistanceSquared, SampledAttackRange));
	TestTrue(TEXT("The first valid player hit commits damage"), Combat->TryApplyAttackDamage(Target));
	TestTrue(TEXT("A committed hit closes the window"), Combat->AttackPhase == EFPEnemyAttackPhase::Recovery);
	Combat->BeginAttackWindow();
	TestTrue(TEXT("A hit transaction cannot reopen its window"), Combat->AttackPhase == EFPEnemyAttackPhase::Recovery);
	TestFalse(TEXT("The same transaction cannot commit damage again"), Combat->TryApplyAttackDamage(Target));

	// 窗口关闭只进入收招；完整攻击结束或取消才归还许可并清除保护计时器。
	Combat->EndAttackWindow();
	AttackWindow->NotifyTick(Mesh, nullptr, 0.016f, NotifyContext);
	Combat->EndAttackWindow();
	TestTrue(TEXT("Repeated window close does not finish an attack"), Combat->IsAttacking());
	TestFalse(TEXT("Window close does not release the attack permit"), Surround->TryAcquireAttackPermission(Probe));
	Combat->ResetCombat();
	TestFalse(TEXT("Reset ends the transaction"), Combat->IsAttacking());
	TestFalse(TEXT("Reset clears the failsafe timer"), World->GetTimerManager().IsTimerActive(Combat->AttackFinishTimerHandle));
	TestTrue(TEXT("Reset returns to Idle"), Combat->AttackPhase == EFPEnemyAttackPhase::Idle);
	TestTrue(TEXT("Reset releases the permit while the enemy is alive"), Surround->TryAcquireAttackPermission(Probe));
	Combat->ResetCombat();
	TestFalse(TEXT("Repeated reset cannot release another enemy's permit"), Surround->TryAcquireAttackPermission(Enemy));
	Surround->ReleaseAttackPermission(Probe);

	// 区分正常结束的冷却与取消：Reset 可重试，Finish 消耗一次完整攻击冷却。
	TestTrue(TEXT("The original enemy can take the permit again"), Surround->TryAcquireAttackPermission(Enemy));
	TestTrue(TEXT("Reset does not impose normal-finish cooldown"), Combat->TryAttackTarget());
	Combat->FinishAttack();
	TestTrue(TEXT("Normal completion returns to Idle"), Combat->AttackPhase == EFPEnemyAttackPhase::Idle);
	TestFalse(TEXT("Normal completion uses the same timer cleanup"), World->GetTimerManager().IsTimerActive(Combat->AttackFinishTimerHandle));
	TestFalse(TEXT("Normal completion applies its cooldown"), Combat->CanStartAttack());
	// 不等待真实时间：仅移动测试事务的冷却基准，重新建立 EndPlay 所需的活动事务。
	Combat->LastAttackTime -= Combat->AttackInterval;
	TestTrue(TEXT("A cooled-down enemy takes a new permit"), Surround->TryAcquireAttackPermission(Enemy));
	TestTrue(TEXT("A cooled-down transaction restarts"), Combat->TryAttackTarget());
	Controller->StopAI();
	TestFalse(TEXT("StopAI cancels the attack, not only the decision tree"), Combat->IsAttacking());
	TestFalse(TEXT("StopAI cancels the old attack timer"), World->GetTimerManager().IsTimerActive(Combat->AttackFinishTimerHandle));
	TestTrue(TEXT("StopAI releases the captured permit issuer"), Surround->TryAcquireAttackPermission(Probe));
	Surround->ReleaseAttackPermission(Probe);
	Controller->InitializeCombatContext(Target, Surround);
	if (!ReportVisibleCombatTarget(Controller, Target, *this)) return false;
	if (!TestTrue(TEXT("A restarted context obtains a new inner lease and revision"), SeedCombatSlots(Target))) return false;
	TestTrue(TEXT("The stopped enemy can acquire a fresh permit"), Surround->TryAcquireAttackPermission(Enemy));
	TestTrue(TEXT("A new context begins a fresh attack"), Combat->TryAttackTarget());
	// 换目标不能让旧攻击改打新玩家；应取消旧事务，再由新事务捕获目标。
	AfpstrueCharacter* ReplacementTarget = World->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("Replacement player"), ReplacementTarget)) return false;
	ReplacementTarget->SetActorEnableCollision(false);
	ReplacementTarget->SetActorLocation(FVector(200.0f, 0.0f, 0.0f));
	Controller->InitializeCombatContext(ReplacementTarget, Surround);
	if (!ReportVisibleCombatTarget(Controller, ReplacementTarget, *this)) return false;
	if (!TestTrue(TEXT("Replacement context reacquires target-owned leases"), SeedCombatSlots(ReplacementTarget))) return false;
	TestFalse(TEXT("Replacing combat target cancels the old attack instead of retargeting its damage"), Combat->IsAttacking());
	TestFalse(TEXT("Old target context is cleared"), Combat->AttackTarget.IsValid());
	TestTrue(TEXT("Replacement target permits a new attack"), Combat->TryAttackTarget());
	TestTrue(TEXT("New attack captures the replacement target"), Combat->AttackTarget.Get() == ReplacementTarget);
	// 只销毁组件，保留有效 Enemy，避免 Manager 的失效弱引用兜底让遗漏清理的测试也通过。
	Combat->DestroyComponent();
	TestFalse(TEXT("Component EndPlay ends its attack"), Combat->IsAttacking());
	TestTrue(TEXT("Component EndPlay releases its living owner's permit"), Surround->TryAcquireAttackPermission(Probe));
	Combat->HandleAttackFinishedNotify(Enemy->GetMesh(), FAnimNotifyEventReference());
	TestFalse(TEXT("A finish callback after EndPlay cannot revive the transaction"), Combat->IsAttacking());
	Surround->ReleaseAttackPermission(Probe);
	Enemy->SetAnimationSharingCoordinator(nullptr);
	Sharing->Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueSurroundCacheLifecycleTest,
	"fpstrue.AI.Surround.CacheAndSlotOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 验证位置/导航缓存各有期限，旧槽位身份和失效弱引用不能释放新占用者。
bool FFpstrueSurroundCacheLifecycleTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginCombatTestWorld(Fixture, *this, true)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AfpstrueSurroundManager* Manager = World->SpawnActor<AfpstrueSurroundManager>();
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("Manager"), Manager) || !TestNotNull(TEXT("Target"), Target)) return false;
	Target->SetActorEnableCollision(false);
	Target->SetActorLocation(FVector::ZeroVector);
	Manager->SetTargetCharacter(Target);
	if (!TestTrue(TEXT("A valid target starts an owned asynchronous position query"), Manager->ApproachQueryId != INDEX_NONE)) return false;
	const uint32 InitialRevision = Manager->PositionQueryRevision;
	Manager->UpdateSharedTargetSnapshot(false);
	TestEqual(TEXT("An in-flight shared query is not resubmitted by another sample"), Manager->PositionQueryRevision, InitialRevision);
	// 固定 EQS 输出只检验异步交接与世代；真实 NavMesh、遮挡和点位质量另由场景验收。
	const auto MakeResult = [Manager](int32 QueryId, const TArray<FFPEnemyQueryPoint>& Points, bool bSuccess)
	{
		TSharedPtr<FEnvQueryResult> Result = MakeShared<FEnvQueryResult>();
		Result->QueryID = QueryId;
		Result->Owner = Manager;
		Result->ItemType = UfpstrueEnvQueryItemType_Position::StaticClass();
		const int32 ValueSize = GetDefault<UfpstrueEnvQueryItemType_Position>()->GetValueSize();
		for (const FFPEnemyQueryPoint& Point : Points)
		{
			const int32 Offset = Result->RawData.AddZeroed(ValueSize);
			UfpstrueEnvQueryItemType_Position::SetValue(Result->RawData.GetData() + Offset, Point);
			Result->Items.Add(FEnvQueryItem(Offset));
			Result->Items.Last().Score = 1.0f;
		}
		if (bSuccess) Result->MarkAsFinishedWithoutIssues();
		else Result->MarkAsFailed();
		return Result;
	};
	const auto FailOwnedQuery = [&]()
	{
		const int32 QueryId = Manager->ApproachQueryId;
		const uint32 Revision = Manager->PositionQueryRevision;
		const FVector QueryTarget = Manager->CachedTargetLocation;
		if (UEnvQueryManager* Queries = UEnvQueryManager::GetCurrent(World)) Queries->AbortQuery(QueryId);
		// Abort 也可能同步派发失败；若已被消费，重复交接必须无副作用。
		Manager->HandleApproachQueryFinished(MakeResult(QueryId, {}, false), Revision, QueryTarget);
	};
	FailOwnedQuery();
	TestTrue(TEXT("Missing navigation schedules retry independently of location validity"), Manager->bNeedsNavigationRetry);
	// 小于移动阈值的位移也会在快照超龄后刷新；失败投影与成功投影缓存分别重新尝试。
	Target->SetActorLocation(FVector(190.0f, 0.0f, 0.0f));
	Manager->NextNavigationRetryTime = World->GetTimeSeconds() + 10.0;
	Manager->UpdateSharedTargetSnapshot(false);
	TestEqual(TEXT("Before TTL, sub-threshold motion reuses snapshot"), Manager->CachedTargetLocation.X, 0.0);
	Manager->SharedTargetSnapshotTime -= Manager->SharedTargetMaxAge + 0.1;
	Manager->UpdateSharedTargetSnapshot(false);
	TestEqual(TEXT("Expired snapshot catches sub-threshold motion even after it stops"), Manager->CachedTargetLocation.X, 190.0);
	FailOwnedQuery();
	Manager->NextNavigationRetryTime = World->GetTimeSeconds() - 1.0;
	const uint32 FailedRevision = Manager->PositionQueryRevision;
	Manager->UpdateSharedTargetSnapshot(false);
	TestTrue(TEXT("Stationary failed projection starts a new asynchronous query"), Manager->PositionQueryRevision > FailedRevision);
	FailOwnedQuery();
	TestTrue(TEXT("Stationary failed projection is retried and rescheduled"), Manager->NextNavigationRetryTime > World->GetTimeSeconds());
	// 模拟过去成功的缓存进入新导航世代；目标未动且没有失败重试，也必须重新向当前导航系统取值。
	Manager->bNeedsNavigationRetry = false;
	Manager->NavigationCacheTime = World->GetTimeSeconds() - Manager->NavigationCacheMaxAge - 0.1;
	Manager->SurroundSlots[0].bHasProjectedApproachLocation = true;
	const uint32 CachedRevision = Manager->PositionQueryRevision;
	Manager->UpdateSharedTargetSnapshot(false);
	TestTrue(TEXT("Expired success cache starts a new query while the target stays still"), Manager->PositionQueryRevision > CachedRevision);
	FailOwnedQuery();
	TestFalse(TEXT("Expired success cache is reprojected instead of trusted forever"), Manager->SurroundSlots[0].bHasProjectedApproachLocation);
	TestEqual(TEXT("Reprojection refreshes its own age independently of target motion"), Manager->NavigationCacheTime, World->GetTimeSeconds());

	TArray<FFPEnemyQueryPoint> Points;
	for (const int32 SlotIndex : {0, 2})
	{
		for (const int32 Kind : {0, 1})
		{
			FFPEnemyQueryPoint& Point = Points.AddDefaulted_GetRef();
			const FfpstrueSurroundSlot& Slot = Manager->SurroundSlots[SlotIndex];
			const float Radius = Kind == 0 ? Slot.Radius : Manager->AttackApproachRadius;
			Point.Nav = FNavLocation(Manager->CachedTargetLocation +
				FVector(FMath::Cos(Slot.AngleRadians) * Radius, FMath::Sin(Slot.AngleRadians) * Radius, 0.0f));
			Point.SlotIndex = SlotIndex;
			Point.Kind = Kind;
			Point.Preference = 1.0f;
		}
	}
	const int32 PublishedQueryId = 1000001;
	Manager->ApproachQueryId = PublishedQueryId;
	const uint32 PublishedRevision = ++Manager->PositionQueryRevision;
	Manager->HandleApproachQueryFinished(MakeResult(PublishedQueryId, Points, true), PublishedRevision, Manager->CachedTargetLocation);
	if (!TestTrue(TEXT("Current matching result publishes usable approach points"), Manager->SurroundSlots[0].bHasProjectedApproachLocation)) return false;
	const FVector PublishedApproach = Manager->SurroundSlots[0].ProjectedApproachLocation;
	Manager->ApproachQueryId = PublishedQueryId + 1;
	const uint32 NewRevision = ++Manager->PositionQueryRevision;
	TArray<FFPEnemyQueryPoint> StalePoints = Points;
	StalePoints[1].Nav = FNavLocation(PublishedApproach + FVector(5000.0f, 0.0f, 0.0f));
	Manager->HandleApproachQueryFinished(MakeResult(PublishedQueryId, StalePoints, true), PublishedRevision, Manager->CachedTargetLocation);
	TestEqual(TEXT("Old query cannot consume the newer owned request"), Manager->ApproachQueryId, PublishedQueryId + 1);
	TestTrue(TEXT("Old query cannot overwrite newer candidate positions"), Manager->SurroundSlots[0].ProjectedApproachLocation.Equals(PublishedApproach));
	Manager->HandleApproachQueryFinished(MakeResult(PublishedQueryId + 1, StalePoints, true), PublishedRevision, Manager->CachedTargetLocation);
	TestEqual(TEXT("Matching ID with an obsolete revision remains rejected"), Manager->ApproachQueryId, PublishedQueryId + 1);
	Manager->HandleApproachQueryFinished(MakeResult(PublishedQueryId + 1, StalePoints, true), NewRevision,
		Manager->CachedTargetLocation + FVector(Manager->SharedTargetMoveThreshold + 1.0f, 0.0f, 0.0f));
	TestTrue(TEXT("A result built around an obsolete target snapshot is rejected"), Manager->SurroundSlots[0].ProjectedApproachLocation.Equals(PublishedApproach));
	TestEqual(TEXT("An obsolete target snapshot releases the completed query identity"), Manager->ApproachQueryId, INDEX_NONE);
	Manager->ApproachQueryId = PublishedQueryId + 1;
	Manager->HandleApproachQueryFinished(MakeResult(PublishedQueryId + 1, Points, true), NewRevision, Manager->CachedTargetLocation);

	// 构造两个独立占用者，模拟槽位已复用而旧映射仍迟到注销的边界。
	const auto SpawnEnemy = [World]()
	{
		AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
			AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Enemy != nullptr)
		{
			Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
			Enemy->FinishSpawning(FTransform::Identity);
		}
		return Enemy;
	};
	AfpstrueEnemyCharacter* OldEnemy = SpawnEnemy();
	AfpstrueEnemyCharacter* NewEnemy = SpawnEnemy();
	if (!TestNotNull(TEXT("Old occupant"), OldEnemy) || !TestNotNull(TEXT("New occupant"), NewEnemy)) return false;
	OldEnemy->SetActorLocation(Manager->SurroundSlots[0].ProjectedApproachLocation);
	NewEnemy->SetActorLocation(Manager->SurroundSlots[2].ProjectedApproachLocation);
	FFPEnemyTacticalAssignment FirstAssignment;
	FFPEnemyTacticalAssignment SecondAssignment;
	const uint32 SharedRevision = Manager->PositionQueryRevision;
	TestTrue(TEXT("First enemy consumes the published shared result"), Manager->GetOrRequestTacticalAssignment(OldEnemy, FirstAssignment));
	TestTrue(TEXT("Second enemy consumes the same published shared result"), Manager->GetOrRequestTacticalAssignment(NewEnemy, SecondAssignment));
	TestFalse(TEXT("Shared candidates retain distinct occupied approach goals"), FirstAssignment.Goal.Equals(SecondAssignment.Goal));
	TestEqual(TEXT("Consuming candidates does not run one query per enemy"), Manager->PositionQueryRevision, SharedRevision);
	Manager->ReleaseSurroundSlot(OldEnemy);
	Manager->ReleaseSurroundSlot(NewEnemy);
	Manager->EnemyToSlot.Add(OldEnemy, 0);
	Manager->SurroundSlots[0].Occupant = NewEnemy;
	Manager->ReleaseSurroundSlot(OldEnemy);
	TestTrue(TEXT("Stale slot mapping does not release the new occupant"), Manager->SurroundSlots[0].Occupant.Get() == NewEnemy);
	Manager->GetOrRequestTacticalAssignment(OldEnemy, FirstAssignment);
	Manager->EnemyToSlot.Add(TWeakObjectPtr<AfpstrueEnemyCharacter>(OldEnemy), 0);
	OldEnemy->Destroy();
	Manager->CleanupInvalidEntries();
	TestTrue(TEXT("Expired weak-key cleanup also preserves a reused slot"), Manager->SurroundSlots[0].Occupant.Get() == NewEnemy);
	Manager->SurroundSlots[0].Occupant.Reset();
	Manager->SurroundSlots[0].bHasProjectedSlotLocation = true;
	Manager->SurroundSlots[0].bHasProjectedApproachLocation = false;
	Manager->SurroundSlots[2].bHasProjectedApproachLocation = false;
	TestEqual(TEXT("An unreachable approach is never reserved based only on a valid standing point"),
		Manager->FindBestFreeSlot(FVector::ZeroVector, 0), INDEX_NONE);
	// 内圈只需要攻击接近点、外圈只需要布局点；废弃的外圈接近/LOS 不能再触发导航重试。
	TArray<FFPEnemyQueryPoint> CompletePoints;
	for (int32 SlotIndex = 0; SlotIndex < Manager->SurroundSlots.Num(); ++SlotIndex)
	{
		const FfpstrueSurroundSlot& Slot = Manager->SurroundSlots[SlotIndex];
		FFPEnemyQueryPoint& Point = CompletePoints.AddDefaulted_GetRef();
		const float Radius = Slot.RingIndex == 0 ? Manager->AttackApproachRadius : Slot.Radius;
		Point.Nav = FNavLocation(Manager->CachedTargetLocation +
			FVector(FMath::Cos(Slot.AngleRadians) * Radius, FMath::Sin(Slot.AngleRadians) * Radius, 0.0f));
		Point.SlotIndex = SlotIndex;
		Point.Kind = Slot.RingIndex == 0 ? 1 : 0;
	}
	int32 CompleteQueryId = PublishedQueryId + 10;
	const auto PublishComplete = [&](const TArray<FFPEnemyQueryPoint>& QueryPoints)
	{
		Manager->ApproachQueryId = ++CompleteQueryId;
		const uint32 Revision = ++Manager->PositionQueryRevision;
		Manager->HandleApproachQueryFinished(MakeResult(CompleteQueryId, QueryPoints, true), Revision, Manager->CachedTargetLocation);
	};
	PublishComplete(CompletePoints);
	TestFalse(TEXT("All consumed inner approach and outer standing goals satisfy the navigation cache"), Manager->bNeedsNavigationRetry);
	TestFalse(TEXT("Outer seats do not require a projected attack approach"), Manager->SurroundSlots[Manager->InnerSlotCount].bHasProjectedApproachLocation);
	TArray<FFPEnemyQueryPoint> MissingOuter = CompletePoints;
	MissingOuter.RemoveAt(Manager->InnerSlotCount);
	PublishComplete(MissingOuter);
	TestTrue(TEXT("A missing consumed outer standing point still schedules navigation retry"), Manager->bNeedsNavigationRetry);
	TArray<FFPEnemyQueryPoint> MissingInner = CompletePoints;
	MissingInner.RemoveAt(0);
	PublishComplete(MissingInner);
	TestTrue(TEXT("A missing consumed inner approach still schedules navigation retry"), Manager->bNeedsNavigationRetry);
	// 即使旧请求携带外圈 Kind1，也不能恢复无消费者的缓存或影响可用性判断。
	FFPEnemyQueryPoint LegacyOuter = CompletePoints[Manager->InnerSlotCount];
	LegacyOuter.Kind = 1;
	CompletePoints.Add(LegacyOuter);
	PublishComplete(CompletePoints);
	TestFalse(TEXT("An unused outer approach cannot force retry once all consumed goals are available"), Manager->bNeedsNavigationRetry);
	TestFalse(TEXT("A legacy outer approach result is not published"), Manager->SurroundSlots[Manager->InnerSlotCount].bHasProjectedApproachLocation);
	// 目标撤销后清空快照和所有站位，不允许下一轮复用旧玩家的分配。
	Manager->ApproachQueryId = PublishedQueryId + 2;
	const uint32 ClearedRevision = ++Manager->PositionQueryRevision;
	Manager->SetTargetCharacter(nullptr);
	Manager->HandleApproachQueryFinished(MakeResult(PublishedQueryId + 2, Points, true), ClearedRevision, FVector(190.0f, 0.0f, 0.0f));
	FVector Snapshot;
	TestFalse(TEXT("Clearing target invalidates the shared snapshot"), Manager->GetSharedTargetSnapshot(Snapshot));
	TestEqual(TEXT("Clearing target releases old allocations"), Manager->EnemyToSlot.Num(), 0);
	TestEqual(TEXT("Late success cannot restore query ownership after reset"), Manager->ApproachQueryId, INDEX_NONE);
	TestFalse(TEXT("Late success cannot republish slots after reset"), Manager->SurroundSlots[0].bHasProjectedApproachLocation);
	Manager->SetTargetCharacter(Target);
	const int32 EndingQueryId = Manager->ApproachQueryId;
	const uint32 EndingRevision = Manager->PositionQueryRevision;
	Manager->Destroy();
	Manager->HandleApproachQueryFinished(MakeResult(EndingQueryId, Points, true), EndingRevision, Target->GetActorLocation());
	TestFalse(TEXT("Late success after EndPlay cannot revive a shared snapshot"), Manager->GetSharedTargetSnapshot(Snapshot));
	TestEqual(TEXT("EndPlay releases every attack permit"), Manager->ActiveAttackers.Num(), 0);
	TestFalse(TEXT("EndPlay stops the shared target timer"), World->GetTimerManager().IsTimerActive(Manager->SharedTargetTimerHandle));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueSurroundTacticalQueueTest,
	"fpstrue.AI.Surround.TacticalQueueAndRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 固定已投影位置以隔离导航质量；请求、版本、FIFO、许可和回收均调用生产入口。
bool FFpstrueSurroundTacticalQueueTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginCombatTestWorld(Fixture, *this, true)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AfpstrueSurroundManager* Manager = World->SpawnActor<AfpstrueSurroundManager>();
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("Queue manager"), Manager) || !TestNotNull(TEXT("Queue target"), Target)) return false;
	Target->SetActorEnableCollision(false);
	Target->SetActorLocation(FVector::ZeroVector);
	Manager->SetTargetCharacter(Target);
	Manager->PublishedTargetLocation = Manager->CachedTargetLocation;
	const int32 OuterIndex = Manager->InnerSlotCount;
	Manager->SurroundSlots[0].bHasProjectedApproachLocation = true;
	Manager->SurroundSlots[0].ProjectedApproachLocation = FVector(120.0f, 0.0f, 0.0f);
	Manager->SurroundSlots[OuterIndex].bHasProjectedSlotLocation = true;
	Manager->SurroundSlots[OuterIndex].ProjectedSlotLocation = FVector(430.0f, 0.0f, 0.0f);
	const auto SpawnEnemy = [World](float X)
	{
		const FTransform Transform(FVector(X, 0.0f, 0.0f));
		AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
			AfpstrueEnemyCharacter::StaticClass(), Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Enemy != nullptr)
		{
			Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
			Enemy->FinishSpawning(Transform);
			Enemy->SetActorEnableCollision(false);
		}
		return Enemy;
	};
	AfpstrueEnemyCharacter* First = SpawnEnemy(1000.0f);
	AfpstrueEnemyCharacter* Second = SpawnEnemy(1200.0f);
	AfpstrueEnemyCharacter* Third = SpawnEnemy(1400.0f);
	AfpstrueEnemyCharacter* Fourth = SpawnEnemy(1600.0f);
	if (!TestNotNull(TEXT("First"), First) || !TestNotNull(TEXT("Second"), Second)
		|| !TestNotNull(TEXT("Third"), Third) || !TestNotNull(TEXT("Fourth"), Fourth)) return false;
	FFPEnemyTacticalAssignment A, B, C, D;
	if (!Manager->GetOrRequestTacticalAssignment(First, A) || !Manager->GetOrRequestTacticalAssignment(Second, B)
		|| !Manager->GetOrRequestTacticalAssignment(Third, C) || !Manager->GetOrRequestTacticalAssignment(Fourth, D)) return false;
	TestTrue(TEXT("First valid request obtains the only inner position"), A.Role == EFPEnemyTacticalRole::Inner);
	TestTrue(TEXT("Second request holds outer while still queuing for inner"), B.Role == EFPEnemyTacticalRole::Outer);
	TestTrue(TEXT("Overflow receives a real peripheral waiting goal"), C.Role == EFPEnemyTacticalRole::Waiting && C.Goal.Size2D() > Manager->OuterRadius);
	TestEqual(TEXT("Only requests without inner eligibility remain queued"), Manager->WaitingQueue.Num(), 3);
	const uint64 SecondSequence = Manager->Participants.FindChecked(Second).RequestSequence;
	FFPEnemyTacticalAssignment Repeated;
	Manager->GetOrRequestTacticalAssignment(Second, Repeated);
	TestEqual(TEXT("Repeated decisions do not requeue"), Manager->WaitingQueue.Num(), 3);
	TestEqual(TEXT("Repeated decisions preserve FIFO identity"), Manager->Participants.FindChecked(Second).RequestSequence, SecondSequence);
	TestEqual(TEXT("Repeated decisions preserve assignment revision"), Repeated.Revision, B.Revision);
	TestTrue(TEXT("Inner lease permits one attack transaction"), Manager->TryAcquireAttackPermission(First));
	TestFalse(TEXT("Outer occupancy is not permission to attack"), Manager->TryAcquireAttackPermission(Second));
	Manager->bEnableActiveAttackerBudget = false;
	TestFalse(TEXT("Disabling concurrency throttling cannot bypass inner eligibility"), Manager->TryAcquireAttackPermission(Third));
	Manager->bEnableActiveAttackerBudget = true;
	Manager->ReleaseAttackPermission(First);
	TestTrue(TEXT("Ending one swing retains the long-lived inner lease"), Manager->HasInnerSlot(First, A.Revision));
	Manager->ReleaseSurroundSlot(First);
	Manager->GetOrRequestTacticalAssignment(Second, Repeated);
	TestTrue(TEXT("FIFO outer holder advances to the freed inner position"), Repeated.Role == EFPEnemyTacticalRole::Inner);
	TestFalse(TEXT("Promotion invalidates the previous outer revision"), Manager->HasInnerSlot(Second, B.Revision));
	FVector Goal;
	TestFalse(TEXT("An old movement assignment cannot resolve after promotion"), Manager->ResolveTacticalMoveGoal(Second, B, Goal));
	Manager->ReportTacticalMoveFailure(Second, B.Revision);
	TestTrue(TEXT("Old failure callback cannot release a newer lease"), Manager->HasInnerSlot(Second, Repeated.Revision));
	Manager->GetOrRequestTacticalAssignment(Third, C);
	TestTrue(TEXT("Vacated outer position is filled from the same FIFO"), C.Role == EFPEnemyTacticalRole::Outer);
	const uint64 Released = Manager->TacticalReleaseCount;
	const uint64 Granted = Manager->TacticalGrantCount;
	Manager->ReleaseSurroundSlot(First);
	TestEqual(TEXT("Repeated release does not release resources twice"), Manager->TacticalReleaseCount, Released);
	TestEqual(TEXT("Repeated release does not grant duplicate positions"), Manager->TacticalGrantCount, Granted);
	const TWeakObjectPtr<AfpstrueEnemyCharacter> DeadKey(Second);
	Second->Destroy();
	Manager->CleanupInvalidEntries();
	Manager->PumpTacticalQueue();
	TestFalse(TEXT("Destroyed participant is erased by its original weak identity"), Manager->Participants.Contains(DeadKey));
	Manager->GetOrRequestTacticalAssignment(Third, C);
	Manager->GetOrRequestTacticalAssignment(Fourth, D);
	TestTrue(TEXT("Weak-key cleanup also triggers the next inner promotion"), C.Role == EFPEnemyTacticalRole::Inner);
	TestTrue(TEXT("Promotion cascades a waiting participant into outer"), D.Role == EFPEnemyTacticalRole::Outer);
	Manager->Participants.FindChecked(Third).LastProgressTime -= Manager->SlotApproachTimeout + 1.0;
	Manager->CleanupInvalidEntries();
	Manager->PumpTacticalQueue();
	Manager->GetOrRequestTacticalAssignment(Third, Repeated);
	TestTrue(TEXT("No-progress holder returns to stable peripheral waiting during retry delay"), Repeated.Role == EFPEnemyTacticalRole::Waiting);
	TestTrue(TEXT("Timeout preserves the participant's original peripheral location"), Repeated.Goal.Equals(Manager->CachedTargetLocation + Manager->Participants.FindChecked(Third).WaitingOffset));
	TestEqual(TEXT("No-progress timeout is observable"), Manager->TacticalTimeoutCount, static_cast<uint64>(1));
	TestTrue(TEXT("Next FIFO request receives the freed inner lease"), Manager->HasInnerSlot(Fourth));
	const uint64 TimedOutRevision = Repeated.Revision;
	Manager->Participants.FindChecked(Third).RetryAfter = World->GetTimeSeconds() - 1.0;
	Manager->PumpTacticalQueue();
	Manager->GetOrRequestTacticalAssignment(Third, Repeated);
	TestTrue(TEXT("Retry expiry permits outer admission without stealing the current inner"), Repeated.Role == EFPEnemyTacticalRole::Outer);
	TestTrue(TEXT("Readmission has a new assignment version"), Repeated.Revision != TimedOutRevision);
	Manager->GetOrRequestTacticalAssignment(Fourth, D);
	Fourth->SetActorLocation(D.Goal);
	Manager->Participants.FindChecked(Fourth).LastProgressTime -= 100.0;
	Manager->CleanupInvalidEntries();
	TestTrue(TEXT("Arrived holder retains its lease across individual attacks"), Manager->HasInnerSlot(Fourth, D.Revision));
	Manager->ReportTacticalMoveFailure(Fourth, D.Revision);
	TestFalse(TEXT("Current unreachable report revokes its old inner identity"), Manager->HasInnerSlot(Fourth, D.Revision));
	TestTrue(TEXT("Unreachable recovery gives the next request a chance"), Manager->HasInnerSlot(Third));
	// 目标离场后不再补位，但释放事件仍须在同一次调用里排空，不能依赖下一轮 Timer。
	Target->Destroy();
	const uint64 GrantsBeforeTargetLoss = Manager->TacticalGrantCount;
	Manager->ReleaseSurroundSlot(Third);
	TestFalse(TEXT("Release removes the participant after target departure"), Manager->Participants.Contains(Third));
	TestTrue(TEXT("Target departure cannot strand a pending release notification"), Manager->PendingTacticalNotifications.IsEmpty());
	TestEqual(TEXT("Target departure prevents new grants"), Manager->TacticalGrantCount, GrantsBeforeTargetLoss);
	Manager->SetTargetCharacter(nullptr);
	const FFPEnemyTacticalDebugSnapshot Empty = Manager->GetTacticalDebugSnapshot();
	TestEqual(TEXT("Target reset removes every queue request"), Empty.QueuedEnemies, 0);
	TestEqual(TEXT("Target reset removes every occupied slot"), Empty.OccupiedInnerSlots + Empty.OccupiedOuterSlots, 0);
	TestEqual(TEXT("Target reset removes every attack permit"), Empty.ActiveAttackers, 0);
	TestFalse(TEXT("Old assignment cannot resolve after target reset"), Manager->ResolveTacticalMoveGoal(Third, C, Goal));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueSurroundTacticalBoundaryTest,
	"fpstrue.AI.Surround.TacticalBoundaryAndAssignments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 验证资格/终点/路径段，明确不把 RVO、击退或玩家靠近造成的被动越界说成物理空气墙。
bool FFpstrueSurroundTacticalBoundaryTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginCombatTestWorld(Fixture, *this, true)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AfpstrueSurroundManager* Manager = World->SpawnActor<AfpstrueSurroundManager>();
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("Boundary manager"), Manager) || !TestNotNull(TEXT("Boundary target"), Target)) return false;
	Target->SetActorEnableCollision(false);
	Target->SetActorLocation(FVector::ZeroVector);
	Manager->SetTargetCharacter(Target);
	Manager->PublishedTargetLocation = Manager->CachedTargetLocation;
	Manager->SurroundSlots[0].bHasProjectedApproachLocation = true;
	Manager->SurroundSlots[0].ProjectedApproachLocation = FVector(120.0f, 0.0f, 0.0f);
	const int32 OuterIndex = Manager->InnerSlotCount;
	Manager->SurroundSlots[OuterIndex].bHasProjectedSlotLocation = true;
	Manager->SurroundSlots[OuterIndex].ProjectedSlotLocation = FVector(430.0f, 0.0f, 0.0f);
	Manager->SurroundSlots[OuterIndex].ProjectedApproachLocation = FVector(220.0f, 0.0f, 0.0f);
	const auto SpawnEnemy = [World]()
	{
		AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(),
			FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Enemy != nullptr) { Enemy->AutoPossessAI = EAutoPossessAI::Disabled; Enemy->FinishSpawning(FTransform::Identity); Enemy->SetActorEnableCollision(false); }
		return Enemy;
	};
	AfpstrueEnemyCharacter* Inner = SpawnEnemy();
	AfpstrueEnemyCharacter* Outer = SpawnEnemy();
	if (!TestNotNull(TEXT("Inner"), Inner) || !TestNotNull(TEXT("Outer"), Outer)) return false;
	FFPEnemyTacticalAssignment InnerAssignment, OuterAssignment;
	Manager->GetOrRequestTacticalAssignment(Inner, InnerAssignment);
	Manager->GetOrRequestTacticalAssignment(Outer, OuterAssignment);
	TestTrue(TEXT("A projected standing point alone grants the outer seat"), OuterAssignment.Role == EFPEnemyTacticalRole::Outer);
	TestTrue(TEXT("Outer uses its standing point rather than the inward attack approach"), OuterAssignment.Goal.Equals(FVector(430.0f, 0.0f, 0.0f)));
	Outer->SetActorLocation(FVector(-430.0f, 0.0f, 0.0f));
	FNavigationPath Crossing(TArray<FVector>{Outer->GetActorLocation(), OuterAssignment.Goal});
	TestFalse(TEXT("An outside endpoint does not authorize a chord through the inner circle"), Manager->IsTacticalPathAllowed(Outer, Crossing));
	FVector Resolved;
	if (!TestTrue(TEXT("Outer resolver offers an arc waypoint"), Manager->ResolveTacticalMoveGoal(Outer, OuterAssignment, Resolved))) return false;
	FNavigationPath Arc(TArray<FVector>{Outer->GetActorLocation(), Resolved});
	TestTrue(TEXT("Arc's first segment remains outside the protected circle"), Manager->IsTacticalPathAllowed(Outer, Arc));
	FNavigationPath Around(TArray<FVector>{Outer->GetActorLocation(), FVector(-430.0f, 500.0f, 0.0f), FVector(430.0f, 500.0f, 0.0f), OuterAssignment.Goal});
	TestTrue(TEXT("A multi-segment route outside the boundary is accepted"), Manager->IsTacticalPathAllowed(Outer, Around));
	Outer->SetActorLocation(FVector(100.0f, 0.0f, 0.0f));
	TestTrue(TEXT("Passive entry can resolve an outward recovery goal"), Manager->ResolveTacticalMoveGoal(Outer, OuterAssignment, Resolved));
	FNavigationPath Outward(TArray<FVector>{Outer->GetActorLocation(), Resolved});
	TestTrue(TEXT("Passive entry permits monotonic outward movement"), Manager->IsTacticalPathAllowed(Outer, Outward));
	FNavigationPath Inward(TArray<FVector>{Outer->GetActorLocation(), FVector(50.0f, 0.0f, 0.0f), Resolved});
	TestFalse(TEXT("Passive entry does not permit moving deeper before leaving"), Manager->IsTacticalPathAllowed(Outer, Inward));
	Inner->SetActorLocation(FVector(-430.0f, 0.0f, 0.0f));
	TestTrue(TEXT("A genuine inner holder is allowed to approach through the circle"), Manager->IsTacticalPathAllowed(Inner, Crossing));
	Manager->ReleaseSurroundSlot(Inner);
	TestFalse(TEXT("Outer assignment becomes stale when promoted"), Manager->ResolveTacticalMoveGoal(Outer, OuterAssignment, Resolved));
	Manager->GetOrRequestTacticalAssignment(Outer, OuterAssignment);
	TestTrue(TEXT("New inner assignment uses the actual attack point"), OuterAssignment.Role == EFPEnemyTacticalRole::Inner && OuterAssignment.Goal.Equals(FVector(120.0f, 0.0f, 0.0f)));
	const uint64 LeaseRevision = OuterAssignment.Revision;
	Target->SetActorLocation(FVector(50.0f, 0.0f, 0.0f));
	Manager->SharedTargetSnapshotTime -= Manager->SharedTargetMaxAge + 0.1;
	Manager->UpdateSharedTargetSnapshot(false);
	TestTrue(TEXT("Player movement updates geometry without reassigning slot identity"), Manager->HasInnerSlot(Outer, LeaseRevision));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFPEnemyBehaviorStartupTest,
	"fpstrue.AI.BehaviorTree.StartupFailureAndStopIdempotence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 验证无效行为树拒绝激活、上下文不能复活失败实例，以及 StopAI 重复调用不会重复释放资源。
bool FFPEnemyBehaviorStartupTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginCombatTestWorld(Fixture, *this, true)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(),
		FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("Startup enemy"), Enemy) || !TestNotNull(TEXT("Startup controller"), Controller) || !TestNotNull(TEXT("Startup target"), Target)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(FTransform::Identity);
	Enemy->SetActorEnableCollision(false);
	Target->SetActorEnableCollision(false);
	UBehaviorTree* InvalidTree = NewObject<UBehaviorTree>(Controller);
	InvalidTree->BlackboardAsset = NewObject<UBlackboardData>(InvalidTree);
	Controller->BehaviorTreeAsset = InvalidTree;
	AddExpectedError(TEXT("has an incompatible Blackboard; activation rejected"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("No valid enemy BehaviorTree is available"), EAutomationExpectedErrorFlags::Contains, 1);
	Controller->Possess(Enemy);
	TestFalse(TEXT("An incompatible tree never becomes activation-ready"), Controller->IsBehaviorReady());
	TestFalse(TEXT("Failed startup closes combat commands"), Controller->AcceptsCombatCommands());
	Controller->InitializeCombatContext(Target, nullptr);
	TestFalse(TEXT("Context injection cannot resurrect failed behavior startup"), Controller->AcceptsCombatCommands());
	TestNull(TEXT("Failed startup cannot publish a target through context injection"), Controller->TargetCharacter.Get());
	const uint32 FailedStopRevision = Controller->MoveRequestRevision;
	Controller->StopAI();
	Controller->StopAI();
	TestEqual(TEXT("Repeated completed stops do not repeat movement cleanup"), Controller->MoveRequestRevision, FailedStopRevision);
	Controller->UnPossess();
	Controller->BehaviorTreeAsset = nullptr;
	Controller->Possess(Enemy);
	TestTrue(TEXT("A new Possess can start the valid native tree"), Controller->IsBehaviorReady());
	Controller->InitializeCombatContext(Target, nullptr);
	Controller->StopAI();
	const uint32 CompletedStopRevision = Controller->MoveRequestRevision;
	Controller->StopAI();
	TestEqual(TEXT("Successful lifecycle also stops idempotently"), Controller->MoveRequestRevision, CompletedStopRevision);
	TestFalse(TEXT("Completed stop really stops the brain"), Controller->GetBrainComponent()->IsRunning());
	Controller->InitializeCombatContext(Target, nullptr);
	TestTrue(TEXT("Explicit restart of a previously successful lifecycle reruns the tree"), Controller->IsBehaviorReady());
	TestTrue(TEXT("Explicit restart publishes only its new target"), Controller->TargetCharacter.Get() == Target);
	Controller->StopAI();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyPositionQueryIdentityTest,
	"fpstrue.AI.EQS.TemplateAndItemIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 导航测试会改写原生 Point 前缀；槽位身份与候选类别必须跟随原项保留，模板也不随敌人请求重复创建。
bool FFpstrueEnemyPositionQueryIdentityTest::RunTest(const FString& Parameters)
{
	UEnvQuery* QueryOwner = NewObject<UEnvQuery>();
	UEnvQuery* Attack = FPCreateEnemyPositionQuery(*QueryOwner, false);
	UEnvQuery* Cover = FPCreateEnemyPositionQuery(*QueryOwner, true);
	TestNotNull(TEXT("Attack query template exists"), Attack);
	TestNotNull(TEXT("Cover query template exists"), Cover);
	TestTrue(TEXT("Attack requests reuse one unchanged template"), FPCreateEnemyPositionQuery(*QueryOwner, false) == Attack);
	TestTrue(TEXT("Cover requests reuse one unchanged template"), FPCreateEnemyPositionQuery(*QueryOwner, true) == Cover);
	TestTrue(TEXT("Attack and cover retain distinct query templates"), Attack != Cover);
	const UfpstrueEnvQueryItemType_Position* ItemType = GetDefault<UfpstrueEnvQueryItemType_Position>();
	TArray<uint8> RawData;
	RawData.AddZeroed(ItemType->GetValueSize());
	FFPEnemyQueryPoint Point;
	Point.Nav = FNavLocation(FVector(10.0f, 20.0f, 30.0f));
	Point.SlotIndex = 17;
	Point.Kind = 1;
	Point.Preference = -0.2f;
	UfpstrueEnvQueryItemType_Position::SetValue(RawData.GetData(), Point);
	const FNavLocation Projected(FVector(40.0f, 50.0f, 60.0f));
	ItemType->SetItemNavLocation(RawData.GetData(), Projected);
	const FFPEnemyQueryPoint Updated = UfpstrueEnvQueryItemType_Position::GetValue(RawData.GetData());
	TestTrue(TEXT("Navigation updates the item's actual location"), Updated.Nav.Location.Equals(Projected.Location));
	TestEqual(TEXT("Navigation preserves the owning slot identity"), Updated.SlotIndex, Point.SlotIndex);
	TestEqual(TEXT("Navigation preserves the candidate kind"), Updated.Kind, Point.Kind);
	TestEqual(TEXT("Navigation preserves the candidate preference"), Updated.Preference, Point.Preference);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyPositionQueryRuntimeTest,
	"fpstrue.AI.EQS.AsyncParametersAndMissingNavigation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 推进真实异步 EQS；生成器数量验证与生产导航过滤分别运行，缺快照和无 NavMesh 都不能制造可用点。
bool FFpstrueEnemyPositionQueryRuntimeTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginCombatTestWorld(Fixture, *this, true)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(),
		FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("EQS uses a real enemy owner"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(FTransform::Identity);
	Enemy->SetActorEnableCollision(false);
	Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
	UEnvQueryManager* Queries = UEnvQueryManager::GetCurrent(World);
	if (!TestNotNull(TEXT("The temporary world has the real EQS manager"), Queries)) return false;
	UEnvQuery* GeneratorOnly = NewObject<UEnvQuery>(Enemy);
	UEnvQueryOption* GeneratorOption = NewObject<UEnvQueryOption>(GeneratorOnly);
	GeneratorOption->Generator = NewObject<UfpstrueEnvQueryGenerator_Positions>(GeneratorOption);
	GeneratorOnly->GetOptionsMutable().Add(GeneratorOption);
	UEnvQuery* Attack = FPCreateEnemyPositionQuery(*Enemy, false);
	UEnvQuery* Cover = FPCreateEnemyPositionQuery(*Enemy, true);
	UEnvQuery* Templates[] = {GeneratorOnly, GeneratorOnly, GeneratorOnly, Attack, Cover};
	TSharedPtr<FEnvQueryResult> Results[UE_ARRAY_COUNT(Templates)];
	TArray<int32> QueryIds;
	// 失败或提前返回也先撤销委托，世界清理不能访问已退出的局部结果数组。
	ON_SCOPE_EXIT
	{
		for (const int32 QueryId : QueryIds) Queries->AbortQuery(QueryId);
	};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Templates); ++Index)
	{
		FEnvQueryRequest Request(Templates[Index], Enemy);
		if (Index != 0)
		{
			Request.SetFloatParam(TEXT("OriginX"), 0.0f).SetFloatParam(TEXT("OriginY"), 0.0f).SetFloatParam(TEXT("OriginZ"), 0.0f);
			Request.SetFloatParam(TEXT("ThreatX"), 100.0f).SetFloatParam(TEXT("ThreatY"), 0.0f).SetFloatParam(TEXT("ThreatZ"), 0.0f);
			Request.SetFloatParam(TEXT("InnerCount"), 4.0f).SetFloatParam(TEXT("OuterCount"), 5.0f);
		}
		if (Index == 1) Request.SetFloatParam(TEXT("OriginX"), std::numeric_limits<float>::quiet_NaN());
		// 历史配置多带的参数允许被忽略，不能重新生成已删除的外圈接近点。
		if (Index == 2) Request.SetFloatParam(TEXT("OuterApproach"), 50.0f);
		if (Index == 4) Request.SetFloatParam(TEXT("Cover"), 1.0f);
		const int32 QueryId = Request.Execute(EEnvQueryRunMode::AllMatching,
			FQueryFinishedSignature::CreateLambda([&, Index](TSharedPtr<FEnvQueryResult> Result) { Results[Index] = Result; }));
		if (!TestTrue(TEXT("Each input case starts a genuine asynchronous EQS instance"), QueryId != INDEX_NONE)) return false;
		QueryIds.Add(QueryId);
	}
	bool bAllFinished = false;
	for (int32 Frame = 0; Frame < 200 && !bAllFinished; ++Frame)
	{
		if (!Fixture.TickTestWorld(0.01f)) { Fixture.ForwardErrorMessages(this); return false; }
		bAllFinished = true;
		for (const TSharedPtr<FEnvQueryResult>& Result : Results) bAllFinished &= Result.IsValid();
	}
	if (!TestTrue(TEXT("The real EQS loop completes every bounded input case"), bAllFinished)) return false;
	TestEqual(TEXT("Missing position snapshots generate no candidates"), Results[0]->Items.Num(), 0);
	TestEqual(TEXT("Non-finite position snapshots generate no candidates"), Results[1]->Items.Num(), 0);
	TestTrue(TEXT("Valid float count parameters complete generation"), Results[2]->IsSuccessful());
	TestEqual(TEXT("Four inner slots generate four candidates each; five outer slots generate standing points only"), Results[2]->Items.Num(), 21);
	int32 InnerApproaches = 0, StandingPoints = 0;
	for (int32 Index = 0; Index < Results[2]->Items.Num(); ++Index)
	{
		const FFPEnemyQueryPoint& Candidate = Results[2]->GetItemAsTypeChecked<UfpstrueEnvQueryItemType_Position>(Index);
		if (Candidate.Kind == 0) ++StandingPoints;
		if (Candidate.Kind == 1)
		{
			++InnerApproaches;
			TestTrue(TEXT("Only inner slots generate LOS-tested approach candidates"), Candidate.SlotIndex < 4);
		}
	}
	TestEqual(TEXT("Every inner and outer slot retains its layout point"), StandingPoints, 9);
	TestEqual(TEXT("Only four inner slots generate three approach alternatives"), InnerApproaches, 12);
	TestFalse(TEXT("Production attack query cannot succeed without navigation"), Results[3]->IsSuccessful());
	TestFalse(TEXT("Production cover query cannot succeed without navigation"), Results[4]->IsSuccessful());
	TestEqual(TEXT("Missing navigation never publishes raw attack positions"), Results[3]->Items.Num(), 0);
	TestEqual(TEXT("Missing navigation never publishes raw cover positions"), Results[4]->Items.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyPositionQueryGcLifetimeTest,
	"fpstrue.AI.EQS.TwoOwnersInflightGcAndRecreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 真正入队的 EQS 在下一次 Tick 前跨过 GC；先用弱句柄检测旧图悬空，失败时 Abort，不能故意调用已释放生成器。
bool FFpstrueEnemyPositionQueryGcLifetimeTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginCombatTestWorld(Fixture, *this, true)) return false;
	UWorld* World = Fixture.GetTestWorld();
	TStrongObjectPtr<UWorld> KeepWorld(World);
	UEnvQueryManager* Queries = UEnvQueryManager::GetCurrent(World);
	if (!TestNotNull(TEXT("Lifetime fixture uses the real EQS manager"), Queries)) return false;
	const auto SpawnOwner = [&]()
	{
		auto* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(),
			FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Enemy != nullptr)
		{
			Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
			Enemy->FinishSpawning(FTransform::Identity);
			Enemy->SetActorEnableCollision(false);
			Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
		}
		return Enemy;
	};
	AfpstrueEnemyCharacter* Owners[2] = {SpawnOwner(), SpawnOwner()};
	if (!TestNotNull(TEXT("First real request owner"), Owners[0]) || !TestNotNull(TEXT("Second real request owner"), Owners[1])) return false;
	TStrongObjectPtr<UEnvQuery> HeldTemplates[4];
	for (int32 Index = 0; Index < 4; ++Index) HeldTemplates[Index].Reset(FPCreateEnemyPositionQuery(*Owners[Index / 2], Index % 2 != 0));
	TWeakObjectPtr<UEnvQuery> CanonicalAttack = HeldTemplates[0].Get(), CanonicalCover = HeldTemplates[1].Get();
	TSharedPtr<FEnvQueryInstance> Instances[8];
	TSharedPtr<FEnvQueryResult> Results[8];
	TWeakObjectPtr<UEnvQueryGenerator> Generators[8];
	TArray<int32> QueryIds;
	ON_SCOPE_EXIT
	{
		// 所有退出先撤销真实委托，局部结果数组和临时 World 才能安全离开作用域。
		for (const int32 QueryId : QueryIds) Queries->AbortQuery(QueryId);
	};
	const auto Queue = [&](int32 Index, AfpstrueEnemyCharacter* Owner, UEnvQuery* Template)
	{
		FEnvQueryRequest Request(Template, Owner);
		const float OriginX = Owner == Owners[0] ? 111.0f : 222.0f;
		Request.SetFloatParam(TEXT("OriginX"), OriginX).SetFloatParam(TEXT("OriginY"), 0).SetFloatParam(TEXT("OriginZ"), 0);
		Request.SetFloatParam(TEXT("ThreatX"), 1000).SetFloatParam(TEXT("ThreatY"), 0).SetFloatParam(TEXT("ThreatZ"), 0);
		Request.SetFloatParam(TEXT("Cover"), Index % 2 != 0 ? 1.0f : 0.0f);
		Request.SetFloatParam(TEXT("InnerCount"), 4).SetFloatParam(TEXT("OuterCount"), 5);
		Instances[Index] = Queries->PrepareQueryInstance(Request, EEnvQueryRunMode::AllMatching);
		if (!Instances[Index].IsValid() || Instances[Index]->Options.Num() != 1) return false;
		Generators[Index] = Instances[Index]->Options[0].Generator;
		TestTrue(TEXT("Request Owner remains per-instance, not on the shared graph"), Instances[Index]->Owner.Get() == Owner);
		TestEqual(TEXT("Origin parameter remains independent in every request"), Instances[Index]->NamedParams.FindChecked(TEXT("OriginX")), OriginX);
		const int32 QueryId = Queries->RunQuery(Instances[Index], FQueryFinishedSignature::CreateLambda(
			[&, Index](TSharedPtr<FEnvQueryResult> Result) { Results[Index] = Result; }));
		if (QueryId == INDEX_NONE) return false;
		QueryIds.Add(QueryId);
		return true;
	};
	const auto CheckGenerators = [&](int32 Count, const TCHAR* Phase)
	{
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		bool bAllValid = true;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const bool bValid = Generators[Index].IsValid();
			AddInfo(FString::Printf(TEXT("EQS_GC Phase=%s Query=%d GeneratorAlive=%d"), Phase, Index, bValid));
			bAllValid &= TestTrue(FString::Printf(TEXT("In-flight generator %d survives real GC during %s"), Index, Phase), bValid);
		}
		return bAllValid;
	};
	const auto Finish = [&](int32 First, int32 Last)
	{
		for (int32 Frame = 0; Frame < 200; ++Frame)
		{
			bool bFinished = true;
			for (int32 Index = First; Index < Last; ++Index) bFinished &= Results[Index].IsValid();
			if (bFinished) return true;
			if (!Fixture.TickTestWorld(0.01f)) { Fixture.ForwardErrorMessages(this); return false; }
		}
		return false;
	};
	for (int32 Index = 0; Index < 4; ++Index)
		if (!TestTrue(TEXT("The real manager queues each Owner/Attack/Cover combination"), Queue(Index, Owners[Index / 2], HeldTemplates[Index].Get()))) return false;
	// 旧代码在这里保留了原模板的强引用，仍会因同名 LocalTemplate 覆写而丢失旧缓存生成器。
	if (!CheckGenerators(4, TEXT("BothOwnersAlive"))) return false;
	Owners[0]->Destroy();
	HeldTemplates[0].Reset(); HeldTemplates[1].Reset();
	if (!CheckGenerators(4, TEXT("FirstOwnerDestroyed")) || !TestTrue(TEXT("All four genuine asynchronous queries finish"), Finish(0, 4))) return false;
	TestEqual(TEXT("Destroyed first Owner's attack completes as OwnerLost"), Results[0]->GetRawStatus(), EEnvQueryStatus::OwnerLost);
	TestEqual(TEXT("Destroyed first Owner's cover completes as OwnerLost"), Results[1]->GetRawStatus(), EEnvQueryStatus::OwnerLost);
	TestTrue(TEXT("Surviving Owner's attack still executes"), Results[2]->GetRawStatus() != EEnvQueryStatus::OwnerLost);
	TestTrue(TEXT("Surviving Owner's cover still executes"), Results[3]->GetRawStatus() != EEnvQueryStatus::OwnerLost);
	if (!Queue(4, Owners[1], HeldTemplates[2].Get()) || !Queue(5, Owners[1], HeldTemplates[3].Get())) return false;
	Owners[1]->Destroy();
	HeldTemplates[2].Reset(); HeldTemplates[3].Reset();
	if (!CheckGenerators(6, TEXT("AllOwnersDestroyed"))) return false;
	if (!TestTrue(TEXT("World keeps exactly the canonical attack template alive after every caller leaves"), CanonicalAttack.IsValid()) ||
		!TestTrue(TEXT("World keeps the canonical cover template alive after every caller leaves"), CanonicalCover.IsValid())) return false;
	auto* NewOwner = SpawnOwner();
	if (!TestNotNull(TEXT("A new real request Owner can join after collection"), NewOwner)) return false;
	HeldTemplates[0].Reset(FPCreateEnemyPositionQuery(*NewOwner, false));
	HeldTemplates[1].Reset(FPCreateEnemyPositionQuery(*NewOwner, true));
	TestTrue(TEXT("New Owner reuses canonical attack rather than replacing an in-flight local graph"), HeldTemplates[0].Get() == CanonicalAttack.Get());
	TestTrue(TEXT("New Owner reuses canonical cover rather than replacing an in-flight local graph"), HeldTemplates[1].Get() == CanonicalCover.Get());
	if (!Queue(6, NewOwner, HeldTemplates[0].Get()) || !Queue(7, NewOwner, HeldTemplates[1].Get()) ||
		!CheckGenerators(8, TEXT("NewOwnerQueued")) || !TestTrue(TEXT("Old and new Owner's real asynchronous requests finish"), Finish(4, 8))) return false;
	TestEqual(TEXT("Last old Owner's pending attack completes as OwnerLost"), Results[4]->GetRawStatus(), EEnvQueryStatus::OwnerLost);
	TestEqual(TEXT("Last old Owner's pending cover completes as OwnerLost"), Results[5]->GetRawStatus(), EEnvQueryStatus::OwnerLost);
	TestTrue(TEXT("New Owner's attack executes after GC/rejoin"), Results[6]->GetRawStatus() != EEnvQueryStatus::OwnerLost);
	TestTrue(TEXT("New Owner's cover executes after GC/rejoin"), Results[7]->GetRawStatus() != EEnvQueryStatus::OwnerLost);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyLowHealthRetreatTest,
	"fpstrue.AI.BehaviorTree.LowHealthRetreatLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 真实伤害事件请求撤退；异步掩体迟到、无路径与不回血都必须有界退出，并归还攻击和站位资源。
bool FFpstrueEnemyLowHealthRetreatTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginCombatTestWorld(Fixture, *this, true)) return false;
	UWorld* World = Fixture.GetTestWorld();
	const auto SpawnEnemy = [World]()
	{
		AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(),
			FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Enemy != nullptr)
		{
			Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
			Enemy->FinishSpawning(FTransform::Identity);
			Enemy->SetActorEnableCollision(false);
			Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
		}
		return Enemy;
	};
	AfpstrueEnemyCharacter* Enemy = SpawnEnemy();
	AfpstrueEnemyCharacter* Probe = SpawnEnemy();
	AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	AfpstrueSurroundManager* Surround = World->SpawnActor<AfpstrueSurroundManager>();
	if (!TestNotNull(TEXT("Retreat enemy"), Enemy) || !TestNotNull(TEXT("Permit probe"), Probe) ||
		!TestNotNull(TEXT("Retreat controller"), Controller) || !TestNotNull(TEXT("Known threat"), Target) ||
		!TestNotNull(TEXT("Cover manager"), Surround)) return false;
	Target->SetActorEnableCollision(false);
	Target->GetCharacterMovement()->SetComponentTickEnabled(false);
	Target->SetActorLocation(FVector(180.0f, 0.0f, 0.0f));
	Surround->SetTargetCharacter(Target);
	Controller->Possess(Enemy);
	Controller->InitializeCombatContext(Target, Surround);
	if (!ReportVisibleCombatTarget(Controller, Target, *this)) return false;
	DisableAutomaticBehaviorTick(*Controller->GetBrainComponent());
	Controller->RetreatTimeLimit = 0.3f;
	Controller->RetreatCooldown = 0.4f;
	Controller->RetreatDecisionInterval = 10.0f;
	Surround->MaxActiveAttackers = 1;
	UfpstrueHealthComponent* Health = Enemy->FindComponentByClass<UfpstrueHealthComponent>();
	UfpstrueEnemyCombatComponent* Combat = Enemy->GetCombatComponent();
	if (!TestNotNull(TEXT("Real health component"), Health) || !TestNotNull(TEXT("Real combat transaction"), Combat)) return false;
	Surround->PublishedTargetLocation = Surround->CachedTargetLocation;
	// 掩体异步身份照常运行；本夹具只固定接近点缓存，避免无 NavMesh 的位置查询覆盖测试输入。
	const int32 OldApproachQuery = Surround->ApproachQueryId;
	Surround->ApproachQueryId = INDEX_NONE;
	++Surround->PositionQueryRevision;
	if (UEnvQueryManager* Queries = UEnvQueryManager::GetCurrent(World)) Queries->AbortQuery(OldApproachQuery);
	Surround->bNeedsNavigationRetry = false;
	Surround->NavigationCacheMaxAge = 100.0f;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		FfpstrueSurroundSlot& Slot = Surround->SurroundSlots[Index];
		Slot.bHasProjectedSlotLocation = Slot.bHasProjectedApproachLocation = true;
		Slot.ProjectedSlotLocation = Surround->CachedTargetLocation + FVector(250.0f, Index * 250.0f, 0.0f);
		Slot.ProjectedApproachLocation = Enemy->GetNavAgentLocation() + FVector(0.0f, Index * 200.0f, 0.0f);
	}
	FFPEnemyTacticalAssignment Assignment;
	if (!TestTrue(TEXT("Enemy occupies a genuine inner lease before retreat"), Surround->GetOrRequestTacticalAssignment(Enemy, Assignment))
		|| !TestTrue(TEXT("Permit probe also owns an inner lease"), Surround->GetOrRequestTacticalAssignment(Probe, Assignment))) return false;
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	if (!TestTrue(TEXT("Healthy attacker owns its permit"), Surround->TryAcquireAttackPermission(Enemy)) ||
		!TestTrue(TEXT("Healthy attack begins through the real combat component"), Combat->TryAttackTarget())) return false;
	TestFalse(TEXT("Another living enemy cannot take the active attack permit"), Surround->TryAcquireAttackPermission(Probe));
	UGameplayStatics::ApplyDamage(Enemy, 75.0f, nullptr, nullptr, nullptr);
	TestTrue(TEXT("Real health notification requests retreat without editing AI flags"), Controller->bRetreatRequested);
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	TestTrue(TEXT("Low health is published in the independent retreat key"), Controller->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::Retreating));
	TestFalse(TEXT("An old attack leaf cannot bypass a pending retreat decision"), Controller->ExecuteBehaviorAction(EFPEnemyBehaviorAction::SustainAttack));
	// 只固定尚未完成的查询身份，检验 Controller 上限；无导航失败在后续真实 MoveTo 单独验证。
	const auto SeedPendingCover = [&]()
	{
		AfpstrueSurroundManager::FCoverRequest& Pending = Surround->CoverRequests.Add(Enemy);
		Pending.ThreatLocation = Controller->LastKnownTargetLocation;
		Pending.QueryId = 1000010;
		Pending.Revision = ++Surround->NextCoverRevision;
		Pending.State = EFPCoverQueryResult::Pending;
		Pending.StartedTime = World->GetTimeSeconds();
		Surround->PendingCoverQueries = 1;
		return Pending.Revision;
	};
	const uint32 FirstCoverRevision = SeedPendingCover();
	TestTrue(TEXT("Retreat starts while a cover result is pending"), Controller->ExecuteBehaviorAction(EFPEnemyBehaviorAction::Retreat));
	TestTrue(TEXT("A pending cover request keeps retreat active"), Controller->bRetreatActive);
	TestFalse(TEXT("Retreat cancels the old attack transaction"), Combat->IsAttacking());
	TestFalse(TEXT("Retreat releases the old surround slot"), Surround->EnemyToSlot.Contains(Enemy));
	TestTrue(TEXT("Retreat releases the captured attack permit while its owner lives"), Surround->TryAcquireAttackPermission(Probe));
	Surround->ReleaseAttackPermission(Probe);
	TestNull(TEXT("Active retreat closes the public combat target bridge"), Controller->GetTargetCharacter());
	TestFalse(TEXT("Direct combat calls cannot restart an attack during retreat"), Combat->TryAttackTarget());
	// Significance 只降低非紧急维护；首次/等待掩体保留响应，停留与总期限仍是硬退出条件。
	Controller->RetreatDecisionInterval = 0.25f;
	Controller->RetreatTimeLimit = 3.0f;
	Controller->RetreatEndTime = World->GetTimeSeconds() + 3.0;
	Controller->SetSignificanceDecisionMultiplier(4.0f);
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	TestEqual(TEXT("Waiting for cover keeps the urgent interval despite background significance"), Controller->GetBehaviorDecisionDelay(), 0.25f);
	Controller->bHasRetreatGoal = true;
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	TestEqual(TEXT("Existing cover maintenance follows the significance multiplier"), Controller->GetBehaviorDecisionDelay(), 1.0f);
	Controller->RetreatHoldUntil = World->GetTimeSeconds() + 0.1;
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	TestTrue(TEXT("Significance cannot delay the cover hold deadline"), Controller->GetBehaviorDecisionDelay() <= 0.101f);
	// 先采样维护间隔，再经真实 MoveTo 的立即到达分支设置停留期限；下一次等待必须立即缩短。
	Controller->RetreatHoldUntil = 0.0;
	Controller->RetreatHoldSeconds = 0.1f;
	Controller->RetreatDecisionInterval = 1.0f;
	Controller->RetreatTimeLimit = 10.0f;
	Controller->RetreatEndTime = World->GetTimeSeconds() + 10.0;
	Controller->RetreatGoalLocation = Controller->GetNavAgentLocation();
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	TestEqual(TEXT("Arrival fixture samples a four-second maintenance interval before reaching cover"), Controller->GetBehaviorDecisionDelay(), 4.0f);
	TestTrue(TEXT("Retreat executes the real immediate-arrival movement branch"), Controller->ExecuteBehaviorAction(EFPEnemyBehaviorAction::Retreat));
	TestTrue(TEXT("Real arrival establishes a new cover hold deadline"), Controller->RetreatHoldUntil > World->GetTimeSeconds());
	TestTrue(TEXT("Real arrival preserves the engine's completed move goal"), Controller->bHasMoveGoal);
	TestTrue(TEXT("Real arrival immediately limits the next wait to the short cover hold"), Controller->GetBehaviorDecisionDelay() <= 0.1f);
	Controller->bHasRetreatGoal = false;
	Controller->RetreatHoldUntil = 0.0;
	Controller->RetreatDecisionInterval = 10.0f;
	Controller->RetreatTimeLimit = 0.3f;
	Controller->RetreatEndTime = World->GetTimeSeconds() + 0.3;
	Controller->SetSignificanceDecisionMultiplier(1.0f);
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	TestTrue(TEXT("A long decision interval cannot wait past the retreat time limit"), Controller->GetBehaviorDecisionDelay() <= Controller->RetreatTimeLimit);
	if (!Fixture.TickTestWorld(0.0f) || !Fixture.TickTestWorld(0.35f)) { Fixture.ForwardErrorMessages(this); return false; }
	Controller->ExecuteBehaviorAction(EFPEnemyBehaviorAction::Retreat);
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	TestFalse(TEXT("A missing asynchronous cover result cannot hold retreat forever"), Controller->bRetreatActive);
	TestFalse(TEXT("Timed-out retreat clears the Blackboard flag"), Controller->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::Retreating));
	TestFalse(TEXT("Timed-out retreat releases pending cover ownership"), Surround->CoverRequests.Contains(Enemy));
	TSharedPtr<FEnvQueryResult> LateCover = MakeShared<FEnvQueryResult>();
	LateCover->QueryID = 1000010;
	LateCover->Owner = Enemy;
	LateCover->ItemType = UfpstrueEnvQueryItemType_Position::StaticClass();
	FFPEnemyQueryPoint CoverPoint;
	CoverPoint.Nav = FNavLocation(FVector(5000.0f, 0.0f, 0.0f));
	CoverPoint.Kind = 2;
	LateCover->RawData.AddZeroed(GetDefault<UfpstrueEnvQueryItemType_Position>()->GetValueSize());
	UfpstrueEnvQueryItemType_Position::SetValue(LateCover->RawData.GetData(), CoverPoint);
	LateCover->Items.Add(FEnvQueryItem(0));
	LateCover->Items.Last().Score = 1.0f;
	LateCover->MarkAsFinishedWithoutIssues();
	Surround->HandleCoverQueryFinished(LateCover, Enemy, FirstCoverRevision);
	TestFalse(TEXT("A late success cannot recreate released cover ownership"), Surround->CoverRequests.Contains(Enemy));
	TestEqual(TEXT("A late success cannot recreate the pending cover count"), Surround->PendingCoverQueries, 0);
	TestTrue(TEXT("The enemy still has low health without any automatic healing"), Health->GetHealthNormalized() < Controller->LowHealthRetreatThreshold);
	TestTrue(TEXT("An unsuccessful retreat restores the visible target bridge"), Controller->GetTargetCharacter() == Target);
	UGameplayStatics::ApplyDamage(Enemy, 1.0f, nullptr, nullptr, nullptr);
	TestFalse(TEXT("Damage during cooldown cannot immediately restart retreat"), Controller->bRetreatRequested);
	if (!Fixture.TickTestWorld(0.5f)) { Fixture.ForwardErrorMessages(this); return false; }
	for (int32 Sample = 0; Sample < 4; ++Sample) Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	TestFalse(TEXT("Cooldown expiry alone does not repeatedly retreat without healing"), Controller->bRetreatRequested || Controller->bRetreatActive);
	TestTrue(TEXT("Another normal attack remains possible after bounded retreat failure"), Combat->TryAttackTarget());
	Combat->ResetCombat();
	UGameplayStatics::ApplyDamage(Enemy, 1.0f, nullptr, nullptr, nullptr);
	TestTrue(TEXT("Fresh damage after cooldown can request another retreat"), Controller->bRetreatRequested);
	AfpstrueSurroundManager::FCoverRequest& UnreachableCover = Surround->CoverRequests.Add(Enemy);
	UnreachableCover.ThreatLocation = Controller->LastKnownTargetLocation;
	UnreachableCover.Location = FVector(5000.0f, 0.0f, 0.0f);
	UnreachableCover.State = EFPCoverQueryResult::Found;
	UnreachableCover.StartedTime = World->GetTimeSeconds();
	Controller->ExecuteBehaviorAction(EFPEnemyBehaviorAction::Retreat);
	TestFalse(TEXT("A real engine path failure exits retreat instead of holding the goal forever"), Controller->bRetreatActive);
	TestFalse(TEXT("An unreachable cover point releases its reservation"), Surround->CoverRequests.Contains(Enemy));
	if (!Fixture.TickTestWorld(0.5f)) { Fixture.ForwardErrorMessages(this); return false; }
	UGameplayStatics::ApplyDamage(Enemy, 1.0f, nullptr, nullptr, nullptr);
	SeedPendingCover();
	Controller->ExecuteBehaviorAction(EFPEnemyBehaviorAction::Retreat);
	TestTrue(TEXT("A new pending retreat starts before healing"), Controller->bRetreatActive);
	Surround->HandleCoverQueryFinished(LateCover, Enemy, FirstCoverRevision);
	TestTrue(TEXT("A reused query ID with an old cover revision cannot replace the new pending request"),
		Surround->CoverRequests.FindChecked(Enemy).State == EFPCoverQueryResult::Pending);
	Health->ResetHealth();
	Controller->RefreshBehaviorDecision(*Controller->GetBlackboardComponent());
	TestFalse(TEXT("Real health restoration exits active retreat"), Controller->bRetreatActive || Controller->bRetreatRequested);
	TestFalse(TEXT("Health restoration also releases cover ownership"), Surround->CoverRequests.Contains(Enemy));
	Controller->StopAI();
	UGameplayStatics::ApplyDamage(Enemy, 75.0f, nullptr, nullptr, nullptr);
	TestFalse(TEXT("Health notifications after StopAI cannot restore retreat commands"), Controller->bRetreatRequested);
	TestFalse(TEXT("StopAI clears retreat in the reused Blackboard"), Controller->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::Retreating));
	// 新 Possess 重新订阅血量事件；真实树正在长 Idle 等待时，受伤必须提前唤醒采样。
	Health->ResetHealth();
	Controller->UnPossess();
	Controller->IdleDecisionInterval = 10.0f;
	Controller->GetBrainComponent()->PrimaryComponentTick.bCanEverTick = true;
	Controller->Possess(Enemy);
	Controller->GetBrainComponent()->SetComponentTickEnabled(true);
	Target->SetActorLocation(FVector(100000.0f, 0.0f, 0.0f));
	Controller->InitializeCombatContext(Target, Surround);
	if (!ReportVisibleCombatTarget(Controller, Target, *this)) return false;
	for (int32 Frame = 0; Frame < 40; ++Frame)
	{
		if (!Fixture.TickTestWorld(0.01f)) { Fixture.ForwardErrorMessages(this); return false; }
	}
	TestTrue(TEXT("The real behavior tree is running before the health wake event"), Controller->GetBrainComponent()->IsRunning());
	TestEqual(TEXT("The far visible threat leaves a ten-second idle decision wait"), Controller->GetBehaviorDecisionDelay(), 10.0f);
	UGameplayStatics::ApplyDamage(Enemy, 75.0f, nullptr, nullptr, nullptr);
	if (!Fixture.TickTestWorld(0.02f)) { Fixture.ForwardErrorMessages(this); return false; }
	TestTrue(TEXT("The health event wakes a ten-second behavior wait for a real retreat sample"),
		Controller->GetBlackboardComponent()->GetValueAsBool(FPEnemyBlackboard::Retreating));
	Controller->StopAI();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyCombatStateConfigTest,
	"fpstrue.Gameplay.Combat.ConfigurationSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 检查无资产时保留旧蓝图参数，有资产时整份战斗参数统一取自资产且运行中不自动热改。
bool FFpstrueEnemyCombatStateConfigTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginCombatTestWorld(Fixture, *this)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AActor* Owner = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Temporary configuration owner"), Owner)) return false;
	UfpstrueEnemyCombatComponent* Legacy = NewObject<UfpstrueEnemyCombatComponent>(Owner);
	Legacy->AttackRange = 321.0f;
	Legacy->AttackDamage = 13.0f;
	Owner->AddInstanceComponent(Legacy);
	Legacy->RegisterComponent();
	TestEqual(TEXT("No asset preserves the existing Blueprint range"), Legacy->AttackRange, 321.0f);
	TestEqual(TEXT("No asset preserves the existing Blueprint damage"), Legacy->AttackDamage, 13.0f);

	// 用与旧字段不同的值覆盖范围、伤害、时序和刀刃采样，证明没有混合多个配置来源。
	UfpstrueEnemyCombatConfig* Configuration = NewObject<UfpstrueEnemyCombatConfig>(Owner);
	Configuration->AttackRange = 654.0f;
	Configuration->AttackDamage = 17.0f;
	Configuration->AttackInterval = 2.0f;
	Configuration->AttackAnimationDuration = 1.7f;
	Configuration->AttackFailSafeDuration = 6.0f;
	Configuration->AttackCompletionGracePeriod = 0.3f;
	Configuration->WeaponTraceStartSocketName = TEXT("BladeRoot");
	Configuration->WeaponTraceEndSocketName = TEXT("BladeTip");
	Configuration->WeaponTraceRadius = 12.0f;
	Configuration->WeaponTraceSampleCount = 6;
	UfpstrueEnemyCombatComponent* Configured = NewObject<UfpstrueEnemyCombatComponent>(Owner);
	Configured->CombatConfiguration = Configuration;
	Configured->AttackRange = 999.0f;
	Configured->bDrawAttackTrace = true;
	Owner->AddInstanceComponent(Configured);
	Configured->RegisterComponent();
	TestEqual(TEXT("The asset wins over legacy Blueprint range"), Configured->AttackRange, Configuration->AttackRange);
	TestEqual(TEXT("Damage comes from the same source"), Configured->AttackDamage, Configuration->AttackDamage);
	TestEqual(TEXT("Cooldown comes from the same source"), Configured->AttackInterval, Configuration->AttackInterval);
	TestEqual(TEXT("Animation duration comes from the same source"), Configured->AttackAnimationDuration, Configuration->AttackAnimationDuration);
	TestEqual(TEXT("Fail-safe comes from the same source"), Configured->AttackFailSafeDuration, Configuration->AttackFailSafeDuration);
	TestEqual(TEXT("Grace period comes from the same source"), Configured->AttackCompletionGracePeriod, Configuration->AttackCompletionGracePeriod);
	TestEqual(TEXT("Start Socket comes from the same source"), Configured->WeaponTraceStartSocketName, Configuration->WeaponTraceStartSocketName);
	TestEqual(TEXT("End Socket comes from the same source"), Configured->WeaponTraceEndSocketName, Configuration->WeaponTraceEndSocketName);
	TestEqual(TEXT("Trace radius comes from the same source"), Configured->WeaponTraceRadius, Configuration->WeaponTraceRadius);
	TestEqual(TEXT("Sample count comes from the same source"), Configured->WeaponTraceSampleCount, Configuration->WeaponTraceSampleCount);
	TestTrue(TEXT("The local debug switch is not gameplay configuration"), Configured->bDrawAttackTrace);
	TestEqual(TEXT("BeginPlay initializes cooldown using the selected interval"),
		Configured->LastAttackTime, static_cast<float>(World->GetTimeSeconds() - Configuration->AttackInterval));
	Configuration->AttackDamage = 99.0f;
	TestEqual(TEXT("Asset edits do not silently hot-reload an active instance"), Configured->AttackDamage, 17.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyAttackPlaybackTest,
	"fpstrue.Gameplay.Combat.ExplicitPlaybackAndStopReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 用真实 Montage 实例验证动作/播放双重身份、旧 Notify 拒绝、停止重入和实际播放时长保护。
bool FFpstrueEnemyAttackPlaybackTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Fixture;
	if (!BeginCombatTestWorld(Fixture, *this, true)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(AfpstrueEnemyCharacter::StaticClass(),
		FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Enemy"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	Enemy->FinishSpawning(FTransform::Identity);
	Enemy->SetActorEnableCollision(false);
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
	if (!TestNotNull(TEXT("Target"), Target) || !TestNotNull(TEXT("Controller"), Controller)) return false;
	Target->SetActorEnableCollision(false);
	Target->SetActorLocation(FVector(200.0, 0.0, 0.0));
	Controller->Possess(Enemy);
	Controller->InitializeCombatContext(Target, nullptr);
	if (!ReportVisibleCombatTarget(Controller, Target, *this)) return false;
	DisableAutomaticBehaviorTick(*Controller->GetBrainComponent());
	UfpstrueEnemyCombatComponent* Combat = Enemy->GetCombatComponent();
	USkeletalMeshComponent* Mesh = Enemy->GetMesh();
	USkeletalMesh* BladeMesh = NewObject<USkeletalMesh>(Enemy);
	{
		FReferenceSkeletonModifier Modifier(BladeMesh->GetRefSkeleton(), nullptr);
		Modifier.Add(FMeshBoneInfo(TEXT("weapontop"), TEXT("weapontop"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("weaponend"), TEXT("weaponend"), 0), FTransform(FVector(50.0, 0.0, 0.0)));
	}
	BladeMesh->CalculateInvRefMatrices();
	USkeleton* PlaybackSkeleton = NewObject<USkeleton>(Enemy);
	BladeMesh->SetSkeleton(PlaybackSkeleton);
	PlaybackSkeleton->MergeAllBonesToBoneTree(BladeMesh);
	Mesh->SetSkeletalMesh(BladeMesh);
	// 引擎真实分配 Montage 实例；不向 MontageInstances/事务字段注入伪造播放 ID。
	UAnimInstance* Anim = NewObject<UAnimInstance>(Mesh);
	Mesh->AnimScriptInstance = Anim;
	Anim->InitializeAnimation();
	// 构造不同组的可播放 Montage，保留引擎的真实实例分配及结束回调，不手工造播放 ID。
	const auto MakeMontage = [Anim](FName Slot, FName Group)
	{
		Anim->CurrentSkeleton->SetSlotGroupName(Slot, Group);
		UAnimMontage* Montage = NewObject<UAnimMontage>(Anim);
		Montage->SetSkeleton(Anim->CurrentSkeleton);
		Montage->SetCompositeLength(2.0f);
		Montage->SlotAnimTracks[0].SlotName = Slot;
		// 运行时数据构造，不依赖 WITH_EDITOR 的 AddAnimCompositeSection 编辑器辅助接口。
		FCompositeSection& Section = Montage->CompositeSections.AddDefaulted_GetRef();
		Section.SectionName = TEXT("Default");
		Section.Link(Montage, 0.0f);
		return Montage;
	};
	// 无关槽位先播放，再连续执行 A/B：绑定和停止只能命中显式捕获的攻击实例。
	UAnimMontage* Unrelated = MakeMontage(TEXT("HitSlot"), TEXT("HitGroup"));
	UAnimMontage* Attack = MakeMontage(TEXT("AttackSlot"), TEXT("AttackGroup"));
	TestTrue(TEXT("Unrelated slot plays first"), Anim->Montage_Play(Unrelated, 1.0f, EMontagePlayReturnType::MontageLength, 0.0f, false) > 0.0f);
	if (!TestTrue(TEXT("Attack A starts through business entrypoint"), Combat->TryAttackTarget())) return false;
	const int64 ActionA = Combat->AttackSequence;
	if (!TestTrue(TEXT("The explicit attack command plays and binds A"), Combat->PlayAttackMontageForAttack(ActionA, Attack))) return false;
	const int32 PlaybackA = Combat->AttackPlayback.MontageInstanceId;
	TestTrue(TEXT("An earlier unrelated slot does not steal attack ownership"), Combat->AttackPlayback.Montage.Get() == Attack);
	TestNotNull(TEXT("Unrelated slot remains active"), Anim->GetActiveInstanceForMontage(Unrelated));
	Combat->ResetCombat();
	TestNotNull(TEXT("Reset only stops captured attack, not unrelated slot"), Anim->GetActiveInstanceForMontage(Unrelated));
	if (!TestTrue(TEXT("Attack B starts"), Combat->TryAttackTarget())) return false;
	const int64 ActionB = Combat->AttackSequence;
	TestFalse(TEXT("Late command A cannot play into B"), Combat->PlayAttackMontageForAttack(ActionA, Attack));
	if (!TestTrue(TEXT("Same Montage asset is explicitly replayed for B"), Combat->PlayAttackMontageForAttack(ActionB, Attack))) return false;
	TestNotEqual(TEXT("Real replay receives new instance identity"), Combat->AttackPlayback.MontageInstanceId, PlaybackA);
	TestFalse(TEXT("Another slot cannot overwrite bound playback"), Combat->BindAttackMontageForAttack(ActionB, Mesh, Unrelated));
	// 同一资产重播获得新实例 ID；旧排队通知与 Branching Point 都不能结束新攻击。
	FAnimNotifyEventReference OldNotify(nullptr, Attack);
	OldNotify.AddContextData<UE::Anim::FAnimNotifyMontageInstanceContext>(PlaybackA);
	FAnimNotifyEventReference CurrentNotify(nullptr, Attack);
	CurrentNotify.AddContextData<UE::Anim::FAnimNotifyMontageInstanceContext>(Combat->AttackPlayback.MontageInstanceId);
	UfpstrueAnimNotifyState_AttackWindow* Window = NewObject<UfpstrueAnimNotifyState_AttackWindow>(World);
	UfpstrueAnimNotify_AttackFinished* Finished = NewObject<UfpstrueAnimNotify_AttackFinished>(World);
	Window->NotifyBegin(Mesh, Attack, 0.5f, CurrentNotify);
	TestTrue(TEXT("Current real playback opens damage window"), Combat->AttackPhase == EFPEnemyAttackPhase::Active);
	Window->NotifyEnd(Mesh, Attack, OldNotify);
	Finished->Notify(Mesh, Attack, OldNotify);
	FBranchingPointNotifyPayload OldBranch(Mesh, Attack, nullptr, PlaybackA);
	Window->BranchingPointNotifyEnd(OldBranch);
	TestTrue(TEXT("Old queued and branching notifications cannot close B"), Combat->AttackPhase == EFPEnemyAttackPhase::Active);
	TestFalse(TEXT("A different mesh cannot supply the current attack identity"), Combat->IsAttackNotifyCurrent(Target->GetMesh(), CurrentNotify));
	Finished->Notify(Mesh, Attack, FAnimNotifyEventReference());
	TestTrue(TEXT("Native completion without playback identity cannot finish B"), Combat->IsAttacking());
	Finished->Notify(Mesh, Attack, CurrentNotify);
	TestFalse(TEXT("Current native completion finishes B"), Combat->IsAttacking());
	Combat->LastAttackTime -= Combat->AttackInterval;
	TestTrue(TEXT("Attack C starts after cooldown"), Combat->TryAttackTarget());
	TestTrue(TEXT("Attack C binds playback"), Combat->PlayAttackMontageForAttack(Combat->AttackSequence, Attack));
	FAnimMontageInstance* Instance = Anim->GetActiveInstanceForMontage(Attack);
	if (!TestNotNull(TEXT("Stop reentry uses engine playback"), Instance)) return false;
	int32 StopCallbacks = 0;
	bool bRestarted = false;
	// 在真实动画停止广播中尝试重建目标与攻击，整局 StopAI 的命令门必须仍保持关闭。
	Instance->OnMontageBlendingOutStarted.BindLambda([&](UAnimMontage*, bool)
	{
		++StopCallbacks;
		Controller->InitializeCombatContext(Target, nullptr);
		bRestarted |= Combat->TryAttackTarget();
	});
	Controller->StopAI();
	TestEqual(TEXT("Real Montage stop dispatches the reentry callback"), StopCallbacks, 1);
	TestFalse(TEXT("Stop callback cannot start a replacement attack"), bRestarted);
	TestFalse(TEXT("Whole AI stop leaves no attack"), Combat->IsAttacking());
	TestFalse(TEXT("Stopped controller keeps command gate closed"), Controller->AcceptsCombatCommands());
	TestNull(TEXT("Stop callback cannot re-inject the target"), Controller->GetTargetCharacter());
	Instance->OnMontageBlendingOutStarted.Unbind();

	// 没有 AttackFinished Notify 的动画也通过真实实例自然结束收尾，而不是等待保护 Timer。
	Controller->InitializeCombatContext(Target, nullptr);
	if (!ReportVisibleCombatTarget(Controller, Target, *this)) return false;
	TestTrue(TEXT("A new session accepts the next attack"), Combat->TryAttackTarget());
	TestTrue(TEXT("Natural-completion attack plays"), Combat->PlayAttackMontageForAttack(Combat->AttackSequence, Attack));
	FAnimMontageInstance* NaturalPlayback = Anim->GetActiveInstanceForMontage(Attack);
	if (!TestNotNull(TEXT("Natural playback instance"), NaturalPlayback)) return false;
	for (int32 Step = 0; Step < 30 && Combat->IsAttacking(); ++Step)
	{
		// 引擎推进实际 Montage 权重/时间线并调用 Terminate；未手动执行业务完成委托或伪造播放身份。
		NaturalPlayback->UpdateWeight(0.1f);
		NaturalPlayback->Advance(0.1f, nullptr, false);
	}
	TestFalse(TEXT("Natural Montage completion closes the attack without a finish Notify"), Combat->IsAttacking());
	TestFalse(TEXT("Natural completion clears failsafe timer"), World->GetTimerManager().IsTimerActive(Combat->AttackFinishTimerHandle));
	TestFalse(TEXT("Natural completion applies normal attack cooldown"), Combat->CanStartAttack());
	Combat->LastAttackTime -= Combat->AttackInterval;
	TestTrue(TEXT("Explicit external-play binding starts a transaction"), Combat->TryAttackTarget());
	const int64 BoundAction = Combat->AttackSequence;
	TestTrue(TEXT("External explicit Montage playback succeeds"), Anim->Montage_Play(Attack, 1.0f, EMontagePlayReturnType::MontageLength, 0.0f, false) > 0.0f);
	FAnimMontageInstance* InterruptedPlayback = Anim->GetActiveInstanceForMontage(Attack);
	if (!TestNotNull(TEXT("Interrupted playback instance"), InterruptedPlayback)) return false;
	bool bObserverSawCleanState = false;
	// 外部已有动画结束观察者应在战斗清理完成后执行，且取消不能计入正常结束冷却。
	InterruptedPlayback->OnMontageEnded.BindLambda([&](UAnimMontage*, bool bInterrupted)
	{
		bObserverSawCleanState = bInterrupted && !Combat->IsAttacking();
	});
	TestTrue(TEXT("Explicit binding installs the same owned completion handler"), Combat->BindAttackMontageForAttack(BoundAction, Mesh, Attack));
	TestTrue(TEXT("Duplicate explicit bind remains idempotent"), Combat->BindAttackMontageForAttack(BoundAction, Mesh, Attack));
	const float CooldownBeforeInterrupt = Combat->LastAttackTime;
	Anim->Montage_Stop(0.0f, Attack);
	InterruptedPlayback->UpdateWeight(0.1f);
	InterruptedPlayback->Advance(0.1f, nullptr, false);
	TestFalse(TEXT("Real Montage interruption cancels the attack"), Combat->IsAttacking());
	TestEqual(TEXT("Interruption does not consume normal-finish cooldown"), Combat->LastAttackTime, CooldownBeforeInterrupt);
	TestTrue(TEXT("An existing external observer runs only after owned cleanup"), bObserverSawCleanState);
	// 空资产是明确播放失败；不能让攻击事务和保护 Timer 占位到超时。
	TestTrue(TEXT("Failed-playback transaction starts"), Combat->TryAttackTarget());
	TestFalse(TEXT("Missing Montage reports playback failure"), Combat->PlayAttackMontageForAttack(Combat->AttackSequence, nullptr));
	TestFalse(TEXT("Playback failure releases the transaction immediately"), Combat->IsAttacking());
	TestFalse(TEXT("Playback failure clears the failsafe timer"), World->GetTimerManager().IsTimerActive(Combat->AttackFinishTimerHandle));
	TestTrue(TEXT("Playback failure remains retryable"), Combat->CanStartAttack());
	// 慢速播放的保护时间取实际时长；外部绑定则使用剩余时长，避免固定超时提前打断。
	TestTrue(TEXT("Slow attack starts"), Combat->TryAttackTarget());
	TestTrue(TEXT("A legal 0.1x attack plays"), Combat->PlayAttackMontageForAttack(Combat->AttackSequence, Attack, 0.1f));
	FAnimMontageInstance* SlowPlayback = Anim->GetActiveInstanceForMontage(Attack);
	if (!TestNotNull(TEXT("Slow engine playback instance"), SlowPlayback)) return false;
	TestTrue(TEXT("Failsafe covers actual 20-second playback rather than fixed five seconds"),
		World->GetTimerManager().GetTimerRate(Combat->AttackFinishTimerHandle) >= 20.0f);
	// 暂停夹具物理/自动动画，只推进真实世界 Timer；不能靠修改 Timer 状态假装跨过旧超时点。
	Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
	Target->GetCharacterMovement()->SetComponentTickEnabled(false);
	Mesh->SetComponentTickEnabled(false);
	if (!Fixture.TickTestWorld(0.0f) || !Fixture.TickTestWorld(5.5f)) { Fixture.ForwardErrorMessages(this); return false; }
	TestTrue(TEXT("The slow attack survives beyond the previous 5.1-second timeout"), Combat->IsAttacking());
	for (int32 Step = 0; Step < 230 && Combat->IsAttacking(); ++Step)
	{
		SlowPlayback->UpdateWeight(0.1f);
		SlowPlayback->Advance(0.1f, nullptr, false);
	}
	TestFalse(TEXT("Slow playback still finishes through its engine completion"), Combat->IsAttacking());
	Combat->LastAttackTime -= Combat->AttackInterval;
	TestTrue(TEXT("External slow binding transaction starts"), Combat->TryAttackTarget());
	TestTrue(TEXT("External 0.1x play succeeds"), Anim->Montage_Play(Attack, 0.1f, EMontagePlayReturnType::MontageLength, 0.0f, true) > 0.0f);
	Anim->Montage_SetPosition(Attack, 0.5f);
	TestTrue(TEXT("External slow play binds"), Combat->BindAttackMontageForAttack(Combat->AttackSequence, Mesh, Attack));
	const float RemainingBudget = World->GetTimerManager().GetTimerRate(Combat->AttackFinishTimerHandle);
	TestTrue(TEXT("Explicit binding budgets remaining 15 seconds, not the whole 20 or fixed five"), RemainingBudget >= 15.0f && RemainingBudget < 16.0f);
	Combat->ResetCombat();
	return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueEnemyAttackAssetIntegrationTest,
	"fpstrue.Gameplay.Combat.ProjectBlueprintPlaybackIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 加载项目真实 enemy_BP/AnimBP/Montage，检查播放桥和结束清理；仍不等价于完整姿态与命中画面验收。
bool FFpstrueEnemyAttackAssetIntegrationTest::RunTest(const FString& Parameters)
{
	// 必需资产缺失必须失败；本测试不以原生空 Montage 或默认类替代真实项目蓝图。
	UClass* EnemyClass = LoadClass<AfpstrueEnemyCharacter>(nullptr,
		TEXT("/Game/FirstPerson/Blueprints/enemy/enemy_BP.enemy_BP_C"));
	if (!TestNotNull(TEXT("Required project enemy Blueprint class loads"), EnemyClass)) return false;
	TArray<UAnimMontage*> ProjectAttacks;
	for (const TCHAR* Name : {TEXT("enenmyattack1"), TEXT("enemyattack2"), TEXT("enemyattack3"), TEXT("enemyattack4")})
	{
		const FString AssetPath = FString::Printf(TEXT("/Game/EnemyWarriorAnimPack/Animations/InPlace/Attacks/%s.%s"), Name, Name);
		UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *AssetPath);
		if (!TestNotNull(FString::Printf(TEXT("Required project attack loads: %s"), Name), Montage)) return false;
		ProjectAttacks.Add(Montage);
	}
	FTestWorldWrapper Fixture;
	if (!BeginCombatTestWorld(Fixture, *this, true)) return false;
	UWorld* World = Fixture.GetTestWorld();
	AfpstrueCharacter* Target = World->SpawnActor<AfpstrueCharacter>();
	if (!TestNotNull(TEXT("Test target"), Target)) return false;
	Target->SetActorEnableCollision(false);
	Target->SetActorLocation(FVector(180.0, 0.0, 0.0));
	// 分别用新实例测试自然结束和中断，不改事务状态/播放 ID，也无需跳过冷却。
	for (const bool bInterrupt : {false, true})
	{
		AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(EnemyClass,
			FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!TestNotNull(TEXT("Real project Blueprint spawns"), Enemy)) return false;
		Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
		Enemy->FinishSpawning(FTransform::Identity);
		Enemy->SetActorEnableCollision(false);
		AfpstrueEnemyAIController* Controller = World->SpawnActor<AfpstrueEnemyAIController>();
		if (!TestNotNull(TEXT("Controller"), Controller)) return false;
		Controller->Possess(Enemy);
		Controller->InitializeCombatContext(Target, nullptr);
		if (!ReportVisibleCombatTarget(Controller, Target, *this)) return false;
		DisableAutomaticBehaviorTick(*Controller->GetBrainComponent());
		UfpstrueEnemyCombatComponent* Combat = Enemy->GetCombatComponent();
		USkeletalMeshComponent* Mesh = Enemy->GetMesh();
		UAnimInstance* ProjectAnim = Mesh->GetAnimInstance();
		if (!TestNotNull(TEXT("Project SkeletalMesh retained"), Mesh->GetSkeletalMeshAsset()) ||
			!TestNotNull(TEXT("Project AnimBlueprint initialized"), ProjectAnim)) return false;
		TestTrue(TEXT("Animation is the configured Blueprint instance, not a synthetic UAnimInstance"),
			ProjectAnim->GetClass()->ClassGeneratedBy != nullptr && ProjectAnim->GetClass() == Mesh->GetAnimClass());
		const float InitialCooldown = Combat->LastAttackTime;
		// 唯一业务输入：由 enemy_BP 的新事件自己选择资产并调用 PlayAttackMontageForAttack。
		if (!TestTrue(TEXT("TryAttackTarget executes the real project Blueprint playback bridge"), Combat->TryAttackTarget())) return false;
		if (!TestTrue(TEXT("Blueprint bound an owned playback synchronously"), Combat->AttackPlayback.IsSet())) return false;
		UAnimMontage* SelectedMontage = Combat->AttackPlayback.Montage.Get();
		TestTrue(TEXT("Blueprint selected one of the four real attack Montages"), ProjectAttacks.Contains(SelectedMontage));
		TestTrue(TEXT("Owned playback uses the project mesh and its original AnimInstance"),
			Combat->AttackPlayback.Mesh.Get() == Mesh && Combat->AttackPlayback.AnimInstance.Get() == ProjectAnim);
		FAnimMontageInstance* Playback = ProjectAnim->GetMontageInstanceForID(Combat->AttackPlayback.MontageInstanceId);
		if (!TestNotNull(TEXT("The engine allocated the bound project Montage instance"), Playback)) return false;
		TestTrue(TEXT("Bound instance is actually playing the selected project asset"), Playback->IsPlaying() && Playback->Montage == SelectedMontage);
		// 分别让引擎自然推进或中断实际实例，验证无需保护 Timer 触发也能完成事务清理。
		if (bInterrupt) ProjectAnim->Montage_Stop(0.0f, SelectedMontage);
		const int32 MaxSteps = FMath::CeilToInt((SelectedMontage->GetPlayLength() + 2.0f) / 0.05f);
		for (int32 Step = 0; Step < MaxSteps && Combat->IsAttacking(); ++Step)
		{
			Playback->UpdateWeight(0.05f);
			Playback->Advance(0.05f, nullptr, false);
		}
		TestFalse(bInterrupt ? TEXT("Project Montage interruption clears attack") : TEXT("Project Montage naturally completes attack"), Combat->IsAttacking());
		TestFalse(TEXT("Project completion releases the failsafe timer without ticking that timer"), World->GetTimerManager().IsTimerActive(Combat->AttackFinishTimerHandle));
		TestFalse(TEXT("Project completion releases playback ownership"), Combat->AttackPlayback.IsSet());
		if (bInterrupt) TestEqual(TEXT("Interruption preserves prior cooldown"), Combat->LastAttackTime, InitialCooldown);
		else TestFalse(TEXT("Natural completion applies configured attack cooldown"), Combat->CanStartAttack());
		Controller->StopAI();
		Enemy->Destroy();
		Controller->Destroy();
	}
	return true;
}
#endif // WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFPEnemyNativeFloorCacheTest,
	"fpstrue.AI.Movement.NativeStationaryFloorCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 运行真实 CMC 查询和 Walking，不自建缓存或用降频代替正确性；标记仅证明是否复用了原查询结果。
bool FFPEnemyNativeFloorCacheTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Native enemy defaults permit the engine stationary floor cache"),
		GetDefault<AfpstrueEnemyCharacter>()->GetCharacterMovement()->bAlwaysCheckFloor);
	FTestWorldWrapper Fixture;
	if (!Fixture.CreateTestWorld(EWorldType::Game)) { Fixture.ForwardErrorMessages(this); return false; }
	UWorld* World = Fixture.GetTestWorld();
	World->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!Fixture.BeginPlayInTestWorld()) { Fixture.ForwardErrorMessages(this); return false; }
	// UE5.5 StartFalling在Editor中保护世界最初1秒，避免PIE首帧场景未加载时跌落。
	// 真实推进World越过保护期，不修改TimeSeconds/GIsEditor或伪造MOVE_Falling。
	for (int32 Frame = 0; Frame < 12; ++Frame)
		if (!Fixture.TickTestWorld(0.1f)) { Fixture.ForwardErrorMessages(this); return false; }
	if (!TestTrue(TEXT("Real world ticking passes the native editor startup falling guard"),
		World->HasBegunPlay() && World->GetTimeSeconds() >= 1.0f)) return false;
	AActor* FloorActor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Native floor cache has a real floor actor"), FloorActor)) return false;
	UBoxComponent* Floor = NewObject<UBoxComponent>(FloorActor);
	FloorActor->AddInstanceComponent(Floor);
	FloorActor->SetRootComponent(Floor);
	Floor->SetMobility(EComponentMobility::Static);
	Floor->SetBoxExtent(FVector(1000.0f, 1000.0f, 20.0f));
	Floor->SetCollisionObjectType(ECC_WorldStatic);
	Floor->SetCollisionResponseToAllChannels(ECR_Block);
	Floor->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Floor->SetWorldLocation(FVector(0.0f, 0.0f, -20.0f));
	Floor->RegisterComponent();
	const FTransform SpawnTransform(FVector(0.0f, 0.0f, 98.0f));
	AfpstrueEnemyCharacter* Enemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
		AfpstrueEnemyCharacter::StaticClass(), SpawnTransform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Floor cache fixture has a real enemy"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement();
	// 原生配置是可覆盖的默认值，不在 BeginPlay 强压资产；保留特殊关卡的连续地面检测资格。
	Movement->bAlwaysCheckFloor = true;
	Enemy->FinishSpawning(SpawnTransform);
	TestTrue(TEXT("An explicitly authored Always Check Floor option survives BeginPlay"), Movement->bAlwaysCheckFloor);
	Movement->bAlwaysCheckFloor = false;
	Movement->bRunPhysicsWithNoController = true;
	Movement->bUseRVOAvoidance = false;
	Movement->SetMovementMode(MOVE_Walking);
	Movement->SetComponentTickEnabled(false); // 下方显式调用公开 TickComponent，避免测试世界额外推进一帧。
	const FVector CacheMarker(123456.0f, 654321.0f, 777777.0f);
	const auto SeedStaticFloor = [&]()
	{
		FFindFloorResult FoundFloor;
		Movement->FindFloor(Enemy->GetActorLocation(), FoundFloor, false);
		if (!TestTrue(TEXT("The physical box is a walkable floor"), FoundFloor.IsWalkableFloor())) return false;
		Movement->CurrentFloor = FoundFloor;
		Enemy->SetBase(Floor);
		Movement->CurrentFloor.HitResult.TraceStart = CacheMarker;
		Movement->bForceNextFloorCheck = false;
		Movement->bJustTeleported = false;
		return true;
	};
	FFindFloorResult Result;
	if (!SeedStaticFloor()) return false;
	Movement->FindFloor(Enemy->GetActorLocation(), Result, true);
	TestTrue(TEXT("Stationary valid static floor reuses the native query result"),
		Result.IsWalkableFloor() && Result.HitResult.TraceStart == CacheMarker);
	Movement->FindFloor(Enemy->GetActorLocation(), Result, false);
	TestTrue(TEXT("A changed location request bypasses the cached query"),
		Result.IsWalkableFloor() && Result.HitResult.TraceStart != CacheMarker);
	if (!SeedStaticFloor()) return false;
	Movement->bForceNextFloorCheck = true;
	Movement->FindFloor(Enemy->GetActorLocation(), Result, true);
	TestTrue(TEXT("Explicit floor invalidation forces a real query"),
		Result.IsWalkableFloor() && Result.HitResult.TraceStart != CacheMarker);
	if (!SeedStaticFloor()) return false;
	Movement->bJustTeleported = true;
	Movement->FindFloor(Enemy->GetActorLocation(), Result, true);
	TestTrue(TEXT("Teleport invalidation forces a real query"),
		Result.IsWalkableFloor() && Result.HitResult.TraceStart != CacheMarker);
	if (!SeedStaticFloor()) return false;
	Movement->bAlwaysCheckFloor = true;
	Movement->FindFloor(Enemy->GetActorLocation(), Result, true);
	TestTrue(TEXT("The native designer opt-out restores continuous queries"),
		Result.IsWalkableFloor() && Result.HitResult.TraceStart != CacheMarker);
	Movement->bAlwaysCheckFloor = false;
	if (!SeedStaticFloor()) return false;
	Floor->SetMobility(EComponentMobility::Movable);
	Movement->FindFloor(Enemy->GetActorLocation(), Result, true);
	TestTrue(TEXT("A movable base is never treated as a reusable static floor"),
		Result.IsWalkableFloor() && Result.HitResult.TraceStart != CacheMarker);
	Floor->SetMobility(EComponentMobility::Static);
	if (!SeedStaticFloor()) return false;
	const double BeforeMoveX = Enemy->GetActorLocation().X;
	Movement->Velocity = FVector(200.0f, 0.0f, 0.0f);
	Movement->TickComponent(0.02f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("Actual native Walking still advances the actor"), Enemy->GetActorLocation().X > BeforeMoveX);
	TestTrue(TEXT("Actual movement refreshes the floor rather than carrying the stationary result"),
		Movement->CurrentFloor.IsWalkableFloor() && Movement->CurrentFloor.HitResult.TraceStart != CacheMarker);
	Movement->StopMovementImmediately();
	if (!SeedStaticFloor()) return false;
	Floor->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Movement->FindFloor(Enemy->GetActorLocation(), Result, true);
	TestFalse(TEXT("Disabling the current floor collision invalidates the cache"), Result.IsWalkableFloor());
	Floor->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	if (!SeedStaticFloor()) return false;
	Floor->UnregisterComponent();
	Movement->FindFloor(Enemy->GetActorLocation(), Result, true);
	TestFalse(TEXT("Unregistering a static floor invalidates the cache while its actor stays alive"), Result.IsWalkableFloor());
	Floor->RegisterComponent();
	if (!SeedStaticFloor()) return false;
	Floor->DestroyPhysicsState();
	Movement->FindFloor(Enemy->GetActorLocation(), Result, true);
	TestFalse(TEXT("Retiring only the static floor physics state invalidates the cache"), Result.IsWalkableFloor());
	Floor->RecreatePhysicsState();
	if (!SeedStaticFloor()) return false;
	Floor->DestroyComponent();
	Movement->FindFloor(Enemy->GetActorLocation(), Result, true);
	TestFalse(TEXT("Removing the floor component invalidates the cache"), Result.IsWalkableFloor());
	Movement->TickComponent(0.02f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("Walking without its removed support transitions to Falling"), Movement->IsFalling());
	// 生产资产必须实际继承新默认值；若保存了旧 true，只报告配置差异，不在运行时覆盖作者选择。
	UClass* EnemyBlueprintClass = LoadClass<AfpstrueEnemyCharacter>(nullptr,
		TEXT("/Game/FirstPerson/Blueprints/enemy/enemy_BP.enemy_BP_C"));
	if (!TestNotNull(TEXT("The production enemy Blueprint loads for the floor policy check"), EnemyBlueprintClass)) return false;
	TestFalse(TEXT("The production enemy Blueprint default permits stationary floor reuse"),
		EnemyBlueprintClass->GetDefaultObject<AfpstrueEnemyCharacter>()->GetCharacterMovement()->bAlwaysCheckFloor);
	const FTransform BlueprintSpawn(FVector(4000.0f, 0.0f, 98.0f));
	AfpstrueEnemyCharacter* BlueprintEnemy = World->SpawnActorDeferred<AfpstrueEnemyCharacter>(
		EnemyBlueprintClass, BlueprintSpawn, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("The production enemy Blueprint spawns with its authored movement policy"), BlueprintEnemy)) return false;
	BlueprintEnemy->AutoPossessAI = EAutoPossessAI::Disabled;
	BlueprintEnemy->FinishSpawning(BlueprintSpawn);
	TestFalse(TEXT("The production enemy runtime keeps the stationary floor cache enabled"),
		BlueprintEnemy->GetCharacterMovement()->bAlwaysCheckFloor);
	TestTrue(TEXT("The production enemy runtime uses the lifecycle-safe native floor query"),
		BlueprintEnemy->GetCharacterMovement()->IsA<UfpstrueEnemyMovementComponent>());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFpstrueAnimationSharingStopTest,
	"fpstrue.Performance.AnimationSharing.StopAndSwapLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// 验证真实插件注销的 RemoveAtSwap 句柄更新、停止重入与待注册取消，不覆盖动画质量或收益。
bool FFpstrueAnimationSharingStopTest::RunTest(const FString& Parameters)
{
	// 使用临时世界与插件真实的 UnregisterActor；只省略依赖动画资产的注册 Setup。
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game))
	{
		World.ForwardErrorMessages(this);
		return false;
	}
	World.GetTestWorld()->GetWorldSettings()->DefaultGameMode = AGameModeBase::StaticClass();
	if (!World.BeginPlayInTestWorld())
	{
		World.ForwardErrorMessages(this);
		return false;
	}
	if (!TestTrue(TEXT("The plugin is enabled for the unregister regression"), UAnimationSharingManager::AnimationSharingEnabled()))
	{
		return false;
	}

	AActor* Owner = World.GetTestWorld()->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("A temporary coordinator owner exists"), Owner))
	{
		return false;
	}
	UfpstrueEnemyAnimationSharingCoordinator* Coordinator = NewObject<UfpstrueEnemyAnimationSharingCoordinator>(Owner);
	Owner->AddInstanceComponent(Coordinator);
	Coordinator->RegisterComponent();
	UAnimationSharingManager* Manager = NewObject<UAnimationSharingManager>(World.GetTestWorld());
	Coordinator->SharingManager = Manager;

	// PerSkeletonData 是插件现有反射属性。通过它建立最小状态，避免测试专用插件子类、
	// 私有访问宏或未导出的 UAnimSharingInstance::StaticClass 链接依赖。
	const FArrayProperty* SkeletonDataProperty = FindFProperty<FArrayProperty>(Manager->GetClass(), TEXT("PerSkeletonData"));
	const FObjectPropertyBase* SkeletonEntryProperty = SkeletonDataProperty != nullptr
		? CastField<FObjectPropertyBase>(SkeletonDataProperty->Inner) : nullptr;
	if (!TestNotNull(TEXT("The plugin exposes its reflected skeleton data array"), SkeletonEntryProperty))
	{
		return false;
	}
	UAnimSharingInstance* Data = static_cast<UAnimSharingInstance*>(NewObject<UObject>(Manager, SkeletonEntryProperty->PropertyClass));
	SkeletonDataProperty->ContainerPtrToValuePtr<TArray<TObjectPtr<UAnimSharingInstance>>>(Manager)->Add(Data);

	USkeletalMeshComponent* Leader = NewObject<USkeletalMeshComponent>(Owner);
	Owner->AddInstanceComponent(Leader);
	Leader->RegisterComponent();
	TArray<AfpstrueEnemyCharacter*> Enemies;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		AfpstrueEnemyCharacter* Enemy = World.GetTestWorld()->SpawnActorDeferred<AfpstrueEnemyCharacter>(
			AfpstrueEnemyCharacter::StaticClass(), FTransform::Identity, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!TestNotNull(TEXT("A native follower exists"), Enemy))
		{
			return false;
		}
		Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
		Enemy->FinishSpawning(FTransform::Identity);
		Enemies.Add(Enemy);
	}

	// 建立三个最小共享跟随者，保留插件句柄更新委托，以真实 UnregisterActor 检验交换行为。
	const auto SeedFollowers = [&]()
	{
		Coordinator->bRunning = true;
		for (int32 Index = 0; Index < Enemies.Num(); ++Index)
		{
			AfpstrueEnemyCharacter* Enemy = Enemies[Index];
			const TWeakObjectPtr<AfpstrueEnemyCharacter> EnemyKey(Enemy);
			USkeletalMeshComponent* Mesh = Enemy->GetMesh();
			Mesh->SetLeaderPoseComponent(Leader, true);
			Mesh->SetComponentTickEnabled(false);
			Mesh->PrimaryComponentTick.bCanEverTick = false;
			Mesh->bIgnoreLeaderPoseComponentLOD = true;
			Data->RegisteredActors.Add(Enemy);
			FPerActorData& ActorData = Data->PerActorData.AddZeroed_GetRef();
			ActorData.ComponentIndices.Add(Index);
			ActorData.UpdateActorHandleDelegate = FUpdateActorHandle::CreateUObject(
				Coordinator, &UfpstrueEnemyAnimationSharingCoordinator::HandleActorHandleUpdated, EnemyKey);
			FPerComponentData& ComponentData = Data->PerComponentData.AddZeroed_GetRef();
			ComponentData.ActorIndex = Index;
			ComponentData.Component = Mesh;
			// Skeleton 0 的 ActorHandle 等于 ActorIndex，真实注销会在交换时重新计算它。
			Coordinator->RegisteredActorHandles.Add(EnemyKey, static_cast<uint32>(Index));
		}
	};

	// 整体停止先关闭新注册，随后每个跟随者都应恢复独立 Tick、LOD 与 Leader 关系。
	SeedFollowers();
	Coordinator->Stop();
	TestFalse(TEXT("Stop disables new registrations"), Coordinator->bRunning);
	TestEqual(TEXT("Stop removes every plugin actor"), Data->RegisteredActors.Num(), 0);
	TestEqual(TEXT("Stop removes every plugin component"), Data->PerComponentData.Num(), 0);
	TestEqual(TEXT("Stop removes every plugin actor record"), Data->PerActorData.Num(), 0);
	TestEqual(TEXT("Stop removes every coordinator handle"), Coordinator->RegisteredActorHandles.Num(), 0);
	TestEqual(TEXT("Stop removes every pending request"), Coordinator->PendingActorRegistrations.Num(), 0);
	for (AfpstrueEnemyCharacter* Enemy : Enemies)
	{
		// 旧实现会在 Stop 的 swap 回调内递归注销尾对象，跳过该对象的 LOD 标志恢复。
		TestFalse(TEXT("Every follower regains its own LOD control"), Enemy->GetMesh()->bIgnoreLeaderPoseComponentLOD);
		TestFalse(TEXT("Every follower is detached from its leader"), Enemy->GetMesh()->LeaderPoseComponent.IsValid());
		TestTrue(TEXT("Every follower can tick independently"), Enemy->GetMesh()->PrimaryComponentTick.bCanEverTick);
	}
	Coordinator->HandleActorHandleUpdated(42, TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemies[0]));
	TestEqual(TEXT("An unknown callback cannot revive a stopped registration"), Coordinator->RegisteredActorHandles.Num(), 0);

	// 正常运行时移除首对象，插件尾对象换到首槽；后续 Significance 必须命中那个对象。
	SeedFollowers();
	// 原生敌人尚未获得低档渲染策略，不具备共享资格；Refresh 应复用完整注销路径。
	Coordinator->RefreshEnemyRegistration(Enemies[0]);
	const uint32* SwappedHandle = Coordinator->RegisteredActorHandles.Find(TWeakObjectPtr<AfpstrueEnemyCharacter>(Enemies[2]));
	if (!TestNotNull(TEXT("The swapped follower keeps its registration"), SwappedHandle))
	{
		Coordinator->Stop();
		return false;
	}
	TestEqual(TEXT("The tail follower receives the first actor handle"), *SwappedHandle, uint32(0));
	TestEqual(TEXT("The actual plugin swaps the tail actor into the first slot"), Data->RegisteredActors[0].Get(), static_cast<AActor*>(Enemies[2]));
	Manager->UpdateSignificanceForActorHandle(*SwappedHandle, 0.75f);
	TestEqual(TEXT("The updated handle addresses the swapped actor"), Data->PerActorData[0].SignificanceValue, 0.75f);
	TestEqual(TEXT("The other follower significance is unchanged"), Data->PerActorData[1].SignificanceValue, 0.0f);
	Coordinator->SuspendEnemy(Enemies[0]);
	TestEqual(TEXT("Repeated Suspend does not remove another follower"), Data->RegisteredActors.Num(), 2);
	// 未登记对象重复刷新不应碰插件数据，也不能重置不属于共享层的 LOD 标志。
	Enemies[0]->GetMesh()->bIgnoreLeaderPoseComponentLOD = true;
	Coordinator->RefreshEnemyRegistration(Enemies[0]);
	TestEqual(TEXT("Unregistered Refresh does not remove another follower"), Data->RegisteredActors.Num(), 2);
	TestTrue(TEXT("Unregistered Refresh preserves unrelated LOD flags"), Enemies[0]->GetMesh()->bIgnoreLeaderPoseComponentLOD);

	// 失败或中止的待注册请求也必须能退出，并归还可能已经接管的 LOD 标志。
	const TWeakObjectPtr<AfpstrueEnemyCharacter> PendingEnemy(Enemies[0]);
	Coordinator->PendingActorRegistrations.Add(PendingEnemy);
	Enemies[0]->GetMesh()->bIgnoreLeaderPoseComponentLOD = true;
	Coordinator->SuspendEnemy(Enemies[0]);
	TestFalse(TEXT("Suspend clears a pending request"), Coordinator->PendingActorRegistrations.Contains(PendingEnemy));
	TestFalse(TEXT("Suspend restores pending follower LOD control"), Enemies[0]->GetMesh()->bIgnoreLeaderPoseComponentLOD);
	Coordinator->Stop();
	Coordinator->Stop();
	TestEqual(TEXT("Repeated Stop leaves the plugin empty"), Data->RegisteredActors.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
